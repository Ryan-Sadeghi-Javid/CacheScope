// Branch predictor tests.
//
// The two-bit counter tables start at weakly-taken (value 2), which is what
// makes the small hand-computed sequences below predictable.
#include <gtest/gtest.h>

#include <vector>

#include "cachescope/predictor.hpp"

namespace cachescope {
namespace {

BranchConfig small_cfg() {
  BranchConfig c;
  c.pc_index_bits = 10;
  c.history_bits = 8;
  c.chooser_index_bits = 10;
  c.perceptron_index_bits = 6;
  c.perceptron_history = 16;
  c.density = 0.2;
  return c;
}

double accuracy(BranchPredictor& p) {
  const BranchStats& s = p.stats();
  if (s.branches == 0) return 0.0;
  return 1.0 - static_cast<double>(s.mispredicts) / static_cast<double>(s.branches);
}

// Feeds `pattern` repeatedly for `iterations` full cycles.
void feed(BranchPredictor& p, Addr pc, const std::vector<bool>& pattern, int iterations) {
  for (int i = 0; i < iterations; ++i) {
    for (bool taken : pattern) p.tick(pc, taken);
  }
}

TEST(Bimodal, StartsWeaklyTakenAndSaturates) {
  BimodalPredictor p(10);
  const Addr pc = 0x100;
  EXPECT_EQ(p.counter_at(pc), 2) << "tables are initialised weakly-taken";
  EXPECT_TRUE(p.predict(pc));

  p.update(pc, true, true);
  EXPECT_EQ(p.counter_at(pc), 3);
  p.update(pc, true, true);
  EXPECT_EQ(p.counter_at(pc), 3) << "must saturate at 3";

  p.update(pc, false, true);
  EXPECT_EQ(p.counter_at(pc), 2);
  p.update(pc, false, true);
  EXPECT_EQ(p.counter_at(pc), 1);
  EXPECT_FALSE(p.predict(pc));
  p.update(pc, false, false);
  p.update(pc, false, false);
  EXPECT_EQ(p.counter_at(pc), 0) << "must saturate at 0";
}

TEST(Bimodal, FourNotTakenBranchesCostExactlyOneMispredict) {
  // Hand-computed, pc fixed, counter starts at 2:
  //   #1 predicts taken (2)     -> WRONG, counter 1
  //   #2 predicts not-taken (1) -> right,  counter 0
  //   #3 predicts not-taken (0) -> right,  counter 0
  //   #4 predicts not-taken (0) -> right,  counter 0
  BimodalPredictor p(10);
  for (int i = 0; i < 4; ++i) p.tick(0x100, false);

  EXPECT_EQ(p.stats().branches, 4u);
  EXPECT_EQ(p.stats().mispredicts, 1u);
  EXPECT_EQ(p.stats().taken, 0u);
  EXPECT_DOUBLE_EQ(accuracy(p), 0.75);
}

TEST(Bimodal, AlternatingPatternCostsFiftyPercent) {
  // T,N,T,N against a single counter starting at 2:
  //   predict T (2), actual T -> right, 3
  //   predict T (3), actual N -> WRONG, 2
  //   predict T (2), actual T -> right, 3
  //   predict T (3), actual N -> WRONG, 2
  BimodalPredictor p(10);
  const bool seq[] = {true, false, true, false};
  for (bool t : seq) p.tick(0x100, t);

  EXPECT_EQ(p.stats().branches, 4u);
  EXPECT_EQ(p.stats().mispredicts, 2u);
  EXPECT_DOUBLE_EQ(accuracy(p), 0.5);
}

TEST(Bimodal, SeparatePcsUseSeparateCounters) {
  BimodalPredictor p(10);
  // Train one PC strongly taken and another strongly not-taken.
  for (int i = 0; i < 8; ++i) p.update(0x100, true, true);
  for (int i = 0; i < 8; ++i) p.update(0x200, false, false);

  EXPECT_TRUE(p.predict(0x100));
  EXPECT_FALSE(p.predict(0x200));
}

TEST(Bimodal, AliasingPcsShareACounter) {
  // With 4 index bits, 0x00 and 0x10 collide.
  BimodalPredictor p(4);
  for (int i = 0; i < 8; ++i) p.update(0x00, false, false);
  EXPECT_FALSE(p.predict(0x10)) << "0x10 aliases onto the counter trained by 0x00";
}

TEST(Gshare, LearnsAnAlternatingPatternThatDefeatsBimodal) {
  // Alternating T/N is not predictable from the PC alone but is trivially
  // predictable from one bit of global history.
  const std::vector<bool> alternating = {true, false};

  BimodalPredictor bim(10);
  GsharePredictor gs(10, 8);
  feed(bim, 0x100, alternating, 500);
  feed(gs, 0x100, alternating, 500);

  EXPECT_NEAR(accuracy(bim), 0.5, 0.02) << "bimodal cannot do better than a coin flip here";
  EXPECT_GT(accuracy(gs), 0.98) << "gshare should learn the alternation almost perfectly";
}

TEST(Gshare, HistoryRegisterIsMaskedToTheConfiguredLength) {
  GsharePredictor p(10, /*history_bits=*/3);
  for (int i = 0; i < 10; ++i) p.update(0x100, true, true);
  EXPECT_EQ(p.history(), 0x7u) << "three bits of all-taken history saturates at 0b111";

  p.update(0x100, false, true);
  EXPECT_EQ(p.history(), 0x6u) << "shifting in a not-taken gives 0b110";
}

TEST(Gshare, ZeroHistoryDegeneratesToBimodal) {
  GsharePredictor gs(10, /*history_bits=*/0);
  BimodalPredictor bim(10);
  const std::vector<bool> pattern = {true, true, false, true};
  feed(gs, 0x100, pattern, 200);
  feed(bim, 0x100, pattern, 200);
  EXPECT_DOUBLE_EQ(accuracy(gs), accuracy(bim))
      << "with no history, gshare is exactly a bimodal predictor";
}

TEST(Gshare, LearnsAPeriodFourPattern) {
  GsharePredictor p(12, 8);
  feed(p, 0x400, {true, true, false, false}, 500);
  EXPECT_GT(accuracy(p), 0.98);
}

TEST(Tournament, MatchesTheBetterComponentOnAHistoryPattern) {
  const std::vector<bool> alternating = {true, false};
  BimodalPredictor bim(10);
  GsharePredictor gs(10, 8);
  TournamentPredictor tour(10, 8, 10);
  feed(bim, 0x100, alternating, 500);
  feed(gs, 0x100, alternating, 500);
  feed(tour, 0x100, alternating, 500);

  EXPECT_GT(accuracy(tour), accuracy(bim))
      << "the chooser should abandon the component that is guessing";
  EXPECT_GT(accuracy(tour), 0.95);
}

TEST(Tournament, TracksWhichComponentWasConsulted) {
  TournamentPredictor tour(10, 8, 10);
  feed(tour, 0x100, {true, false}, 200);
  // The chooser must have been exercised; on this pattern the global predictor
  // is the correct choice and should dominate.
  EXPECT_GT(tour.stats().chose_global, 0u);
}

TEST(Tournament, StateIsTheSumOfItsParts) {
  TournamentPredictor tour(10, 8, 10);
  BimodalPredictor bim(10);
  GsharePredictor gs(10, 8);
  // bimodal table + gshare table + gshare history + chooser table
  EXPECT_EQ(tour.state_bits(), bim.state_bits() + gs.state_bits() + (std::size_t{1} << 10) * 2);
}

TEST(Perceptron, ThresholdFollowsTheJimenezLinFormula) {
  // theta = floor(1.93 * history + 14)
  PerceptronPredictor p16(6, 16);
  EXPECT_EQ(p16.threshold(), static_cast<int>(1.93 * 16 + 14));
  PerceptronPredictor p24(6, 24);
  EXPECT_EQ(p24.threshold(), 60) << "1.93*24 + 14 = 60.32, floored to 60";
}

TEST(Perceptron, StartsAtZeroWeightsAndPredictsTaken) {
  PerceptronPredictor p(6, 16);
  // All weights zero gives a dot product of 0, and y >= 0 predicts taken.
  EXPECT_TRUE(p.predict(0x100));
  EXPECT_EQ(p.last_output(), 0);
}

TEST(Perceptron, LearnsAStronglyBiasedBranch) {
  PerceptronPredictor p(6, 16);
  feed(p, 0x100, {true}, 200);
  EXPECT_GT(accuracy(p), 0.99) << "an always-taken branch should be learned by the bias weight";
}

TEST(Perceptron, LearnsAnAlternatingPattern) {
  PerceptronPredictor p(8, 16);
  feed(p, 0x100, {true, false}, 500);
  EXPECT_GT(accuracy(p), 0.95);
}

TEST(Perceptron, HandlesALongPeriodPatternThatStressesHistory) {
  // Period 6; well within a 16-bit history window.
  PerceptronPredictor p(8, 16);
  feed(p, 0x100, {true, true, true, false, false, false}, 800);
  EXPECT_GT(accuracy(p), 0.95);
}

TEST(Perceptron, WeightsStayWithinTheirEightBitRange) {
  // Hammer one branch in one direction for a long time; the clamp must hold,
  // which shows up as the predictor staying correct rather than overflowing
  // into a wrong sign.
  PerceptronPredictor p(6, 8);
  feed(p, 0x100, {true}, 5000);
  EXPECT_TRUE(p.predict(0x100));
  EXPECT_GT(accuracy(p), 0.999);
}

TEST(PredictorFactory, BuildsEachKindWithTheRightName) {
  const BranchConfig c = small_cfg();
  EXPECT_STREQ(make_predictor(PredictorKind::Bimodal, c)->name(), "bimodal");
  EXPECT_STREQ(make_predictor(PredictorKind::Gshare, c)->name(), "gshare");
  EXPECT_STREQ(make_predictor(PredictorKind::Tournament, c)->name(), "tournament");
  EXPECT_STREQ(make_predictor(PredictorKind::Perceptron, c)->name(), "perceptron");
}

TEST(PredictorFactory, ReportsNonZeroStateBudget) {
  const BranchConfig c = small_cfg();
  for (PredictorKind k : {PredictorKind::Bimodal, PredictorKind::Gshare,
                          PredictorKind::Tournament, PredictorKind::Perceptron}) {
    EXPECT_GT(make_predictor(k, c)->state_bits(), 0u);
  }
}

TEST(Predictor, TickCountsBranchesTakenAndMispredicts) {
  BimodalPredictor p(10);
  p.tick(0x100, true);
  p.tick(0x100, true);
  p.tick(0x100, false);
  EXPECT_EQ(p.stats().branches, 3u);
  EXPECT_EQ(p.stats().taken, 2u);
  EXPECT_EQ(p.stats().mispredicts, 1u) << "only the final not-taken is missed";
}

}  // namespace
}  // namespace cachescope
