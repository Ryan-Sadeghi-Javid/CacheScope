// Prefetcher tests: the detectors in isolation, then their effect in a real
// hierarchy with hand-computed hit/miss counts.
#include <gtest/gtest.h>

#include <vector>

#include "cachescope/hierarchy.hpp"
#include "cachescope/prefetcher.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

PrefetcherConfig next_line_cfg(std::uint32_t degree, std::uint32_t distance, bool on_miss_only) {
  PrefetcherConfig p;
  p.kind = PrefetcherKind::NextLine;
  p.degree = degree;
  p.distance = distance;
  p.on_miss_only = on_miss_only;
  return p;
}

TEST(Prefetcher, NextLineFetchesTheFollowingLines) {
  NextLinePrefetcher pf(next_line_cfg(/*degree=*/2, /*distance=*/1, /*on_miss_only=*/true), 64);
  std::vector<Addr> out;
  pf.on_access(0x1000, 0x1000, /*was_miss=*/true, out);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], 0x1040u);
  EXPECT_EQ(out[1], 0x1080u);
}

TEST(Prefetcher, NextLineDistanceSkipsAhead) {
  NextLinePrefetcher pf(next_line_cfg(/*degree=*/1, /*distance=*/3, true), 64);
  std::vector<Addr> out;
  pf.on_access(0x2000, 0x2000, true, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0x2000u + 3 * 64) << "distance 3 targets the third line ahead";
}

TEST(Prefetcher, NextLineRespectsOnMissOnly) {
  NextLinePrefetcher strict(next_line_cfg(1, 1, /*on_miss_only=*/true), 64);
  std::vector<Addr> out;
  strict.on_access(0x1000, 0x1000, /*was_miss=*/false, out);
  EXPECT_TRUE(out.empty()) << "a hit must not trigger a miss-only prefetcher";

  NextLinePrefetcher eager(next_line_cfg(1, 1, /*on_miss_only=*/false), 64);
  eager.on_access(0x1000, 0x1000, /*was_miss=*/false, out);
  EXPECT_EQ(out.size(), 1u) << "an always-on prefetcher triggers on hits too";
}

PrefetcherConfig stride_cfg(std::uint32_t confidence) {
  PrefetcherConfig p;
  p.kind = PrefetcherKind::Stride;
  p.degree = 1;
  p.distance = 1;
  p.table_entries = 64;
  p.stride_confidence = confidence;
  p.on_miss_only = false;
  return p;
}

TEST(Prefetcher, StrideNeedsConfirmationsBeforeIssuing) {
  // Walk 0x1000, 0x1100, 0x1200, 0x1300 with stride 0x100, confidence 2:
  //   0x1000 : first sighting of the region, nothing learned
  //   0x1100 : delta 0x100 observed for the first time, confidence 0
  //   0x1200 : delta repeats, confidence 1 -- still below the threshold
  //   0x1300 : delta repeats, confidence 2 -- now it issues 0x1300 + 0x100
  StridePrefetcher pf(stride_cfg(/*confidence=*/2), 64);
  std::vector<Addr> out;

  pf.on_access(0x1000, 0x1000, false, out);
  EXPECT_TRUE(out.empty());
  pf.on_access(0x1100, 0x1100, false, out);
  EXPECT_TRUE(out.empty());
  pf.on_access(0x1200, 0x1200, false, out);
  EXPECT_TRUE(out.empty()) << "confidence 1 is below the configured threshold of 2";

  pf.on_access(0x1300, 0x1300, false, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0x1400u);

  out.clear();
  pf.on_access(0x1400, 0x1400, false, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0x1500u) << "a locked-on detector keeps running ahead";
}

TEST(Prefetcher, StrideHandlesNegativeStrides) {
  StridePrefetcher pf(stride_cfg(1), 64);
  std::vector<Addr> out;
  // Descending walk inside one 4 KiB region.
  pf.on_access(0x1F00, 0x1F00, false, out);
  pf.on_access(0x1E00, 0x1E00, false, out);
  EXPECT_TRUE(out.empty()) << "first delta only establishes the candidate stride";
  pf.on_access(0x1D00, 0x1D00, false, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0x1C00u) << "a -0x100 stride should prefetch downward";
}

TEST(Prefetcher, StrideResetsWhenThePatternBreaks) {
  StridePrefetcher pf(stride_cfg(1), 64);
  std::vector<Addr> out;
  pf.on_access(0x1000, 0x1000, false, out);
  pf.on_access(0x1100, 0x1100, false, out);
  pf.on_access(0x1200, 0x1200, false, out);
  ASSERT_EQ(out.size(), 1u) << "locked on at stride 0x100";

  out.clear();
  // A different delta must drop confidence and stop issuing.
  pf.on_access(0x1500, 0x1500, false, out);
  EXPECT_TRUE(out.empty()) << "the stride changed, so confidence resets";
}

TEST(Prefetcher, StrideIgnoresRepeatedIdenticalAddresses) {
  StridePrefetcher pf(stride_cfg(1), 64);
  std::vector<Addr> out;
  for (int i = 0; i < 5; ++i) pf.on_access(0x1000, 0x1000, false, out);
  EXPECT_TRUE(out.empty()) << "a zero delta carries no stride information";
}

TEST(Prefetcher, FactoryReturnsNullForNone) {
  PrefetcherConfig p;
  p.kind = PrefetcherKind::None;
  EXPECT_EQ(make_prefetcher(p, 64), nullptr);
}

TEST(Prefetcher, NextLineTurnsASequentialScanIntoAlternatingHits) {
  // Hand-computed, 4 sets x 1 way x 16-byte blocks, next-line degree 1:
  //   l 0x00 : miss, fill set0, prefetch 0x10 into set1
  //   l 0x10 : HIT on the prefetched line (useful prefetch #1)
  //   l 0x20 : miss, fill set2, prefetch 0x30 into set3
  //   l 0x30 : HIT (useful prefetch #2)
  // Expect 2 demand misses, 2 demand hits, 2 useful prefetches.
  SimConfig cfg = testing::tiny_config(/*assoc=*/1, /*num_sets=*/4);
  cfg.levels[0].prefetcher = next_line_cfg(1, 1, /*on_miss_only=*/true);
  cfg.validate();

  Hierarchy h(cfg);
  Cycle now = 0;
  for (Addr a : {0x00, 0x10, 0x20, 0x30}) {
    now += h.access(AccessType::Load, a, now).total_latency;
  }

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.loads, 4u);
  EXPECT_EQ(s.load_misses, 2u);
  EXPECT_EQ(s.load_hits, 2u);
  EXPECT_EQ(s.prefetch_issued, 2u);
  EXPECT_EQ(s.prefetch_fills, 2u);
  EXPECT_EQ(s.prefetch_useful, 2u);
}

TEST(Prefetcher, PrefetchOfAResidentLineIsDropped) {
  // With an always-on prefetcher, re-reading the same pair of lines should
  // find the target already present and consume no memory traffic.
  SimConfig cfg = testing::tiny_config(/*assoc=*/1, /*num_sets=*/4);
  cfg.levels[0].prefetcher = next_line_cfg(1, 1, /*on_miss_only=*/false);
  cfg.validate();

  Hierarchy h(cfg);
  Cycle now = 0;
  now += h.access(AccessType::Load, 0x00, now).total_latency;  // issues prefetch of 0x10
  const Count reads_after_first = h.dram().stats().reads;
  now += h.access(AccessType::Load, 0x00, now).total_latency;  // 0x10 already resident

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.prefetch_issued, 2u);
  EXPECT_EQ(s.prefetch_dropped, 1u) << "the second prefetch found its target resident";
  EXPECT_EQ(h.dram().stats().reads, reads_after_first)
      << "a dropped prefetch must not consume bandwidth";
}

TEST(Prefetcher, UselessPrefetchesAreNotCountedAsUseful) {
  // Prefetch a line that is never demanded: accuracy must be 0.
  SimConfig cfg = testing::tiny_config(/*assoc=*/1, /*num_sets=*/4);
  cfg.levels[0].prefetcher = next_line_cfg(1, 1, true);
  cfg.validate();

  Hierarchy h(cfg);
  h.access(AccessType::Load, 0x00, 0);  // prefetches 0x10, which we never touch

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.prefetch_fills, 1u);
  EXPECT_EQ(s.prefetch_useful, 0u);
}

}  // namespace
}  // namespace cachescope
