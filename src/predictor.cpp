#include "cachescope/predictor.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cachescope {
namespace {

// Two-bit saturating counter: 0,1 predict not-taken; 2,3 predict taken.
inline void bump(std::uint8_t& c, bool taken) {
  if (taken) {
    if (c < 3) ++c;
  } else {
    if (c > 0) --c;
  }
}

constexpr std::uint8_t kWeaklyTaken = 2;

}  // namespace

// ---------------------------------------------------------------- bimodal

BimodalPredictor::BimodalPredictor(std::uint32_t index_bits)
    : mask_((Addr{1} << index_bits) - 1),
      table_(std::size_t{1} << index_bits, kWeaklyTaken) {}

bool BimodalPredictor::predict(Addr pc) { return table_[index(pc)] >= 2; }

void BimodalPredictor::update(Addr pc, bool taken, bool) { bump(table_[index(pc)], taken); }

// ---------------------------------------------------------------- gshare

GsharePredictor::GsharePredictor(std::uint32_t index_bits, std::uint32_t history_bits)
    : mask_((Addr{1} << index_bits) - 1),
      history_bits_(history_bits),
      history_mask_(history_bits == 0 ? 0 : (Addr{1} << history_bits) - 1),
      table_(std::size_t{1} << index_bits, kWeaklyTaken) {}

bool GsharePredictor::predict(Addr pc) { return table_[index(pc)] >= 2; }

void GsharePredictor::update(Addr pc, bool taken, bool) {
  bump(table_[index(pc)], taken);
  // Shift the outcome into the global history after the table update, so the
  // index used for training matches the one used for the prediction.
  ghr_ = ((ghr_ << 1) | (taken ? 1u : 0u)) & history_mask_;
}

// ---------------------------------------------------------------- tournament

TournamentPredictor::TournamentPredictor(std::uint32_t index_bits, std::uint32_t history_bits,
                                         std::uint32_t chooser_bits)
    : local_(index_bits),
      global_(index_bits, history_bits),
      chooser_bits_(chooser_bits),
      chooser_mask_((Addr{1} << chooser_bits) - 1),
      chooser_(std::size_t{1} << chooser_bits, kWeaklyTaken) {}

std::size_t TournamentPredictor::chooser_index() const {
  // Indexed by global history: the chooser is learning "in this history
  // context, which predictor is more trustworthy".
  return static_cast<std::size_t>(global_.history() & chooser_mask_);
}

bool TournamentPredictor::predict(Addr pc) {
  last_local_ = local_.predict(pc);
  last_global_ = global_.predict(pc);
  last_chooser_index_ = chooser_index();
  // High counter values favour the global (gshare) predictor.
  last_used_global_ = chooser_[last_chooser_index_] >= 2;
  return last_used_global_ ? last_global_ : last_local_;
}

void TournamentPredictor::update(Addr pc, bool taken, bool) {
  const bool local_right = (last_local_ == taken);
  const bool global_right = (last_global_ == taken);

  // Only train the chooser when the two components disagree; when they agree
  // the choice had no effect and moving the counter would be noise.
  if (local_right != global_right) {
    bump(chooser_[last_chooser_index_], global_right);
  }
  if (last_used_global_) ++stats_.chose_global;

  // Both components always learn, regardless of which one was consulted.
  local_.update(pc, taken, last_local_);
  global_.update(pc, taken, last_global_);
}

std::size_t TournamentPredictor::state_bits() const {
  return local_.state_bits() + global_.state_bits() + chooser_.size() * 2;
}

// ---------------------------------------------------------------- perceptron

PerceptronPredictor::PerceptronPredictor(std::uint32_t index_bits, std::uint32_t history_len)
    : num_perceptrons_(std::size_t{1} << index_bits),
      history_len_(history_len),
      index_mask_((Addr{1} << index_bits) - 1),
      // Jimenez & Lin's empirical training threshold.
      theta_(static_cast<int>(std::floor(1.93 * history_len + 14))),
      weights_(num_perceptrons_ * (history_len + 1), 0),
      history_(history_len, -1) {}

bool PerceptronPredictor::predict(Addr pc) {
  last_index_ = static_cast<std::size_t>(pc & index_mask_);
  const std::int16_t* w = &weights_[last_index_ * (history_len_ + 1)];
  // w[0] is the bias, trained against the branch's overall taken rate.
  int y = w[0];
  for (std::uint32_t i = 0; i < history_len_; ++i) {
    y += history_[i] > 0 ? w[i + 1] : -w[i + 1];
  }
  last_y_ = y;
  return y >= 0;
}

void PerceptronPredictor::update(Addr pc, bool taken, bool predicted) {
  (void)pc;
  const int t = taken ? 1 : -1;

  // Train on a misprediction, or when the output was correct but not confident
  // enough: that second case is what keeps the weights from saturating at
  // values that cannot adapt.
  if (predicted != taken || std::abs(last_y_) <= theta_) {
    std::int16_t* w = weights(last_index_);
    int nw = w[0] + t;
    w[0] = static_cast<std::int16_t>(std::min(kWeightMax, std::max(kWeightMin, nw)));
    for (std::uint32_t i = 0; i < history_len_; ++i) {
      const int x = history_[i] > 0 ? 1 : -1;
      nw = w[i + 1] + t * x;
      w[i + 1] = static_cast<std::int16_t>(std::min(kWeightMax, std::max(kWeightMin, nw)));
    }
  }

  // Shift the outcome in at the front; history_[0] is the most recent branch.
  if (history_len_ > 0) {
    for (std::uint32_t i = history_len_ - 1; i > 0; --i) history_[i] = history_[i - 1];
    history_[0] = static_cast<std::int8_t>(t);
  }
}

// ---------------------------------------------------------------- factory

std::unique_ptr<BranchPredictor> make_predictor(PredictorKind kind, const BranchConfig& cfg) {
  switch (kind) {
    case PredictorKind::Bimodal:
      return std::unique_ptr<BranchPredictor>(new BimodalPredictor(cfg.pc_index_bits));
    case PredictorKind::Gshare:
      return std::unique_ptr<BranchPredictor>(
          new GsharePredictor(cfg.pc_index_bits, cfg.history_bits));
    case PredictorKind::Tournament:
      return std::unique_ptr<BranchPredictor>(
          new TournamentPredictor(cfg.pc_index_bits, cfg.history_bits, cfg.chooser_index_bits));
    case PredictorKind::Perceptron:
      return std::unique_ptr<BranchPredictor>(
          new PerceptronPredictor(cfg.perceptron_index_bits, cfg.perceptron_history));
  }
  throw std::invalid_argument("unknown branch predictor kind");
}

std::unique_ptr<BranchPredictor> make_predictor(const BranchConfig& cfg) {
  return make_predictor(cfg.kind, cfg);
}

}  // namespace cachescope
