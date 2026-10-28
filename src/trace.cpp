#include "cachescope/trace.hpp"

#include <cstring>
#include <stdexcept>

namespace cachescope {
namespace {

constexpr std::size_t kInitialBuffer = 1 << 20;  // 1 MiB

inline bool is_space(char c) { return c == ' ' || c == '\t'; }

inline void skip_spaces(const char*& p, const char* end) {
  while (p < end && is_space(*p)) ++p;
}

// Hex digit values indexed by byte, with -1 for everything else. A table lookup
// replaces three range comparisons per character, which matters: parsing is
// roughly a third of total runtime on a multi-million-record trace.
struct HexTable {
  signed char v[256];
  constexpr HexTable() : v() {
    for (int i = 0; i < 256; ++i) v[i] = -1;
    for (int i = '0'; i <= '9'; ++i) v[i] = static_cast<signed char>(i - '0');
    for (int i = 'a'; i <= 'f'; ++i) v[i] = static_cast<signed char>(i - 'a' + 10);
    for (int i = 'A'; i <= 'F'; ++i) v[i] = static_cast<signed char>(i - 'A' + 10);
  }
};
constexpr HexTable kHex{};

// Parses an optionally 0x-prefixed hex number. Returns false if no digits.
inline bool parse_hex(const char*& p, const char* end, Addr& out) {
  if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
  Addr v = 0;
  const char* start = p;
  while (p < end) {
    const signed char d = kHex.v[static_cast<unsigned char>(*p)];
    if (d < 0) break;
    v = (v << 4) | static_cast<Addr>(d);
    ++p;
  }
  if (p == start) return false;
  out = v;
  return true;
}

inline bool parse_dec(const char*& p, const char* end, std::uint64_t& out) {
  std::uint64_t v = 0;
  bool any = false;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    any = true;
    ++p;
  }
  if (any) out = v;
  return any;
}

std::string snippet(const char* begin, std::size_t len) {
  const std::size_t n = len > 64 ? 64 : len;
  return std::string(begin, n);
}

}  // namespace

// ---------------------------------------------------------------- LineReader

LineReader::LineReader(const std::string& path) : buf_(kInitialBuffer) {
  if (path == "-") {
    fp_ = stdin;
    owns_ = false;
  } else {
    fp_ = std::fopen(path.c_str(), "rb");
    owns_ = true;
    if (fp_ == nullptr) throw std::runtime_error("cannot open trace file: " + path);
  }
}

LineReader::~LineReader() {
  if (owns_ && fp_ != nullptr) std::fclose(fp_);
}

bool LineReader::refill() {
  // Slide the unconsumed tail to the front, then top the buffer up.
  if (head_ > 0) {
    std::memmove(buf_.data(), buf_.data() + head_, tail_ - head_);
    tail_ -= head_;
    head_ = 0;
  }
  if (tail_ == buf_.size()) {
    // A single line longer than the whole buffer; grow rather than truncate.
    buf_.resize(buf_.size() * 2);
  }
  const std::size_t got = std::fread(buf_.data() + tail_, 1, buf_.size() - tail_, fp_);
  if (got == 0) {
    eof_ = true;
    return false;
  }
  tail_ += got;
  bytes_read_ += got;
  return true;
}

bool LineReader::next_line(const char*& begin, std::size_t& len) {
  for (;;) {
    if (head_ < tail_) {
      const char* start = buf_.data() + head_;
      const void* nl = std::memchr(start, '\n', tail_ - head_);
      if (nl != nullptr) {
        std::size_t n = static_cast<std::size_t>(static_cast<const char*>(nl) - start);
        head_ += n + 1;
        // Tolerate CRLF so traces captured on Windows parse unchanged.
        if (n > 0 && start[n - 1] == '\r') --n;
        begin = start;
        len = n;
        return true;
      }
    }
    if (eof_) {
      // Trailing line with no terminator.
      if (head_ < tail_) {
        begin = buf_.data() + head_;
        len = tail_ - head_;
        head_ = tail_;
        if (len > 0 && begin[len - 1] == '\r') --len;
        return true;
      }
      return false;
    }
    refill();
  }
}

// ------------------------------------------------------------ MemTraceReader

MemTraceReader::MemTraceReader(const std::string& path, ThirdField third_field)
    : reader_(path), third_field_(third_field) {}

bool MemTraceReader::next(MemRef& out) {
  const char* begin = nullptr;
  std::size_t len = 0;
  while (reader_.next_line(begin, len)) {
    const char* p = begin;
    const char* end = begin + len;

    skip_spaces(p, end);
    if (p == end || *p == '#') continue;  // blank line or comment

    AccessType type;
    switch (*p) {
      case 'l': case 'L': case 'r': case 'R': type = AccessType::Load; break;
      case 's': case 'S': case 'w': case 'W': type = AccessType::Store; break;
      default:
        ++malformed_;
        if (first_error_.empty()) {
          first_error_ = "unrecognised operation in line: " + snippet(begin, len);
        }
        continue;
    }
    ++p;

    skip_spaces(p, end);
    Addr addr = 0;
    if (!parse_hex(p, end, addr)) {
      ++malformed_;
      if (first_error_.empty()) {
        first_error_ = "missing or invalid address in line: " + snippet(begin, len);
      }
      continue;
    }

    std::uint64_t third = 0;
    skip_spaces(p, end);
    const bool have_third = parse_dec(p, end, third);

    out.type = type;
    out.addr = addr;
    switch (third_field_) {
      case ThirdField::Size:
        // A zero is meaningless as a byte count; treat it as a single byte so
        // the access still touches exactly one line.
        out.size = have_third && third > 0 ? static_cast<std::uint32_t>(third) : 1u;
        out.instr_gap = 0;
        break;
      case ThirdField::InstrGap:
        out.size = 1;
        out.instr_gap = have_third ? static_cast<std::uint32_t>(third) : 0u;
        break;
      case ThirdField::Ignore:
        out.size = 1;
        out.instr_gap = 0;
        break;
    }

    ++records_;
    return true;
  }
  return false;
}

// --------------------------------------------------------- BranchTraceReader

BranchTraceReader::BranchTraceReader(const std::string& path) : reader_(path) {}

bool BranchTraceReader::next(BranchRef& out) {
  const char* begin = nullptr;
  std::size_t len = 0;
  while (reader_.next_line(begin, len)) {
    const char* p = begin;
    const char* end = begin + len;

    skip_spaces(p, end);
    if (p == end || *p == '#') continue;

    Addr pc = 0;
    if (!parse_hex(p, end, pc)) {
      ++malformed_;
      if (first_error_.empty()) {
        first_error_ = "missing or invalid PC in line: " + snippet(begin, len);
      }
      continue;
    }

    skip_spaces(p, end);
    if (p == end) {
      ++malformed_;
      if (first_error_.empty()) {
        first_error_ = "missing direction in line: " + snippet(begin, len);
      }
      continue;
    }

    bool taken;
    switch (*p) {
      case 't': case 'T': case '1': taken = true; break;
      case 'n': case 'N': case '0': taken = false; break;
      default:
        ++malformed_;
        if (first_error_.empty()) {
          first_error_ = "unrecognised direction in line: " + snippet(begin, len);
        }
        continue;
    }

    out.pc = pc;
    out.taken = taken;
    ++records_;
    return true;
  }
  return false;
}

}  // namespace cachescope
