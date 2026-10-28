// cachescope -- branch direction predictors.
//
// All four predictors share one interface so the sweep tooling can compare them
// at equal storage budgets. state_bits() reports that budget, because comparing
// a 4 K-entry gshare against a 256-perceptron table is only meaningful once you
// know what each one costs.
//
// Indexing convention: the PC is used directly (pc & mask), with no shift. The
// traces this simulator consumes carry byte-granular x86-style PCs whose low
// bits do vary. On a fixed-width RISC target, where the low two bits are always
// zero, a shift of two would use the tables better.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

class BranchPredictor {
 public:
  virtual ~BranchPredictor() = default;

  virtual bool predict(Addr pc) = 0;
  // `predicted` is the value previously returned by predict() for this branch.
  virtual void update(Addr pc, bool taken, bool predicted) = 0;
  virtual const char* name() const = 0;
  // Storage cost of the predictor tables, in bits.
  virtual std::size_t state_bits() const = 0;

  BranchStats& stats() { return stats_; }
  const BranchStats& stats() const { return stats_; }

  // Convenience wrapper that predicts, updates and keeps the counters.
  bool tick(Addr pc, bool taken) {
    const bool p = predict(pc);
    ++stats_.branches;
    if (taken) ++stats_.taken;
    if (p != taken) ++stats_.mispredicts;
    update(pc, taken, p);
    return p == taken;
  }

 protected:
  BranchStats stats_;
};

// Two-bit saturating counters indexed by PC. No history at all: the baseline
// every other predictor has to beat.
class BimodalPredictor : public BranchPredictor {
 public:
  explicit BimodalPredictor(std::uint32_t index_bits);
  bool predict(Addr pc) override;
  void update(Addr pc, bool taken, bool predicted) override;
  const char* name() const override { return "bimodal"; }
  std::size_t state_bits() const override { return table_.size() * 2; }

  // Test hook: counter value at a PC, 0..3.
  std::uint8_t counter_at(Addr pc) const { return table_[index(pc)]; }

 private:
  std::size_t index(Addr pc) const { return static_cast<std::size_t>(pc & mask_); }
  Addr mask_;
  std::vector<std::uint8_t> table_;
};

// Global history XOR-ed with the PC. Catches correlated branches that bimodal
// cannot separate.
class GsharePredictor : public BranchPredictor {
 public:
  GsharePredictor(std::uint32_t index_bits, std::uint32_t history_bits);
  bool predict(Addr pc) override;
  void update(Addr pc, bool taken, bool predicted) override;
  const char* name() const override { return "gshare"; }
  std::size_t state_bits() const override { return table_.size() * 2 + history_bits_; }

  std::uint32_t history() const { return static_cast<std::uint32_t>(ghr_); }
  std::uint8_t counter_at(Addr pc) const { return table_[index(pc)]; }

 private:
  std::size_t index(Addr pc) const {
    return static_cast<std::size_t>((pc ^ ghr_) & mask_);
  }
  Addr mask_;
  std::uint32_t history_bits_;
  Addr history_mask_;
  Addr ghr_ = 0;
  std::vector<std::uint8_t> table_;
};

// Bimodal and gshare running in parallel, with a per-history chooser deciding
// which to trust. The chooser only moves when the two disagree, which is the
// only situation where the choice matters.
class TournamentPredictor : public BranchPredictor {
 public:
  TournamentPredictor(std::uint32_t index_bits, std::uint32_t history_bits,
                      std::uint32_t chooser_bits);
  bool predict(Addr pc) override;
  void update(Addr pc, bool taken, bool predicted) override;
  const char* name() const override { return "tournament"; }
  std::size_t state_bits() const override;

 private:
  std::size_t chooser_index() const;

  BimodalPredictor local_;
  GsharePredictor global_;
  // The chooser's width lives in chooser_mask_ and chooser_.size(); storing the
  // bit count separately as well would be a field that is written and never
  // read, which clang rejects under -Wunused-private-field.
  Addr chooser_mask_;
  std::vector<std::uint8_t> chooser_;
  // Carried between predict() and update() for the branch in flight.
  bool last_local_ = false;
  bool last_global_ = false;
  bool last_used_global_ = false;
  std::size_t last_chooser_index_ = 0;
};

// Jimenez & Lin perceptron predictor. Each perceptron holds one bias weight
// plus one weight per history bit; the dot product's sign is the prediction,
// and training happens on a misprediction or whenever confidence is below the
// threshold.
class PerceptronPredictor : public BranchPredictor {
 public:
  PerceptronPredictor(std::uint32_t index_bits, std::uint32_t history_len);
  bool predict(Addr pc) override;
  void update(Addr pc, bool taken, bool predicted) override;
  const char* name() const override { return "perceptron"; }
  std::size_t state_bits() const override {
    return num_perceptrons_ * (history_len_ + 1) * kWeightBits + history_len_;
  }

  int last_output() const { return last_y_; }
  int threshold() const { return theta_; }

 private:
  static constexpr int kWeightBits = 8;
  static constexpr int kWeightMax = 127;
  static constexpr int kWeightMin = -128;

  std::int16_t* weights(std::size_t p) {
    return &weights_[p * (history_len_ + 1)];
  }

  std::size_t num_perceptrons_;
  std::uint32_t history_len_;
  Addr index_mask_;
  int theta_;
  std::vector<std::int16_t> weights_;
  // History as +1/-1 outcomes, most recent first.
  std::vector<std::int8_t> history_;
  int last_y_ = 0;
  std::size_t last_index_ = 0;
};

std::unique_ptr<BranchPredictor> make_predictor(const BranchConfig& cfg);
// Builds a predictor of an explicitly named kind, reusing cfg for sizes.
std::unique_ptr<BranchPredictor> make_predictor(PredictorKind kind, const BranchConfig& cfg);

}  // namespace cachescope
