// Multi-level behaviour: miss forwarding, writeback routing and inclusion.
#include <gtest/gtest.h>

#include <vector>

#include "cachescope/hierarchy.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

using testing::tiny_level;

// L1: 1 set x 2 ways x 16 B (32 B). L2: 1 set x 2 ways x 16 B (32 B).
// Every address used below maps to the single set of both levels, so conflicts
// are fully under the test's control.
SimConfig two_level(InclusionPolicy inclusion) {
  SimConfig c;
  c.name = "two-level";
  c.levels = {tiny_level("l1d", /*assoc=*/2, /*num_sets=*/1),
              tiny_level("l2", /*assoc=*/2, /*num_sets=*/1)};
  c.levels[1].hit_latency = 10;
  c.inclusion = inclusion;
  c.dram.latency_cycles = 100;
  c.dram.bytes_per_cycle = 1024.0;
  c.third_field = ThirdField::Ignore;
  c.timing.instructions_per_access = 1.0;
  c.validate();
  return c;
}

void walk(Hierarchy& h, const std::vector<std::pair<AccessType, Addr>>& ops) {
  Cycle now = 0;
  for (const auto& op : ops) {
    now += h.access(op.first, op.second, now).total_latency;
  }
}

TEST(Hierarchy, L2SeesExactlyTheL1Misses) {
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  walk(h, {{AccessType::Load, 0x00},
           {AccessType::Load, 0x10},
           {AccessType::Load, 0x00},
           {AccessType::Load, 0x20},
           {AccessType::Load, 0x30}});

  EXPECT_EQ(h.level(1).stats().demand_accesses(), h.level(0).stats().demand_misses())
      << "a level below only ever sees the misses of the level above";
}

TEST(Hierarchy, MissWalksToDramAndFillsEveryLevel) {
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  walk(h, {{AccessType::Load, 0x00}});

  EXPECT_EQ(h.level(0).stats().load_misses, 1u);
  EXPECT_EQ(h.level(1).stats().load_misses, 1u);
  EXPECT_EQ(h.level(0).stats().fills, 1u);
  EXPECT_EQ(h.level(1).stats().fills, 1u);
  EXPECT_EQ(h.dram().stats().reads, 1u);

  // Latency: 1 (L1 lookup) + 10 (L2 lookup) + 100 (DRAM) = 111.
  Hierarchy h2(cfg);
  const MemOutcome out = h2.access(AccessType::Load, 0x00, 0);
  EXPECT_EQ(out.total_latency, 111u);
  EXPECT_EQ(out.l1_latency, 1u);
  EXPECT_EQ(out.llc_latency, 10u) << "with two levels the second is the LLC bucket";
  EXPECT_EQ(out.dram_fixed_latency, 100u);
  EXPECT_EQ(out.source, ServiceSource::Dram);
  EXPECT_FALSE(out.hit_l1);
}

TEST(Hierarchy, L1HitCostsOnlyTheL1Latency) {
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  h.access(AccessType::Load, 0x00, 0);
  const MemOutcome out = h.access(AccessType::Load, 0x00, 200);
  EXPECT_EQ(out.total_latency, 1u);
  EXPECT_TRUE(out.hit_l1);
  EXPECT_EQ(out.source, ServiceSource::L1);
  EXPECT_EQ(out.dram_fixed_latency, 0u);
}

TEST(Hierarchy, L2HitSkipsDram) {
  // Fill both levels with 0x00 and 0x10, then push 0x00 out of L1 only by
  // touching two more lines; the next 0x00 should be served by L2.
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  walk(h, {{AccessType::Load, 0x00}});
  const Count reads_before = h.dram().stats().reads;

  // Evict 0x00 from the 2-way L1 without disturbing L2's copy is impossible
  // in one step, so instead verify the simpler invariant: a line resident in
  // L2 but not L1 is served at L2 latency with no DRAM traffic.
  bool dirty = false;
  ASSERT_TRUE(h.level(0).invalidate(0x00, &dirty));
  ASSERT_FALSE(dirty);

  const MemOutcome out = h.access(AccessType::Load, 0x00, 500);
  EXPECT_EQ(out.total_latency, 11u) << "1 cycle of L1 lookup plus 10 cycles of L2";
  EXPECT_EQ(out.source, ServiceSource::LLC);
  EXPECT_EQ(h.dram().stats().reads, reads_before) << "an L2 hit must not reach memory";
}

TEST(Hierarchy, DirtyL1EvictionLandsInL2NotMemory) {
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  // Store to 0x00 and 0x10 (fills both ways of L1, both dirty in L1), then
  // two loads that conflict and force L1 evictions.
  walk(h, {{AccessType::Store, 0x00},
           {AccessType::Store, 0x10},
           {AccessType::Load, 0x20},
           {AccessType::Load, 0x30}});

  EXPECT_GT(h.level(0).stats().dirty_evictions, 0u);
  EXPECT_EQ(h.level(1).stats().writebacks_received, h.level(0).stats().dirty_evictions)
      << "every dirty L1 eviction must arrive at L2";
}

TEST(Hierarchy, InclusiveLlcEvictionBackInvalidatesTheInnerCopy) {
  // The classic inclusion victim. L2 only observes L1 *misses*, so a line that
  // stays hot in L1 is never re-referenced at L2 and drifts to L2's LRU
  // position. When L2 evicts it, inclusion forces the still-hot L1 copy out.
  //
  //   l 0x00 : miss both, fill both.          L1{00}      L2{00}
  //   l 0x10 : miss both, fill both.          L1{00,10}   L2{00,10}
  //   l 0x00 : L1 HIT (L2 never sees it, so 0x00 is now L2's LRU way)
  //   l 0x20 : miss both. L2 fills first and evicts its LRU way, 0x00,
  //            which back-invalidates the hot L1 copy of 0x00.
  SimConfig cfg = two_level(InclusionPolicy::Inclusive);
  Hierarchy h(cfg);
  walk(h, {{AccessType::Load, 0x00},
           {AccessType::Load, 0x10},
           {AccessType::Load, 0x00},
           {AccessType::Load, 0x20}});

  EXPECT_EQ(h.level(0).stats().back_invalidations, 1u)
      << "the inclusive LLC eviction must drop the L1 copy";
  EXPECT_LT(h.level(0).find(0x00), 0) << "0x00 is no longer resident in L1";

  // And the next reference to 0x00 misses, which is the cost of inclusion.
  const MemOutcome out = h.access(AccessType::Load, 0x00, 1000);
  EXPECT_FALSE(out.hit_l1);
}

TEST(Hierarchy, NonInclusiveKeepsTheInnerCopy) {
  // Identical accesses, non-inclusive: no back-invalidation, and the hot line
  // still hits in L1.
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  walk(h, {{AccessType::Load, 0x00},
           {AccessType::Load, 0x10},
           {AccessType::Load, 0x00},
           {AccessType::Load, 0x20}});

  EXPECT_EQ(h.level(0).stats().back_invalidations, 0u);
  EXPECT_GE(h.level(0).find(0x00), 0) << "0x00 survives in L1 without inclusion";

  const MemOutcome out = h.access(AccessType::Load, 0x00, 1000);
  EXPECT_TRUE(out.hit_l1) << "this is the hit that inclusion costs you";
}

TEST(Hierarchy, InclusionAndNonInclusionDivergeOnTheSameTrace) {
  const std::vector<std::pair<AccessType, Addr>> ops = {
      {AccessType::Load, 0x00}, {AccessType::Load, 0x10}, {AccessType::Load, 0x00},
      {AccessType::Load, 0x20}, {AccessType::Load, 0x00}};

  SimConfig inc = two_level(InclusionPolicy::Inclusive);
  SimConfig non = two_level(InclusionPolicy::NonInclusive);
  Hierarchy hi(inc), hn(non);
  walk(hi, ops);
  walk(hn, ops);

  EXPECT_GT(hi.level(0).stats().load_misses, hn.level(0).stats().load_misses)
      << "inclusion costs extra L1 misses on this access pattern";
}

TEST(Hierarchy, CrossesLineDetectsBlockSpanningAccesses) {
  SimConfig cfg = two_level(InclusionPolicy::NonInclusive);
  Hierarchy h(cfg);
  EXPECT_FALSE(h.crosses_line(0x00, 1));
  EXPECT_FALSE(h.crosses_line(0x00, 16)) << "exactly one 16-byte block";
  EXPECT_TRUE(h.crosses_line(0x0F, 2)) << "last byte of one block plus first of the next";
  EXPECT_TRUE(h.crosses_line(0x08, 16));
}

TEST(Hierarchy, SingleLevelConfigReportsAsL1) {
  SimConfig cfg = testing::tiny_config();
  Hierarchy h(cfg);
  const MemOutcome out = h.access(AccessType::Load, 0x00, 0);
  EXPECT_EQ(out.source, ServiceSource::Dram);
  EXPECT_EQ(out.l1_latency, 1u);
  EXPECT_EQ(out.l2_latency, 0u);
  EXPECT_EQ(out.llc_latency, 0u);
}

TEST(Hierarchy, ThreeLevelLatenciesLandInSeparateBuckets) {
  SimConfig c;
  c.levels = {tiny_level("l1d", 1, 1), tiny_level("l2", 1, 1), tiny_level("llc", 1, 1)};
  c.levels[0].hit_latency = 4;
  c.levels[1].hit_latency = 12;
  c.levels[2].hit_latency = 36;
  c.levels[1].size_bytes = 16;
  c.levels[2].size_bytes = 16;
  c.dram.latency_cycles = 200;
  c.dram.bytes_per_cycle = 1024.0;
  c.third_field = ThirdField::Ignore;
  c.validate();

  Hierarchy h(c);
  const MemOutcome out = h.access(AccessType::Load, 0x00, 0);
  EXPECT_EQ(out.l1_latency, 4u);
  EXPECT_EQ(out.l2_latency, 12u);
  EXPECT_EQ(out.llc_latency, 36u);
  EXPECT_EQ(out.dram_fixed_latency, 200u);
  EXPECT_EQ(out.total_latency, 4u + 12u + 36u + 200u);
}

}  // namespace
}  // namespace cachescope
