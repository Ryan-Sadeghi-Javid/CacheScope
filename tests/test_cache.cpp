// Cache array geometry and line bookkeeping.
#include <gtest/gtest.h>

#include "cachescope/cache.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

using testing::tiny_level;

TEST(Cache, AddressDecomposition) {
  // 4 sets x 1 way x 16-byte blocks: offset bits [3:0], set bits [5:4].
  Cache c(tiny_level("l1d", 1, 4), 1);
  EXPECT_EQ(c.num_sets(), 4u);
  EXPECT_EQ(c.block_size(), 16u);

  EXPECT_EQ(c.block_addr(0x00), 0x00u);
  EXPECT_EQ(c.block_addr(0x0F), 0x00u) << "every byte in a block shares its block address";
  EXPECT_EQ(c.block_addr(0x10), 0x10u);

  EXPECT_EQ(c.set_of(0x00), 0u);
  EXPECT_EQ(c.set_of(0x10), 1u);
  EXPECT_EQ(c.set_of(0x20), 2u);
  EXPECT_EQ(c.set_of(0x30), 3u);
  EXPECT_EQ(c.set_of(0x40), 0u) << "0x40 wraps back to set 0 with a new tag";

  EXPECT_EQ(c.tag_of(0x00), 0u);
  EXPECT_EQ(c.tag_of(0x3F), 0u);
  EXPECT_EQ(c.tag_of(0x40), 1u);
  EXPECT_EQ(c.tag_of(0x80), 2u);
}

TEST(Cache, LineBlockAddressRoundTrips) {
  Cache c(tiny_level("l1d", 2, 4), 1);
  const Addr addrs[] = {0x00, 0x10, 0x40, 0x7F0, 0xDEAD0};
  for (Addr a : addrs) {
    const Addr blk = c.block_addr(a);
    c.insert(blk, false, false);
    const int way = c.find(blk);
    ASSERT_GE(way, 0) << "just-inserted line must be found";
    EXPECT_EQ(c.line_block_addr(c.set_of(blk), static_cast<std::uint32_t>(way)), blk)
        << "reconstructed block address must match what was inserted";
  }
}

TEST(Cache, ColdCacheFillsInvalidWaysBeforeEvicting) {
  Cache c(tiny_level("l1d", 4, 1), 1);
  // Four distinct lines, all mapping to the single set, fit exactly.
  for (int i = 0; i < 4; ++i) {
    const FillResult fr = c.insert(static_cast<Addr>(i) * 16, false, false);
    EXPECT_FALSE(fr.evicted) << "fill " << i << " should take an invalid way";
  }
  EXPECT_EQ(c.valid_lines(), 4u);
  EXPECT_EQ(c.stats().evictions, 0u);

  // The fifth must evict.
  const FillResult fr = c.insert(4 * 16, false, false);
  EXPECT_TRUE(fr.evicted);
  EXPECT_EQ(c.stats().evictions, 1u);
}

TEST(Cache, DirtyEvictionIsReported) {
  Cache c(tiny_level("l1d", 1, 1), 1);
  c.insert(0x00, /*dirty=*/true, false);

  const FillResult fr = c.insert(0x10, false, false);
  ASSERT_TRUE(fr.evicted);
  EXPECT_TRUE(fr.evicted_dirty);
  EXPECT_EQ(fr.evicted_block_addr, 0x00u);
  EXPECT_EQ(c.stats().dirty_evictions, 1u);

  // Evicting a clean line must not be flagged dirty.
  const FillResult fr2 = c.insert(0x20, false, false);
  ASSERT_TRUE(fr2.evicted);
  EXPECT_FALSE(fr2.evicted_dirty);
  EXPECT_EQ(c.stats().dirty_evictions, 1u);
}

TEST(Cache, InvalidateReportsPresenceAndDirtiness) {
  Cache c(tiny_level("l1d", 2, 4), 1);
  c.insert(0x00, /*dirty=*/true, false);
  c.insert(0x10, /*dirty=*/false, false);

  bool dirty = false;
  EXPECT_TRUE(c.invalidate(0x00, &dirty));
  EXPECT_TRUE(dirty);
  EXPECT_LT(c.find(0x00), 0);

  EXPECT_TRUE(c.invalidate(0x10, &dirty));
  EXPECT_FALSE(dirty);

  EXPECT_FALSE(c.invalidate(0x400, &dirty)) << "line was never resident";
  EXPECT_FALSE(dirty);
}

TEST(Cache, PrefetchBitTracksUntouchedSpeculativeFills) {
  Cache c(tiny_level("l1d", 2, 4), 1);
  c.insert(0x00, false, /*prefetched=*/true);
  const int way = c.find(0x00);
  ASSERT_GE(way, 0);
  const std::uint32_t set = c.set_of(0x00);

  EXPECT_TRUE(c.was_prefetched(set, static_cast<std::uint32_t>(way)));
  c.clear_prefetched(set, static_cast<std::uint32_t>(way));
  EXPECT_FALSE(c.was_prefetched(set, static_cast<std::uint32_t>(way)));
  EXPECT_EQ(c.stats().prefetch_fills, 1u);
}

TEST(Cache, DirectMappedConflictSequence) {
  // Hand-computed: 4 sets x 1 way x 16 B. Loads 0x00, 0x10, 0x00, 0x40, 0x00.
  //   0x00 -> set0 tag0 : miss, fill
  //   0x10 -> set1 tag0 : miss, fill
  //   0x00 -> set0 tag0 : HIT
  //   0x40 -> set0 tag1 : miss, evicts 0x00
  //   0x00 -> set0 tag0 : miss, evicts 0x40
  // Expect 4 misses, 1 hit, 2 evictions.
  Cache c(tiny_level("l1d", 1, 4), 1);
  const Addr seq[] = {0x00, 0x10, 0x00, 0x40, 0x00};
  int hits = 0, misses = 0;
  for (Addr a : seq) {
    if (c.find(a) >= 0) {
      ++hits;
    } else {
      ++misses;
      c.insert(a, false, false);
    }
  }
  EXPECT_EQ(hits, 1);
  EXPECT_EQ(misses, 4);
  EXPECT_EQ(c.stats().evictions, 2u);
}

TEST(Cache, Log2ExactRejectsNonPowersOfTwo) {
  EXPECT_EQ(log2_exact(1), 0u);
  EXPECT_EQ(log2_exact(16), 4u);
  EXPECT_EQ(log2_exact(4096), 12u);
  EXPECT_THROW(log2_exact(0), std::invalid_argument);
  EXPECT_THROW(log2_exact(3), std::invalid_argument);
  EXPECT_THROW(log2_exact(48), std::invalid_argument);
}

}  // namespace
}  // namespace cachescope
