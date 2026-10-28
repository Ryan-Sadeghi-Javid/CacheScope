// cachescope -- one level of set-associative cache storage.
//
// This class is deliberately a dumb array plus a replacement policy: it knows
// nothing about the levels around it. All multi-level behaviour (miss
// forwarding, writeback routing, inclusion) lives in Hierarchy, which keeps the
// policy interactions in one readable place.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/replacement.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

struct CacheLine {
  Addr tag = 0;
  bool valid = false;
  bool dirty = false;
  // Installed by a prefetcher and not yet touched by a demand access. Used to
  // separate useful prefetches from pollution.
  bool prefetched = false;
};

// Outcome of installing a line, so the caller can route a writeback.
struct FillResult {
  bool evicted = false;
  bool evicted_dirty = false;
  Addr evicted_block_addr = 0;
};

class Cache {
 public:
  Cache(const LevelConfig& cfg, std::uint64_t seed);

  // Geometry.
  const LevelConfig& config() const { return cfg_; }
  std::uint32_t num_sets() const { return num_sets_; }
  std::uint32_t assoc() const { return cfg_.assoc; }
  std::uint32_t block_size() const { return cfg_.block_size; }

  Addr block_addr(Addr a) const { return a & ~static_cast<Addr>(cfg_.block_size - 1); }
  std::uint32_t set_of(Addr a) const {
    return static_cast<std::uint32_t>((a >> block_bits_) & set_mask_);
  }
  Addr tag_of(Addr a) const { return a >> (block_bits_ + set_bits_); }

  // Lookup with no side effects. Returns the way, or -1 on a miss.
  int find(Addr a) const;

  // Record a demand hit against the replacement policy.
  void touch(std::uint32_t set, std::uint32_t way) { repl_->on_hit(set, way); }

  // Install a line. Prefers an invalid way; otherwise evicts per policy.
  FillResult insert(Addr a, bool dirty, bool prefetched);

  // Drop a line if resident. Returns true if it was, and reports whether the
  // dropped copy was dirty (the caller must then write it back).
  bool invalidate(Addr a, bool* was_dirty = nullptr);

  // Line-state accessors used by the hierarchy.
  bool is_dirty(std::uint32_t set, std::uint32_t way) const { return line(set, way).dirty; }
  void set_dirty(std::uint32_t set, std::uint32_t way, bool d) { line(set, way).dirty = d; }
  bool was_prefetched(std::uint32_t set, std::uint32_t way) const { return line(set, way).prefetched; }
  void clear_prefetched(std::uint32_t set, std::uint32_t way) { line(set, way).prefetched = false; }

  // Reconstructs the full block address of a resident line.
  Addr line_block_addr(std::uint32_t set, std::uint32_t way) const;

  LevelStats& stats() { return stats_; }
  const LevelStats& stats() const { return stats_; }

  const ReplacementPolicy& replacement() const { return *repl_; }

  // Number of valid lines; used by tests and by the occupancy counter.
  Count valid_lines() const;

 private:
  CacheLine& line(std::uint32_t set, std::uint32_t way) {
    return lines_[static_cast<std::size_t>(set) * cfg_.assoc + way];
  }
  const CacheLine& line(std::uint32_t set, std::uint32_t way) const {
    return lines_[static_cast<std::size_t>(set) * cfg_.assoc + way];
  }

  LevelConfig cfg_;
  std::uint32_t num_sets_;
  std::uint32_t block_bits_;
  std::uint32_t set_bits_;
  Addr set_mask_;
  std::vector<CacheLine> lines_;
  std::unique_ptr<ReplacementPolicy> repl_;
  LevelStats stats_;
};

// log2 for exact powers of two.
std::uint32_t log2_exact(std::uint64_t v);

}  // namespace cachescope
