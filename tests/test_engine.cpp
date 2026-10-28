// Engine timing model: CPI arithmetic, AMAT, and the top-down accounting
// identity.
#include <gtest/gtest.h>

#include <cmath>

#include "cachescope/engine.hpp"
#include "test_helpers.hpp"

namespace cachescope {
namespace {

using testing::load;
using testing::store;

// 1 level, 4 sets x 1 way x 16 B, L1 hit latency 1, DRAM latency 100,
// base_cpi 1.0, one instruction per access.
SimConfig timing_config() {
  SimConfig c = testing::tiny_config(/*assoc=*/1, /*num_sets=*/4);
  c.timing.base_cpi = 1.0;
  c.timing.instructions_per_access = 1.0;
  c.timing.mlp_overlap = 0.0;
  c.timing.branch_mispredict_penalty = 10;
  c.branch.kind = PredictorKind::Bimodal;
  c.branch.density = 0.2;
  c.validate();
  return c;
}

TEST(Engine, CpiForOneMissAndNineHits) {
  // Ten loads to the same address:
  //   access 1 : L1 miss -> 1 cycle of L1 lookup + 100 cycles of DRAM = 101
  //   accesses 2..10 : L1 hits, 1 cycle each, fully pipelined (no stall)
  //
  //   instructions = 10
  //   base         = 10 * 1.0                 = 10
  //   l1_stall     = 1 (the wasted L1 lookup) =   1
  //   dram_latency = 100                      = 100
  //   total cycles                            = 111
  //   CPI          = 111 / 10                 = 11.1
  //   AMAT         = (101 + 9*1) / 10         = 11.0
  Engine e(timing_config());
  for (int i = 0; i < 10; ++i) e.step(load(0x00));
  e.finish();

  const MetricTable t = e.metrics();
  EXPECT_DOUBLE_EQ(t.get("sim.instructions"), 10.0);
  EXPECT_DOUBLE_EQ(t.get("sim.accesses"), 10.0);
  EXPECT_DOUBLE_EQ(t.get("topdown.base_cycles"), 10.0);
  EXPECT_DOUBLE_EQ(t.get("topdown.l1_stall_cycles"), 1.0);
  EXPECT_DOUBLE_EQ(t.get("topdown.dram_latency_cycles"), 100.0);
  EXPECT_DOUBLE_EQ(t.get("sim.cycles"), 111.0);
  EXPECT_DOUBLE_EQ(t.get("sim.cpi"), 11.1);
  EXPECT_DOUBLE_EQ(t.get("sim.amat"), 11.0);
  EXPECT_NEAR(t.get("sim.ipc"), 10.0 / 111.0, 1e-12);
}

TEST(Engine, BranchStallEntersTheCpiStack) {
  // Four alternating branches through a bimodal predictor mispredict exactly
  // twice (see test_predictor), giving a rate of 0.5. With density 0.2,
  // penalty 10 and 10 instructions:
  //   branch_stall = 10 * 0.2 * 0.5 * 10 = 10 cycles
  //   total        = 111 + 10            = 121
  //   CPI          = 12.1
  Engine e(timing_config());
  const bool seq[] = {true, false, true, false};
  for (bool taken : seq) {
    BranchRef r;
    r.pc = 0x100;
    r.taken = taken;
    e.step_branch(r);
  }
  for (int i = 0; i < 10; ++i) e.step(load(0x00));
  e.finish();

  const MetricTable t = e.metrics();
  EXPECT_DOUBLE_EQ(t.get("branch.branches"), 4.0);
  EXPECT_DOUBLE_EQ(t.get("branch.mispredicts"), 2.0);
  EXPECT_DOUBLE_EQ(t.get("branch.mispredict_rate"), 0.5);
  EXPECT_DOUBLE_EQ(t.get("branch.accuracy"), 0.5);
  EXPECT_DOUBLE_EQ(t.get("topdown.branch_cycles"), 10.0);
  EXPECT_DOUBLE_EQ(t.get("sim.cycles"), 121.0);
  EXPECT_DOUBLE_EQ(t.get("sim.cpi"), 12.1);
  EXPECT_NEAR(t.get("branch.cpi_contribution"), 1.0, 1e-12);
}

TEST(Engine, TopDownBucketsSumToTotalCycles) {
  Engine e(timing_config());
  // A mix of hits, misses, conflicts and stores.
  const Addr addrs[] = {0x00, 0x10, 0x00, 0x40, 0x00, 0x20, 0x30, 0x40, 0x50, 0x60};
  for (Addr a : addrs) e.step(load(a));
  for (Addr a : addrs) e.step(store(a));
  e.finish();

  const TopDownReport r = e.topdown();
  const double sum = r.cycles.base + r.cycles.l1_stall + r.cycles.l2_stall + r.cycles.llc_stall +
                     r.cycles.dram_latency_stall + r.cycles.dram_queue_stall +
                     r.cycles.branch_stall;
  EXPECT_NEAR(sum, r.total_cycles, 1e-9) << "the buckets are an exact decomposition";
  EXPECT_NEAR(r.total_cycles, e.metrics().get("sim.cycles"), 1e-9);
  EXPECT_NEAR(r.cpi * r.instructions, r.total_cycles, 1e-9);
}

TEST(Engine, TopDownPercentagesSumToOneHundred) {
  Engine e(timing_config());
  for (int i = 0; i < 50; ++i) e.step(load(static_cast<Addr>(i) * 16));
  e.finish();

  const MetricTable t = e.metrics();
  const double total = t.get("topdown.base_pct") + t.get("topdown.l1_stall_pct") +
                       t.get("topdown.l2_stall_pct") + t.get("topdown.llc_stall_pct") +
                       t.get("topdown.dram_latency_pct") + t.get("topdown.dram_queue_pct") +
                       t.get("topdown.branch_pct");
  EXPECT_NEAR(total, 100.0, 1e-6);
}

TEST(Engine, MlpOverlapScalesStallsButNotBaseWork) {
  SimConfig cfg = timing_config();
  cfg.timing.mlp_overlap = 0.5;
  cfg.validate();

  Engine e(cfg);
  for (int i = 0; i < 10; ++i) e.step(load(0x00));
  e.finish();

  const MetricTable t = e.metrics();
  EXPECT_DOUBLE_EQ(t.get("topdown.base_cycles"), 10.0) << "base work is never overlapped";
  EXPECT_DOUBLE_EQ(t.get("topdown.l1_stall_cycles"), 0.5);
  EXPECT_DOUBLE_EQ(t.get("topdown.dram_latency_cycles"), 50.0);
  EXPECT_DOUBLE_EQ(t.get("sim.cycles"), 60.5);
}

TEST(Engine, InstructionGapModeCountsInstructionsExactly) {
  SimConfig cfg = timing_config();
  cfg.third_field = ThirdField::InstrGap;
  cfg.validate();

  Engine e(cfg);
  // Three records with gaps 2, 0 and 5. Each record contributes its gap of
  // non-memory instructions plus the memory instruction itself:
  //   (2+1) + (0+1) + (5+1) = 3 + 1 + 6 = 10.
  for (std::uint32_t gap : {2u, 0u, 5u}) {
    MemRef r = load(0x00);
    r.instr_gap = gap;
    e.step(r);
  }
  e.finish();

  EXPECT_DOUBLE_EQ(e.metrics().get("sim.instructions"), 10.0);
  EXPECT_DOUBLE_EQ(e.metrics().get("sim.accesses"), 3.0);
}

TEST(Engine, InstructionsPerAccessScalesTheInstructionCount) {
  SimConfig cfg = timing_config();
  cfg.timing.instructions_per_access = 3.0;
  cfg.validate();

  Engine e(cfg);
  for (int i = 0; i < 10; ++i) e.step(load(0x00));
  e.finish();

  EXPECT_DOUBLE_EQ(e.metrics().get("sim.instructions"), 30.0);
  // base = 30 cycles, stalls unchanged at 101 -> 131 total.
  EXPECT_DOUBLE_EQ(e.metrics().get("sim.cycles"), 131.0);
}

TEST(Engine, MpkiMatchesMissesPerThousandInstructions) {
  Engine e(timing_config());
  // 8 distinct lines through a 4-set direct-mapped cache; 0x00/0x40 and
  // 0x10/0x50 conflict, so every access is a miss.
  const Addr addrs[] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
  for (Addr a : addrs) e.step(load(a));
  e.finish();

  const MetricTable t = e.metrics();
  const double misses = t.get("l1d.misses");
  const double instrs = t.get("sim.instructions");
  EXPECT_DOUBLE_EQ(misses, 8.0) << "every line is touched for the first time";
  EXPECT_NEAR(t.get("l1d.mpki"), misses * 1000.0 / instrs, 1e-9);
}

TEST(Engine, EmptyRunProducesNoNaNs) {
  Engine e(timing_config());
  e.finish();

  const MetricTable t = e.metrics();
  for (const std::string& name : t.names()) {
    EXPECT_TRUE(std::isfinite(t.get(name))) << name << " must not be NaN or infinite";
  }
  const TopDownReport r = e.topdown();
  EXPECT_EQ(r.diagnosis.bottleneck, "none");
}

TEST(Engine, DiagnosisNamesBandwidthWhenTheBusSaturates) {
  // Narrow the memory bus so a stream of misses must queue, then confirm the
  // diagnosis blames bandwidth rather than latency.
  SimConfig cfg = timing_config();
  cfg.dram.bytes_per_cycle = 0.25;  // 16-byte line occupies 64 cycles
  cfg.dram.latency_cycles = 20;
  cfg.validate();

  Engine e(cfg);
  for (int i = 0; i < 400; ++i) e.step(load(static_cast<Addr>(i) * 16));
  e.finish();

  const TopDownReport r = e.topdown();
  EXPECT_GT(r.cycles.dram_queue_stall, r.cycles.dram_latency_stall)
      << "a saturated bus should spend more time queueing than in fixed latency";
  EXPECT_EQ(r.diagnosis.bottleneck, "dram-bandwidth");
  EXPECT_NE(r.diagnosis.headline.find("Memory-bound"), std::string::npos);
}

TEST(Engine, DiagnosisNamesLatencyWhenBandwidthIsAmple) {
  SimConfig cfg = timing_config();
  cfg.dram.bytes_per_cycle = 1024.0;
  cfg.dram.latency_cycles = 300;
  cfg.validate();

  Engine e(cfg);
  for (int i = 0; i < 200; ++i) e.step(load(static_cast<Addr>(i) * 16));
  e.finish();

  const TopDownReport r = e.topdown();
  EXPECT_EQ(r.cycles.dram_queue_stall, 0.0);
  EXPECT_EQ(r.diagnosis.bottleneck, "dram-latency");
}

TEST(Engine, DiagnosisReportsComputeBoundWhenStallsAreSmall) {
  SimConfig cfg = timing_config();
  // Every access hits after the first, and base work dominates heavily.
  cfg.timing.instructions_per_access = 100.0;
  cfg.validate();

  Engine e(cfg);
  for (int i = 0; i < 200; ++i) e.step(load(0x00));
  e.finish();

  const TopDownReport r = e.topdown();
  EXPECT_EQ(r.diagnosis.bottleneck, "base");
  EXPECT_NE(r.diagnosis.headline.find("Compute-bound"), std::string::npos);
}

TEST(Engine, SummaryJsonCarriesConfigCountersAndDiagnosis) {
  Engine e(timing_config());
  for (int i = 0; i < 20; ++i) e.step(load(static_cast<Addr>(i) * 16));
  e.finish();

  const Json j = e.summary();
  ASSERT_TRUE(j.has("config"));
  ASSERT_TRUE(j.has("counters"));
  ASSERT_TRUE(j.has("topdown"));
  EXPECT_TRUE(j.at("counters").has("l1d.misses"));
  EXPECT_TRUE(j.at("topdown").has("diagnosis"));
  EXPECT_TRUE(j.at("topdown").at("diagnosis").has("headline"));
  // The whole document must survive a round trip through the serialiser.
  const std::string text = j.dump(2);
  const Json reparsed = Json::parse(text);
  EXPECT_DOUBLE_EQ(reparsed.at("counters").at("l1d.misses").as_double(),
                   j.at("counters").at("l1d.misses").as_double());
}

}  // namespace
}  // namespace cachescope
