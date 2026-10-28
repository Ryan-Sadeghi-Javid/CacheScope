// cachescope -- the multi-level memory hierarchy.
//
// Request flow for a demand access:
//
//   1. Walk inward-to-outward (L1, L2, LLC) until a level hits, charging each
//      level's hit latency as it is traversed. A miss at every level goes to
//      DRAM.
//   2. Fill the levels that missed, outermost first, so that a line evicted
//      from an inner level lands in a level that already holds its successor.
//   3. Route dirty evictions outward; under an inclusive policy, also
//      back-invalidate inner copies when an outer level evicts.
//
// Writebacks and write-through traffic never add to the demand latency (a store
// buffer absorbs them) but they do consume DRAM bandwidth, so they still create
// queueing for later demand reads. That is the mechanism that makes a
// write-heavy workload show up as bandwidth-bound.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "cachescope/cache.hpp"
#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/dram.hpp"
#include "cachescope/prefetcher.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

// Per-access latency, split so the engine can attribute cycles without
// re-deriving which levels were traversed.
struct MemOutcome {
  Cycle total_latency = 0;  // includes the L1 hit latency; this is what AMAT averages
  Cycle l1_latency = 0;
  Cycle l2_latency = 0;
  Cycle llc_latency = 0;
  Cycle dram_fixed_latency = 0;
  Cycle dram_queue_delay = 0;
  ServiceSource source = ServiceSource::L1;
  bool hit_l1 = false;
};

class Hierarchy {
 public:
  explicit Hierarchy(const SimConfig& cfg);

  // Issues one demand access. `now` is the cycle the access is made, used for
  // DRAM queueing.
  MemOutcome access(AccessType type, Addr addr, Cycle now);

  // Number of blocks a request of `size` bytes starting at `addr` spans at L1.
  // Used to report line-crossing accesses.
  bool crosses_line(Addr addr, std::uint32_t size) const;

  std::size_t num_levels() const { return levels_.size(); }
  Cache& level(std::size_t i) { return *levels_[i]; }
  const Cache& level(std::size_t i) const { return *levels_[i]; }
  Dram& dram() { return dram_; }
  const Dram& dram() const { return dram_; }
  const SimConfig& config() const { return cfg_; }

  Count line_crossing_accesses() const { return line_crossings_; }
  void note_line_crossing() { ++line_crossings_; }

 private:
  // Installs `addr` at every level in `missed_` that is allowed to allocate,
  // outermost first. Returns the innermost level that actually allocated, or
  // num_levels() if none did (every missing level was no-write-allocate).
  //
  // `store_dirties` marks only that innermost copy dirty: a fill into an outer
  // level on a store miss is a clean copy from memory, and marking it dirty
  // would manufacture writebacks that real hardware never performs.
  std::size_t fill_missed(Addr addr, bool store_dirties, bool prefetched, Cycle now);
  // Handles the eviction produced by a fill at `level`.
  void handle_eviction(std::size_t level, const FillResult& fr, Cycle now);
  // Sends a dirty line outward, starting at `target` (which may be DRAM).
  void write_back_to(std::size_t target, Addr addr, Cycle now);
  // Inclusion maintenance: drop inner copies of a line evicted at `level`.
  void back_invalidate_above(std::size_t level, Addr addr, Cycle now);
  // Propagates a store outward while the levels it leaves are write-through.
  void propagate_write_through(std::size_t from_level, Addr addr, Cycle now);
  // Runs the prefetcher attached to `level` and issues whatever it asks for.
  void run_prefetcher(std::size_t level, Addr addr, bool was_miss, Cycle now);
  // Brings `addr` into `level` speculatively.
  void issue_prefetch(std::size_t level, Addr addr, Cycle now);

  // The config caps the hierarchy at three levels, so every per-level lookup in
  // the hot path can use a fixed-size array instead of a vector.
  static constexpr std::size_t kMaxLevels = 3;

  // Per-level fields the access loop needs, copied out of LevelConfig once.
  // LevelConfig holds a std::string, so touching it per level per access drags
  // a cache line of unrelated data through L1 for no reason.
  struct LevelFast {
    Cycle hit_latency = 0;
    Addr block_mask = 0;
    ServiceSource bucket = ServiceSource::L1;
    WritePolicy write_policy = WritePolicy::WriteBack;
    AllocPolicy alloc_policy = AllocPolicy::WriteAllocate;
    bool has_prefetcher = false;
  };

  SimConfig cfg_;
  std::vector<std::unique_ptr<Cache>> levels_;
  std::vector<std::unique_ptr<Prefetcher>> prefetchers_;
  std::array<LevelFast, kMaxLevels> fast_{};
  std::size_t num_levels_ = 0;
  bool any_prefetcher_ = false;
  bool inclusive_ = false;
  std::uint32_t last_block_size_ = 64;
  Dram dram_;
  Count line_crossings_ = 0;

  // Scratch reused across accesses to keep the hot path allocation-free.
  std::vector<std::size_t> missed_;
  std::array<bool, kMaxLevels> no_alloc_{};
  // Separate from missed_ because issuing a prefetch reuses missed_ as its own
  // fill list; the demand walk's miss pattern must survive that.
  std::array<bool, kMaxLevels> missed_flags_{};
  std::vector<Addr> pf_targets_;
  // Guards against a prefetch fill triggering the prefetcher again.
  bool in_prefetch_ = false;
};

}  // namespace cachescope
