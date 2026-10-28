// cachescope -- replacement policies.
//
// The cache array always fills an invalid way in preference to evicting, so
// pick_victim() is only ever asked about a fully populated set.
#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "cachescope/types.hpp"

namespace cachescope {

class ReplacementPolicy {
 public:
  virtual ~ReplacementPolicy() = default;

  // A resident line was touched by a demand access.
  virtual void on_hit(std::uint32_t set, std::uint32_t way) = 0;
  // A line was just installed in this way.
  virtual void on_fill(std::uint32_t set, std::uint32_t way) = 0;
  // A line was dropped without being replaced (inclusion back-invalidate).
  virtual void on_invalidate(std::uint32_t set, std::uint32_t way) = 0;
  // Choose the way to evict from a full set.
  virtual std::uint32_t pick_victim(std::uint32_t set) = 0;

  virtual const char* name() const = 0;
};

// Least-recently-used. Per set the ways are kept ordered MRU first.
class LruPolicy : public ReplacementPolicy {
 public:
  LruPolicy(std::uint32_t num_sets, std::uint32_t assoc);
  void on_hit(std::uint32_t set, std::uint32_t way) override;
  void on_fill(std::uint32_t set, std::uint32_t way) override;
  void on_invalidate(std::uint32_t set, std::uint32_t way) override;
  std::uint32_t pick_victim(std::uint32_t set) override;
  const char* name() const override { return "lru"; }

  // Exposed for unit tests: ways from most to least recently used.
  const std::vector<std::uint32_t>& order_for_set(std::uint32_t set) const;

 private:
  void promote(std::uint32_t set, std::uint32_t way);

  std::uint32_t assoc_;
  // Flat num_sets * assoc array; row `set` holds the recency order.
  std::vector<std::uint32_t> order_;
};

// First-in first-out: eviction order follows install order and is unaffected
// by hits. Per set the ways are kept oldest first.
class FifoPolicy : public ReplacementPolicy {
 public:
  FifoPolicy(std::uint32_t num_sets, std::uint32_t assoc);
  void on_hit(std::uint32_t set, std::uint32_t way) override;
  void on_fill(std::uint32_t set, std::uint32_t way) override;
  void on_invalidate(std::uint32_t set, std::uint32_t way) override;
  std::uint32_t pick_victim(std::uint32_t set) override;
  const char* name() const override { return "fifo"; }

  const std::vector<std::uint32_t>& order_for_set(std::uint32_t set) const;

 private:
  std::uint32_t assoc_;
  std::vector<std::uint32_t> order_;
};

// Uniform random victim. Seeded from the config so runs are reproducible.
class RandomPolicy : public ReplacementPolicy {
 public:
  RandomPolicy(std::uint32_t num_sets, std::uint32_t assoc, std::uint64_t seed);
  void on_hit(std::uint32_t, std::uint32_t) override {}
  void on_fill(std::uint32_t, std::uint32_t) override {}
  void on_invalidate(std::uint32_t, std::uint32_t) override {}
  std::uint32_t pick_victim(std::uint32_t set) override;
  const char* name() const override { return "random"; }

 private:
  std::uint32_t assoc_;
  std::mt19937_64 rng_;
};

std::unique_ptr<ReplacementPolicy> make_replacement(ReplacementKind kind, std::uint32_t num_sets,
                                                    std::uint32_t assoc, std::uint64_t seed);

}  // namespace cachescope
