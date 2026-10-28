// DRAM latency, bandwidth and queueing arithmetic.
//
// With bytes_per_cycle = 8 and 64-byte requests, each request occupies the bus
// for ceil(64/8) = 8 cycles. Requests that arrive while the bus is busy wait,
// and that waiting is the bandwidth signal the diagnosis relies on.
#include <gtest/gtest.h>

#include "cachescope/dram.hpp"

namespace cachescope {
namespace {

DramConfig cfg_8bpc() {
  DramConfig c;
  c.latency_cycles = 100;
  c.bytes_per_cycle = 8.0;
  c.queue_capacity = 64;
  return c;
}

TEST(Dram, IdleRequestPaysOnlyFixedLatency) {
  Dram d(cfg_8bpc());
  const Dram::Response r = d.access(/*now=*/0, /*is_write=*/false, 64);
  EXPECT_EQ(r.queue_delay, 0u);
  EXPECT_EQ(r.latency, 100u);
  EXPECT_EQ(d.stats().reads, 1u);
  EXPECT_EQ(d.stats().bytes_read, 64u);
  EXPECT_EQ(d.stats().busy_cycles, 8u) << "64 bytes at 8 B/cycle occupies the bus for 8 cycles";
}

TEST(Dram, BackToBackRequestsQueueInEightCycleSteps) {
  // Three requests all arriving at cycle 0:
  //   #1 start 0,  queue 0,  latency 100, bus free at 8
  //   #2 start 8,  queue 8,  latency 108, bus free at 16
  //   #3 start 16, queue 16, latency 116, bus free at 24
  Dram d(cfg_8bpc());
  const Dram::Response a = d.access(0, false, 64);
  const Dram::Response b = d.access(0, false, 64);
  const Dram::Response c = d.access(0, false, 64);

  EXPECT_EQ(a.queue_delay, 0u);
  EXPECT_EQ(b.queue_delay, 8u);
  EXPECT_EQ(c.queue_delay, 16u);

  EXPECT_EQ(a.latency, 100u);
  EXPECT_EQ(b.latency, 108u);
  EXPECT_EQ(c.latency, 116u);

  EXPECT_EQ(d.stats().queue_delay_cycles, 24u);
  EXPECT_EQ(d.stats().busy_cycles, 24u);
}

TEST(Dram, ArrivalAfterTheBusDrainsPaysNoQueueDelay) {
  Dram d(cfg_8bpc());
  d.access(0, false, 64);   // bus free at 8
  d.access(0, false, 64);   // bus free at 16
  const Dram::Response late = d.access(/*now=*/100, false, 64);
  EXPECT_EQ(late.queue_delay, 0u) << "the bus was idle long before cycle 100";
  EXPECT_EQ(late.latency, 100u);
}

TEST(Dram, WritesAreCountedSeparatelyFromReads) {
  Dram d(cfg_8bpc());
  d.access(0, /*is_write=*/false, 64);
  d.access(200, /*is_write=*/true, 64);
  d.access(400, /*is_write=*/true, 64);

  EXPECT_EQ(d.stats().reads, 1u);
  EXPECT_EQ(d.stats().writes, 2u);
  EXPECT_EQ(d.stats().bytes_read, 64u);
  EXPECT_EQ(d.stats().bytes_written, 128u);
  EXPECT_EQ(d.stats().bytes_total(), 192u);
  EXPECT_EQ(d.stats().requests(), 3u);
}

TEST(Dram, BandwidthUtilisationIsBytesOverPeak) {
  Dram d(cfg_8bpc());
  for (int i = 0; i < 3; ++i) d.access(0, false, 64);
  // 192 bytes moved; peak over 100 cycles is 100 * 8 = 800 bytes.
  EXPECT_NEAR(d.bw_utilization(100), 192.0 / 800.0, 1e-9);
  // Over a shorter window the same traffic is a larger share.
  EXPECT_NEAR(d.bw_utilization(50), 192.0 / 400.0, 1e-9);
}

TEST(Dram, BandwidthUtilisationIsClampedToOne) {
  Dram d(cfg_8bpc());
  for (int i = 0; i < 100; ++i) d.access(0, false, 64);
  // 6400 bytes in a 10-cycle window would be 80x the ceiling.
  EXPECT_DOUBLE_EQ(d.bw_utilization(10), 1.0);
}

TEST(Dram, ZeroElapsedWindowReportsZeroRatherThanNaN) {
  Dram d(cfg_8bpc());
  d.access(0, false, 64);
  EXPECT_DOUBLE_EQ(d.bw_utilization(0), 0.0);
  EXPECT_DOUBLE_EQ(d.bw_utilization_window(0, 0), 0.0);
}

TEST(Dram, SaturationDrivesQueueDelayUpWithoutBound) {
  // A stream of requests arriving faster than the bus drains should see the
  // queue delay grow linearly. This is exactly the "bandwidth-bound" signature.
  Dram d(cfg_8bpc());
  Cycle last_delay = 0;
  for (int i = 0; i < 20; ++i) {
    const Dram::Response r = d.access(/*now=*/static_cast<Cycle>(i), false, 64);
    if (i > 0) {
      EXPECT_GT(r.queue_delay, last_delay) << "delay must grow under saturation";
    }
    last_delay = r.queue_delay;
  }
  EXPECT_GT(last_delay, 100u);
}

TEST(Dram, HigherBandwidthRemovesQueueing) {
  DramConfig fast = cfg_8bpc();
  fast.bytes_per_cycle = 64.0;  // a whole line per cycle
  Dram d(fast);
  for (int i = 0; i < 10; ++i) {
    const Dram::Response r = d.access(static_cast<Cycle>(i), false, 64);
    EXPECT_EQ(r.queue_delay, 0u) << "one line per cycle keeps up with one request per cycle";
  }
}

TEST(Dram, SmallRequestsStillCostAtLeastOneCycle) {
  DramConfig c = cfg_8bpc();
  Dram d(c);
  d.access(0, false, 1);  // 1/8 of a cycle rounds up to 1
  EXPECT_EQ(d.stats().busy_cycles, 1u);
}

TEST(Dram, QueueDepthIsTracked) {
  Dram d(cfg_8bpc());
  for (int i = 0; i < 10; ++i) d.access(0, false, 64);
  EXPECT_GT(d.stats().max_queue_depth, 0u);
}

}  // namespace
}  // namespace cachescope
