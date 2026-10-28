// Trace parsing: format compatibility, robustness and the third-field modes.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cachescope/trace.hpp"

namespace cachescope {
namespace {

std::string data(const std::string& name) {
  return std::string(CACHESCOPE_TEST_DATA_DIR) + "/" + name;
}

std::vector<MemRef> read_all(const std::string& file, ThirdField tf, Count* malformed = nullptr,
                             std::string* first_error = nullptr) {
  MemTraceReader r(data(file), tf);
  std::vector<MemRef> out;
  MemRef ref;
  while (r.next(ref)) out.push_back(ref);
  if (malformed) *malformed = r.malformed();
  if (first_error) *first_error = r.first_error();
  return out;
}

TEST(Trace, ParsesTheUpstreamFormat) {
  // "l 0x1fffff50 4" is the format used by kev-kim/cache-simulator.
  const std::vector<MemRef> refs = read_all("tiny_rw.trace", ThirdField::Size);
  ASSERT_EQ(refs.size(), 4u);

  EXPECT_EQ(refs[0].type, AccessType::Store);
  EXPECT_EQ(refs[0].addr, 0x00u);
  EXPECT_EQ(refs[1].type, AccessType::Load);
  EXPECT_EQ(refs[1].addr, 0x00u);
  EXPECT_EQ(refs[2].type, AccessType::Store);
  EXPECT_EQ(refs[2].addr, 0x40u);
  EXPECT_EQ(refs[3].type, AccessType::Load);
}

TEST(Trace, AcceptsAliasesCommentsBlanksAndMissingFields) {
  const std::vector<MemRef> refs = read_all("mixed.trace", ThirdField::Size);
  ASSERT_EQ(refs.size(), 6u) << "the comment and the blank line must not become records";

  EXPECT_EQ(refs[0].type, AccessType::Load);
  EXPECT_EQ(refs[0].addr, 0x1fffff50u);
  EXPECT_EQ(refs[0].size, 4u);

  EXPECT_EQ(refs[1].type, AccessType::Store);

  EXPECT_EQ(refs[2].type, AccessType::Load) << "'r' is accepted as a load";
  EXPECT_EQ(refs[2].addr, 0x1fffff60u) << "a missing 0x prefix still parses as hex";
  EXPECT_EQ(refs[2].size, 8u);

  EXPECT_EQ(refs[3].type, AccessType::Store) << "'w' is accepted as a store";
  EXPECT_EQ(refs[3].addr, 0x1FFFFF68u) << "uppercase 0X and hex digits parse";

  EXPECT_EQ(refs[4].addr, 0x1fffff70u);
  EXPECT_EQ(refs[4].size, 1u) << "a third field of 0 is clamped to one byte under Size mode";

  EXPECT_EQ(refs[5].addr, 0x1fffff78u);
  EXPECT_EQ(refs[5].size, 1u) << "an absent third field defaults to one byte";
}

TEST(Trace, InstrGapModeReadsTheThirdFieldAsAnInstructionCount) {
  const std::vector<MemRef> refs = read_all("mixed.trace", ThirdField::InstrGap);
  ASSERT_EQ(refs.size(), 6u);
  EXPECT_EQ(refs[0].instr_gap, 4u);
  EXPECT_EQ(refs[0].size, 1u) << "size is not meaningful in instr_gap mode";
  EXPECT_EQ(refs[2].instr_gap, 8u);
  EXPECT_EQ(refs[4].instr_gap, 0u) << "zero is a legitimate gap: back-to-back memory ops";
  EXPECT_EQ(refs[5].instr_gap, 0u);
}

TEST(Trace, IgnoreModeDiscardsTheThirdField) {
  const std::vector<MemRef> refs = read_all("mixed.trace", ThirdField::Ignore);
  ASSERT_EQ(refs.size(), 6u);
  for (const MemRef& r : refs) {
    EXPECT_EQ(r.size, 1u);
    EXPECT_EQ(r.instr_gap, 0u);
  }
}

TEST(Trace, MalformedLinesAreSkippedAndCounted) {
  Count malformed = 0;
  std::string first;
  const std::vector<MemRef> refs = read_all("malformed.trace", ThirdField::Size, &malformed, &first);

  EXPECT_EQ(refs.size(), 3u) << "three of the six lines are valid";
  EXPECT_EQ(malformed, 3u);
  EXPECT_FALSE(first.empty()) << "the first failure must be described";
  EXPECT_NE(first.find("operation"), std::string::npos)
      << "the first bad line has an unrecognised op; got: " << first;

  EXPECT_EQ(refs[0].addr, 0x10u);
  EXPECT_EQ(refs[1].addr, 0x30u);
  EXPECT_EQ(refs[2].addr, 0x40u);
}

TEST(Trace, HandlesCrlfLineEndings) {
  const std::vector<MemRef> refs = read_all("crlf.trace", ThirdField::Size);
  ASSERT_EQ(refs.size(), 3u);
  EXPECT_EQ(refs[0].addr, 0x100u);
  EXPECT_EQ(refs[1].addr, 0x200u);
  EXPECT_EQ(refs[2].addr, 0x300u);
  EXPECT_EQ(refs[2].size, 4u) << "a trailing CR must not be absorbed into the size field";
}

TEST(Trace, HandlesAMissingTrailingNewline) {
  const std::vector<MemRef> refs = read_all("no_trailing_newline.trace", ThirdField::Size);
  ASSERT_EQ(refs.size(), 3u) << "the unterminated final line must still be read";
  EXPECT_EQ(refs[2].addr, 0x30u);
}

TEST(Trace, RecordCountMatchesWhatWasReturned) {
  MemTraceReader r(data("tiny_rw.trace"), ThirdField::Size);
  MemRef ref;
  Count n = 0;
  while (r.next(ref)) ++n;
  EXPECT_EQ(r.records(), n);
  EXPECT_EQ(r.malformed(), 0u);
}

TEST(Trace, MissingFileThrows) {
  EXPECT_THROW(MemTraceReader(data("definitely_not_here.trace"), ThirdField::Size),
               std::runtime_error);
}

TEST(BranchTrace, ParsesAllDirectionSpellings) {
  BranchTraceReader r(data("branch_tiny.trace"));
  std::vector<BranchRef> refs;
  BranchRef ref;
  while (r.next(ref)) refs.push_back(ref);

  ASSERT_EQ(refs.size(), 6u);
  EXPECT_EQ(refs[0].pc, 0x400100u);
  EXPECT_TRUE(refs[0].taken) << "t";
  EXPECT_FALSE(refs[1].taken) << "n";
  EXPECT_TRUE(refs[2].taken) << "T";
  EXPECT_FALSE(refs[3].taken) << "N";
  EXPECT_TRUE(refs[4].taken) << "1";
  EXPECT_FALSE(refs[5].taken) << "0";
  EXPECT_EQ(r.malformed(), 0u);
}

TEST(BranchTrace, MalformedLinesAreSkippedAndCounted) {
  BranchTraceReader r(data("branch_malformed.trace"));
  std::vector<BranchRef> refs;
  BranchRef ref;
  while (r.next(ref)) refs.push_back(ref);

  EXPECT_EQ(refs.size(), 2u) << "only the first and last lines are well formed";
  EXPECT_EQ(r.malformed(), 3u);
  EXPECT_FALSE(r.first_error().empty());
  EXPECT_TRUE(refs[0].taken);
  EXPECT_FALSE(refs[1].taken);
  EXPECT_EQ(refs[1].pc, 0x40010cu);
}

TEST(LineReader, CountsBytesRead) {
  LineReader lr(data("crlf.trace"));
  const char* begin = nullptr;
  std::size_t len = 0;
  int lines = 0;
  while (lr.next_line(begin, len)) ++lines;
  EXPECT_EQ(lines, 3);
  EXPECT_EQ(lr.bytes_read(), 33u) << "the fixture is 33 bytes including CRLFs";
}

}  // namespace
}  // namespace cachescope
