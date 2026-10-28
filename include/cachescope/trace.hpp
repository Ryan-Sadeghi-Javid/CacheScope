// cachescope -- trace input.
//
// Parsing is hand-rolled over a large read buffer rather than built on
// std::getline plus std::istringstream. On an 8-million-line trace the stream
// approach dominates the profile; the simulator should be measuring caches, not
// iostream formatting.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

// Buffered line splitter. Yields pointers into an internal buffer that stay
// valid only until the next call.
class LineReader {
 public:
  // `path` of "-" reads standard input.
  explicit LineReader(const std::string& path);
  ~LineReader();

  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;

  bool next_line(const char*& begin, std::size_t& len);
  std::uint64_t bytes_read() const { return bytes_read_; }

 private:
  bool refill();

  std::FILE* fp_ = nullptr;
  bool owns_ = false;
  std::vector<char> buf_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  bool eof_ = false;
  std::uint64_t bytes_read_ = 0;
};

// Reads "l 0x1fffff50 4" style records, the format used by
// kev-kim/cache-simulator. Accepts l/r for loads and s/w for stores, an
// optional 0x prefix, and a missing third field.
class MemTraceReader {
 public:
  MemTraceReader(const std::string& path, ThirdField third_field);

  bool next(MemRef& out);

  Count records() const { return records_; }
  Count malformed() const { return malformed_; }
  const std::string& first_error() const { return first_error_; }

 private:
  LineReader reader_;
  ThirdField third_field_;
  Count records_ = 0;
  Count malformed_ = 0;
  std::string first_error_;
};

// Reads "<hex pc> <t|n>" records. Also accepts 1/0 and T/N.
class BranchTraceReader {
 public:
  explicit BranchTraceReader(const std::string& path);

  bool next(BranchRef& out);

  Count records() const { return records_; }
  Count malformed() const { return malformed_; }
  const std::string& first_error() const { return first_error_; }

 private:
  LineReader reader_;
  Count records_ = 0;
  Count malformed_ = 0;
  std::string first_error_;
};

}  // namespace cachescope
