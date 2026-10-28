// Replacement policy tests.
//
// The LRU/FIFO pair is checked against the classic discriminating sequence
// A, B, A, C on a 2-way set:
//
//   LRU : A fills way0, B fills way1, A hits (A becomes MRU), C evicts B
//         -> set holds {A, C}; a later A hits and a later B misses.
//   FIFO: A fills way0, B fills way1, A hits (order unchanged), C evicts A
//         -> set holds {B, C}; a later A misses and a later B hits.
#include <gtest/gtest.h>

#include <set>

#include "cachescope/cache.hpp"
#include "cachescope/replacement.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

using testing::tiny_level;

// All three addresses map to set 0 of a 1-set, 16-byte-block cache.
constexpr Addr kA = 0x00;
constexpr Addr kB = 0x10;
constexpr Addr kC = 0x20;

TEST(Replacement, LruKeepsTheRecentlyUsedLine) {
  Cache c(tiny_level("l1d", /*assoc=*/2, /*num_sets=*/1, ReplacementKind::LRU), 1);

  EXPECT_LT(c.find(kA), 0);
  c.insert(kA, false, false);
  c.insert(kB, false, false);

  // Touch A so it becomes most recently used.
  const int way_a = c.find(kA);
  ASSERT_GE(way_a, 0);
  c.touch(c.set_of(kA), static_cast<std::uint32_t>(way_a));

  const FillResult fr = c.insert(kC, false, false);
  ASSERT_TRUE(fr.evicted);
  EXPECT_EQ(fr.evicted_block_addr, kB) << "LRU must evict B, the least recently used line";

  EXPECT_GE(c.find(kA), 0) << "A was touched and must survive";
  EXPECT_GE(c.find(kC), 0);
  EXPECT_LT(c.find(kB), 0);
}

TEST(Replacement, FifoIgnoresHitsAndEvictsInInsertOrder) {
  Cache c(tiny_level("l1d", /*assoc=*/2, /*num_sets=*/1, ReplacementKind::FIFO), 1);

  c.insert(kA, false, false);
  c.insert(kB, false, false);

  // A hit must NOT change the eviction order under FIFO.
  const int way_a = c.find(kA);
  ASSERT_GE(way_a, 0);
  c.touch(c.set_of(kA), static_cast<std::uint32_t>(way_a));

  const FillResult fr = c.insert(kC, false, false);
  ASSERT_TRUE(fr.evicted);
  EXPECT_EQ(fr.evicted_block_addr, kA) << "FIFO must evict A, the first one in";

  EXPECT_LT(c.find(kA), 0);
  EXPECT_GE(c.find(kB), 0) << "B is still resident under FIFO";
  EXPECT_GE(c.find(kC), 0);
}

TEST(Replacement, LruAndFifoDivergeOnTheSameSequence) {
  // Same accesses, different survivors: this is the whole point of the policy.
  Cache lru(tiny_level("l1d", 2, 1, ReplacementKind::LRU), 1);
  Cache fifo(tiny_level("l1d", 2, 1, ReplacementKind::FIFO), 1);

  for (Cache* c : {&lru, &fifo}) {
    c->insert(kA, false, false);
    c->insert(kB, false, false);
    const int w = c->find(kA);
    c->touch(c->set_of(kA), static_cast<std::uint32_t>(w));
    c->insert(kC, false, false);
  }

  EXPECT_GE(lru.find(kA), 0);
  EXPECT_LT(lru.find(kB), 0);

  EXPECT_LT(fifo.find(kA), 0);
  EXPECT_GE(fifo.find(kB), 0);
}

TEST(Replacement, LruEvictionOrderOverAFullCycle) {
  // 4-way set, insert A B C D then access them in a known order and confirm
  // the next four victims come out least-recently-used first.
  LruPolicy lru(/*num_sets=*/1, /*assoc=*/4);
  for (std::uint32_t w = 0; w < 4; ++w) lru.on_fill(0, w);
  // Order after fills (MRU..LRU): 3 2 1 0
  EXPECT_EQ(lru.pick_victim(0), 0u);

  lru.on_hit(0, 0);  // MRU..LRU: 0 3 2 1
  EXPECT_EQ(lru.pick_victim(0), 1u);

  lru.on_hit(0, 1);  // 1 0 3 2
  EXPECT_EQ(lru.pick_victim(0), 2u);

  lru.on_hit(0, 2);  // 2 1 0 3
  EXPECT_EQ(lru.pick_victim(0), 3u);
}

TEST(Replacement, FifoOrderIsUnaffectedByHits) {
  FifoPolicy fifo(/*num_sets=*/1, /*assoc=*/4);
  for (std::uint32_t w = 0; w < 4; ++w) fifo.on_fill(0, w);
  // Oldest..newest: 0 1 2 3
  EXPECT_EQ(fifo.pick_victim(0), 0u);

  // Hits change nothing.
  fifo.on_hit(0, 0);
  fifo.on_hit(0, 0);
  fifo.on_hit(0, 3);
  EXPECT_EQ(fifo.pick_victim(0), 0u);

  // A fill moves that way to the back of the queue.
  fifo.on_fill(0, 0);  // oldest..newest: 1 2 3 0
  EXPECT_EQ(fifo.pick_victim(0), 1u);
}

TEST(Replacement, InvalidatedWayIsReusedFirst) {
  LruPolicy lru(1, 4);
  for (std::uint32_t w = 0; w < 4; ++w) lru.on_fill(0, w);
  // Way 2 is dropped, so it should become the next victim even though way 0
  // is older in recency terms.
  lru.on_invalidate(0, 2);
  EXPECT_EQ(lru.pick_victim(0), 2u);
}

TEST(Replacement, RandomStaysInRangeAndIsSeedReproducible) {
  RandomPolicy a(/*num_sets=*/1, /*assoc=*/8, /*seed=*/12345);
  RandomPolicy b(/*num_sets=*/1, /*assoc=*/8, /*seed=*/12345);
  RandomPolicy c(/*num_sets=*/1, /*assoc=*/8, /*seed=*/999);

  std::set<std::uint32_t> seen;
  bool identical_to_b = true;
  bool identical_to_c = true;
  for (int i = 0; i < 2000; ++i) {
    const std::uint32_t va = a.pick_victim(0);
    const std::uint32_t vb = b.pick_victim(0);
    const std::uint32_t vc = c.pick_victim(0);
    ASSERT_LT(va, 8u) << "victim must be a valid way";
    if (va != vb) identical_to_b = false;
    if (va != vc) identical_to_c = false;
    seen.insert(va);
  }
  EXPECT_TRUE(identical_to_b) << "same seed must replay the same victim sequence";
  EXPECT_FALSE(identical_to_c) << "a different seed must produce a different sequence";
  EXPECT_EQ(seen.size(), 8u) << "all ways should be chosen at least once in 2000 draws";
}

TEST(Replacement, PolicyNamesRoundTrip) {
  EXPECT_STREQ(make_replacement(ReplacementKind::LRU, 4, 2, 1)->name(), "lru");
  EXPECT_STREQ(make_replacement(ReplacementKind::FIFO, 4, 2, 1)->name(), "fifo");
  EXPECT_STREQ(make_replacement(ReplacementKind::Random, 4, 2, 1)->name(), "random");
}

}  // namespace
}  // namespace cachescope
