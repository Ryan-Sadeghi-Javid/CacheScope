// cachescope -- the simulation engine.
//
// Owns the hierarchy, the branch predictor and the telemetry sinks, and turns
// per-access latencies into the estimated CPI and its top-down breakdown.
//
// Timing model
// ------------
//   instructions   from the trace (exact when the third field is an instruction
//                  gap, otherwise accesses * instructions_per_access)
//   base cycles    base_cpi * instructions
//   memory stalls  for each access that missed L1, every latency it traversed,
//                  scaled by (1 - mlp_overlap)
//   branch stalls  instructions * branch.density * mispredict_rate * penalty
//   CPI            (base + memory stalls + branch stalls) / instructions
//
// The branch component is composed analytically rather than stepped, because a
// branch trace and a memory trace are separate files describing separate
// instruction streams. See BranchConfig::density.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/hierarchy.hpp"
#include "cachescope/predictor.hpp"
#include "cachescope/telemetry.hpp"
#include "cachescope/trace.hpp"

namespace cachescope {

// Plain-English verdict assembled from the cycle breakdown.
struct Diagnosis {
  std::string headline;               // the one-line answer
  std::vector<std::string> findings;  // supporting observations, most significant first
  std::string bottleneck;             // machine-readable: base|l1|l2|llc|dram-latency|dram-bandwidth|branch
};

struct TopDownReport {
  CycleBreakdown cycles;
  double total_cycles = 0;
  double instructions = 0;
  double cpi = 0;
  double ipc = 0;
  double amat = 0;  // average memory access time in cycles, including L1 hits
  Diagnosis diagnosis;

  Json to_json() const;
};

class Engine {
 public:
  explicit Engine(const SimConfig& cfg);

  // --- branch pass -------------------------------------------------------
  // Run the branch trace first, if one is supplied. The resulting mispredict
  // rate feeds the CPI stack of the memory pass.
  void run_branch_trace(BranchTraceReader& reader);
  // Single-step form, used by tests.
  void step_branch(const BranchRef& ref);

  // --- memory pass -------------------------------------------------------
  void step(const MemRef& ref);
  void run_memory_trace(MemTraceReader& reader);

  // Flushes the final interval row.
  void finish();

  // --- reporting ---------------------------------------------------------
  MetricTable metrics() const;
  TopDownReport topdown() const;
  Json summary() const;

  const Hierarchy& hierarchy() const { return hierarchy_; }
  Hierarchy& hierarchy() { return hierarchy_; }
  const BranchPredictor* predictor() const { return predictor_.get(); }
  Telemetry& telemetry() { return telemetry_; }
  const SimConfig& config() const { return cfg_; }

  double instructions() const { return instructions_; }
  Count accesses() const { return accesses_; }
  Count records() const { return records_; }
  Cycle cycle() const { return cycle_; }

  // Human-readable report written to a stream by the CLI.
  void print_report(std::FILE* out) const;

 private:
  void sample_interval();
  // Cumulative branch stall cycles implied by the measured mispredict rate.
  double branch_stall_cycles(double instructions) const;
  double mispredict_rate() const;

  SimConfig cfg_;
  Hierarchy hierarchy_;
  std::unique_ptr<BranchPredictor> predictor_;
  Telemetry telemetry_;

  Count records_ = 0;
  Count accesses_ = 0;
  double instructions_ = 0;
  Cycle cycle_ = 0;
  Cycle total_access_latency_ = 0;
  CycleBreakdown cycles_;

  // Interval sampling state: the cumulative values at the previous sample, so
  // per-interval deltas can be reported next to the monotonic counters.
  struct IntervalBase {
    Count accesses = 0;
    double instructions = 0;
    double cycles = 0;
    Count dram_bytes = 0;
    Count llc_misses = 0;
    Count l1_accesses = 0;
    Count l1_misses = 0;
  };
  IntervalBase prev_;
  Count interval_index_ = 0;
  Count next_sample_at_ = 0;
};

}  // namespace cachescope
