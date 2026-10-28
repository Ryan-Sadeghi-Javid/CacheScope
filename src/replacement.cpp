#include "cachescope/replacement.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace cachescope {
namespace {

// Rotates `way` to the front of the row, shifting the rest right. Used by LRU
// promotion; assoc is small (typically <= 16) so the memmove-style shift beats
// any node-based structure.
void move_to_front(std::uint32_t* row, std::uint32_t assoc, std::uint32_t way) {
  std::uint32_t i = 0;
  while (i < assoc && row[i] != way) ++i;
  if (i == assoc) return;  // not tracked; nothing to do
  for (; i > 0; --i) row[i] = row[i - 1];
  row[0] = way;
}

void move_to_back(std::uint32_t* row, std::uint32_t assoc, std::uint32_t way) {
  std::uint32_t i = 0;
  while (i < assoc && row[i] != way) ++i;
  if (i == assoc) return;
  for (; i + 1 < assoc; ++i) row[i] = row[i + 1];
  row[assoc - 1] = way;
}

}  // namespace

// ---------------------------------------------------------------- LRU

LruPolicy::LruPolicy(std::uint32_t num_sets, std::uint32_t assoc)
    : assoc_(assoc), order_(static_cast<std::size_t>(num_sets) * assoc) {
  // Start with way 0 as MRU and the highest way as LRU. A cold set fills
  // invalid ways first, so this only matters for determinism.
  for (std::uint32_t s = 0; s < num_sets; ++s) {
    for (std::uint32_t w = 0; w < assoc; ++w) {
      order_[static_cast<std::size_t>(s) * assoc + w] = w;
    }
  }
}

void LruPolicy::promote(std::uint32_t set, std::uint32_t way) {
  move_to_front(&order_[static_cast<std::size_t>(set) * assoc_], assoc_, way);
}

void LruPolicy::on_hit(std::uint32_t set, std::uint32_t way) { promote(set, way); }
void LruPolicy::on_fill(std::uint32_t set, std::uint32_t way) { promote(set, way); }

void LruPolicy::on_invalidate(std::uint32_t set, std::uint32_t way) {
  // An invalidated way should be the next one reused.
  move_to_back(&order_[static_cast<std::size_t>(set) * assoc_], assoc_, way);
}

std::uint32_t LruPolicy::pick_victim(std::uint32_t set) {
  return order_[static_cast<std::size_t>(set) * assoc_ + (assoc_ - 1)];
}

const std::vector<std::uint32_t>& LruPolicy::order_for_set(std::uint32_t set) const {
  // Materialising a row would require a mutable cache; instead expose the flat
  // array through a thread-local scratch vector. Tests are single-threaded.
  static thread_local std::vector<std::uint32_t> scratch;
  scratch.assign(order_.begin() + static_cast<std::ptrdiff_t>(set) * assoc_,
                 order_.begin() + static_cast<std::ptrdiff_t>(set + 1) * assoc_);
  return scratch;
}

// ---------------------------------------------------------------- FIFO

FifoPolicy::FifoPolicy(std::uint32_t num_sets, std::uint32_t assoc)
    : assoc_(assoc), order_(static_cast<std::size_t>(num_sets) * assoc) {
  for (std::uint32_t s = 0; s < num_sets; ++s) {
    for (std::uint32_t w = 0; w < assoc; ++w) {
      order_[static_cast<std::size_t>(s) * assoc + w] = w;
    }
  }
}

void FifoPolicy::on_hit(std::uint32_t, std::uint32_t) {
  // Deliberately empty: that is the whole point of FIFO.
}

void FifoPolicy::on_fill(std::uint32_t set, std::uint32_t way) {
  // Newly installed lines become the youngest, i.e. last to be evicted.
  move_to_back(&order_[static_cast<std::size_t>(set) * assoc_], assoc_, way);
}

void FifoPolicy::on_invalidate(std::uint32_t set, std::uint32_t way) {
  move_to_front(&order_[static_cast<std::size_t>(set) * assoc_], assoc_, way);
}

std::uint32_t FifoPolicy::pick_victim(std::uint32_t set) {
  return order_[static_cast<std::size_t>(set) * assoc_];
}

const std::vector<std::uint32_t>& FifoPolicy::order_for_set(std::uint32_t set) const {
  static thread_local std::vector<std::uint32_t> scratch;
  scratch.assign(order_.begin() + static_cast<std::ptrdiff_t>(set) * assoc_,
                 order_.begin() + static_cast<std::ptrdiff_t>(set + 1) * assoc_);
  return scratch;
}

// ---------------------------------------------------------------- Random

RandomPolicy::RandomPolicy(std::uint32_t, std::uint32_t assoc, std::uint64_t seed)
    : assoc_(assoc), rng_(seed) {}

std::uint32_t RandomPolicy::pick_victim(std::uint32_t) {
  std::uniform_int_distribution<std::uint32_t> dist(0, assoc_ - 1);
  return dist(rng_);
}

// ---------------------------------------------------------------- factory

std::unique_ptr<ReplacementPolicy> make_replacement(ReplacementKind kind, std::uint32_t num_sets,
                                                    std::uint32_t assoc, std::uint64_t seed) {
  switch (kind) {
    case ReplacementKind::LRU:
      return std::unique_ptr<ReplacementPolicy>(new LruPolicy(num_sets, assoc));
    case ReplacementKind::FIFO:
      return std::unique_ptr<ReplacementPolicy>(new FifoPolicy(num_sets, assoc));
    case ReplacementKind::Random:
      return std::unique_ptr<ReplacementPolicy>(new RandomPolicy(num_sets, assoc, seed));
  }
  throw std::invalid_argument("unknown replacement policy");
}

}  // namespace cachescope
