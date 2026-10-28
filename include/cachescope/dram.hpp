// cachescope -- DRAM with a fixed latency and a bandwidth ceiling.
//
// The model is a single serialising data bus in front of a fixed-latency array:
//
//   start       = max(arrival, bus_free_at)
//   queue_delay = start - arrival
//   bus_free_at = start + bytes / bytes_per_cycle
//   latency     = queue_delay + latency_cycles
//
// The separation matters for diagnosis. A latency-bound workload pays
// latency_cycles and nothing more; a bandwidth-bound one pays an unbounded and
// growing queue_delay, which is exactly the signal the top-down summary uses to
// say "bandwidth-bound" rather than "latency-bound".
#pragma once

#include <cstdint>

#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

class Dram {
 public:
  explicit Dram(const DramConfig& cfg);

  struct Response {
    Cycle latency = 0;      // total cycles the requester waits
    Cycle queue_delay = 0;  // portion attributable to the bandwidth cap
  };

  // `now` is the cycle the request reaches memory.
  Response access(Cycle now, bool is_write, std::uint32_t bytes);

  // Fraction of peak bandwidth consumed over a window of `elapsed` cycles.
  // Clamped to 1.0: the bus cannot do better than its ceiling, and rounding of
  // per-request service time can otherwise overshoot slightly.
  double bw_utilization(Cycle elapsed) const;

  // Bandwidth used between two snapshots, for interval sampling.
  double bw_utilization_window(Count bytes_in_window, Cycle elapsed_in_window) const;

  const DramConfig& config() const { return cfg_; }
  DramStats& stats() { return stats_; }
  const DramStats& stats() const { return stats_; }

 private:
  DramConfig cfg_;
  Cycle bus_free_at_ = 0;
  DramStats stats_;
};

}  // namespace cachescope
