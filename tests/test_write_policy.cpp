// Write-policy tests with hand-computed hit/miss and DRAM traffic counts.
//
// Geometry throughout: one level, 4 sets x 1 way x 16-byte blocks (64 B total).
// 0x00 -> set 0 tag 0, 0x40 -> set 0 tag 1, so 0x00 and 0x40 conflict.
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "cachescope/hierarchy.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

using testing::tiny_config;

SimConfig policy_config(WritePolicy wp, AllocPolicy ap) {
  SimConfig c = tiny_config(/*assoc=*/1, /*num_sets=*/4);
  c.levels[0].write_policy = wp;
  c.levels[0].alloc_policy = ap;
  c.validate();
  return c;
}

// Runs a sequence through a fresh hierarchy. Each entry is {is_store, address}.
struct Op {
  bool store;
  Addr addr;
};

void run(Hierarchy& h, const std::vector<Op>& ops) {
  Cycle now = 0;
  for (const Op& o : ops) {
    const MemOutcome out =
        h.access(o.store ? AccessType::Store : AccessType::Load, o.addr, now);
    now += out.total_latency;
  }
}

TEST(WritePolicy, WriteBackAbsorbsRepeatedStores) {
  // s 0x00, s 0x00, s 0x00 against write-back + write-allocate:
  //   1st: store miss -> DRAM read to fetch the line, install dirty
  //   2nd: store hit  -> line already dirty, nothing leaves the cache
  //   3rd: store hit
  // Expect 1 DRAM read, 0 DRAM writes (no eviction ever happens).
  SimConfig cfg = policy_config(WritePolicy::WriteBack, AllocPolicy::WriteAllocate);
  Hierarchy h(cfg);
  run(h, {{true, 0x00}, {true, 0x00}, {true, 0x00}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.stores, 3u);
  EXPECT_EQ(s.store_misses, 1u);
  EXPECT_EQ(s.store_hits, 2u);

  EXPECT_EQ(h.dram().stats().reads, 1u) << "one fetch-on-write";
  EXPECT_EQ(h.dram().stats().writes, 0u) << "write-back must not write through on a hit";
}

TEST(WritePolicy, WriteThroughSendsEveryStoreOnward) {
  // Same sequence against write-through + write-allocate: every store reaches
  // memory, so 3 DRAM writes instead of 0.
  SimConfig cfg = policy_config(WritePolicy::WriteThrough, AllocPolicy::WriteAllocate);
  Hierarchy h(cfg);
  run(h, {{true, 0x00}, {true, 0x00}, {true, 0x00}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.stores, 3u);
  EXPECT_EQ(s.store_misses, 1u);
  EXPECT_EQ(s.store_hits, 2u);

  EXPECT_EQ(h.dram().stats().reads, 1u) << "write-allocate still fetches the line once";
  EXPECT_EQ(h.dram().stats().writes, 3u) << "one DRAM write per store";
  EXPECT_EQ(s.dirty_evictions, 0u) << "a write-through line is never dirty";
}

TEST(WritePolicy, WriteBackConflictProducesTwoDirtyWritebacks) {
  // s 0x00, l 0x00, s 0x40, l 0x00 on write-back + write-allocate:
  //   s 0x00 : miss, DRAM read #1, install dirty
  //   l 0x00 : HIT
  //   s 0x40 : miss (same set, new tag), DRAM read #2, evicts dirty 0x00 -> write #1
  //   l 0x00 : miss, DRAM read #3, evicts dirty 0x40 -> write #2
  SimConfig cfg = policy_config(WritePolicy::WriteBack, AllocPolicy::WriteAllocate);
  Hierarchy h(cfg);
  run(h, {{true, 0x00}, {false, 0x00}, {true, 0x40}, {false, 0x00}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.loads, 2u);
  EXPECT_EQ(s.stores, 2u);
  EXPECT_EQ(s.load_hits, 1u);
  EXPECT_EQ(s.load_misses, 1u);
  EXPECT_EQ(s.store_hits, 0u);
  EXPECT_EQ(s.store_misses, 2u);
  EXPECT_EQ(s.dirty_evictions, 2u);

  EXPECT_EQ(h.dram().stats().reads, 3u);
  EXPECT_EQ(h.dram().stats().writes, 2u);
}

TEST(WritePolicy, NoWriteAllocateDoesNotFetchOnAStoreMiss) {
  // This is the regression guard for a real bug: a store miss against a
  // no-write-allocate level must not issue a DRAM *read*, because no line is
  // being installed. The write alone travels outward.
  //
  //   s 0x00 : miss, no allocation -> 0 reads, 1 write
  //   l 0x00 : miss (nothing was installed) -> DRAM read #1, installs the line
  //   s 0x00 : HIT now -> write-through sends write #2
  SimConfig cfg = policy_config(WritePolicy::WriteThrough, AllocPolicy::NoWriteAllocate);
  Hierarchy h(cfg);
  run(h, {{true, 0x00}, {false, 0x00}, {true, 0x00}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.stores, 2u);
  EXPECT_EQ(s.store_misses, 1u);
  EXPECT_EQ(s.store_hits, 1u);
  EXPECT_EQ(s.loads, 1u);
  EXPECT_EQ(s.load_misses, 1u);

  EXPECT_EQ(h.dram().stats().reads, 1u)
      << "only the load fetches a line; the store miss must not read";
  EXPECT_EQ(h.dram().stats().writes, 2u);
  EXPECT_EQ(s.fills, 1u) << "exactly one line was installed, by the load";
}

TEST(WritePolicy, NoWriteAllocateStoreMissLeavesCacheEmpty) {
  // A run of store misses against a no-write-allocate level installs nothing
  // at all, and every store reaches memory.
  SimConfig cfg = policy_config(WritePolicy::WriteThrough, AllocPolicy::NoWriteAllocate);
  Hierarchy h(cfg);
  run(h, {{true, 0x00}, {true, 0x40}, {true, 0x80}, {true, 0xC0}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.stores, 4u);
  EXPECT_EQ(s.store_misses, 4u);
  EXPECT_EQ(s.store_hits, 0u);
  EXPECT_EQ(s.fills, 0u);
  EXPECT_EQ(h.level(0).valid_lines(), 0u);
  EXPECT_EQ(h.dram().stats().reads, 0u);
  EXPECT_EQ(h.dram().stats().writes, 4u);
}

TEST(WritePolicy, LoadsAlwaysAllocateEvenUnderNoWriteAllocate) {
  // no-write-allocate governs stores only.
  SimConfig cfg = policy_config(WritePolicy::WriteBack, AllocPolicy::NoWriteAllocate);
  Hierarchy h(cfg);
  run(h, {{false, 0x00}, {false, 0x00}});

  const LevelStats& s = h.level(0).stats();
  EXPECT_EQ(s.load_misses, 1u);
  EXPECT_EQ(s.load_hits, 1u) << "the second load must hit the line the first installed";
  EXPECT_EQ(s.fills, 1u);
  EXPECT_EQ(h.dram().stats().reads, 1u);
}

TEST(WritePolicy, StoreMissUnderWriteBackMarksOnlyTheAllocatingLevelDirty) {
  // Two levels, both write-back + write-allocate. A store that misses both
  // must leave L1 dirty and L2 clean: the L2 copy is a clean fill from memory.
  // Evicting the L2 line must then generate no writeback.
  SimConfig cfg;
  cfg.name = "two-level";
  cfg.levels = {testing::tiny_level("l1d", 1, 1), testing::tiny_level("l2", 1, 4)};
  cfg.levels[1].hit_latency = 10;
  cfg.dram.latency_cycles = 100;
  cfg.dram.bytes_per_cycle = 1024.0;
  cfg.third_field = ThirdField::Ignore;
  cfg.validate();

  Hierarchy h(cfg);
  run(h, {{true, 0x00}});

  // L1 has one set, so a load to a conflicting line pushes 0x00 out of L1 and
  // the dirty copy lands in L2.
  EXPECT_EQ(h.level(0).stats().store_misses, 1u);
  EXPECT_EQ(h.level(1).stats().store_misses, 1u);
  EXPECT_EQ(h.level(1).stats().dirty_evictions, 0u);

  run(h, {{false, 0x40}});  // conflicts with 0x00 in the single-set L1
  EXPECT_EQ(h.level(0).stats().dirty_evictions, 1u) << "the dirty L1 copy is written back";
  EXPECT_EQ(h.level(1).stats().writebacks_received, 1u);
  // The writeback found the line already resident in L2 and simply marked it
  // dirty, so no extra L2 fill was needed.
  EXPECT_EQ(h.dram().stats().writes, 0u) << "the dirty line stopped in L2, not memory";
}

}  // namespace
}  // namespace cachescope
