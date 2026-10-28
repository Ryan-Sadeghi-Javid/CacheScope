// cachescope -- raw statistics structs plus the named-metric table.
//
// The simulator hot path only ever touches the plain structs below; string
// lookups happen exclusively when a snapshot is taken. That keeps interval
// sampling cheap enough to run every few thousand accesses.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cachescope/json.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

struct LevelStats {
  Count loads = 0;
  Count stores = 0;
  Count load_hits = 0;
  Count load_misses = 0;
  Count store_hits = 0;
  Count store_misses = 0;

  // Fills caused by a speculative prefetch, and how many of those lines were
  // later touched by a demand access before eviction.
  Count prefetch_issued = 0;
  Count prefetch_fills = 0;
  Count prefetch_useful = 0;
  Count prefetch_dropped = 0;  // target already resident, so no fill happened

  Count fills = 0;
  Count evictions = 0;
  Count dirty_evictions = 0;     // evictions that generated a writeback
  Count writebacks_received = 0; // dirty lines pushed in from the level above
  Count back_invalidations = 0;  // lines dropped to maintain inclusion

  Count demand_accesses() const { return loads + stores; }
  Count demand_hits() const { return load_hits + store_hits; }
  Count demand_misses() const { return load_misses + store_misses; }
};

struct DramStats {
  Count reads = 0;
  Count writes = 0;
  Count bytes_read = 0;
  Count bytes_written = 0;
  Cycle queue_delay_cycles = 0;  // summed waiting time caused by the bandwidth cap
  Cycle busy_cycles = 0;         // cycles the memory system spent transferring
  Count max_queue_depth = 0;
  Count requests_at_capacity = 0;  // arrivals that found the queue full

  Count bytes_total() const { return bytes_read + bytes_written; }
  Count requests() const { return reads + writes; }
};

struct BranchStats {
  Count branches = 0;
  Count mispredicts = 0;
  Count taken = 0;
  // Tournament bookkeeping: how often the chooser picked the global predictor.
  Count chose_global = 0;
};

// Cycle attribution for the top-down summary. These buckets sum exactly to the
// reported cycle count. They are doubles because the MLP-overlap factor scales
// every stall by a fraction, and rounding each access individually would drift
// away from the modelled total over millions of accesses.
//
// Attribution rule: an access that hits in L1 is treated as fully pipelined and
// costs nothing. Any access that misses L1 exposes every latency it then
// traverses, including the wasted L1 lookup. So a load served by DRAM charges
// l1_hit_latency to l1_stall, each traversed mid-level latency to its own
// bucket, the fixed DRAM latency to dram_latency_stall, and any
// bandwidth-induced waiting to dram_queue_stall.
struct CycleBreakdown {
  double base = 0;      // useful work: base_cpi * instructions
  double l1_stall = 0;  // L1 lookup latency wasted on accesses that missed L1
  double l2_stall = 0;
  double llc_stall = 0;
  double dram_latency_stall = 0;
  double dram_queue_stall = 0;
  double branch_stall = 0;

  double total() const {
    return base + l1_stall + l2_stall + llc_stall + dram_latency_stall + dram_queue_stall +
           branch_stall;
  }
  double memory_stall() const {
    return l1_stall + l2_stall + llc_stall + dram_latency_stall + dram_queue_stall;
  }
};

// Ordered name -> value table. Insertion order is preserved so that CSV
// columns stay stable across rows and across runs of the same config.
class MetricTable {
 public:
  void set(const std::string& name, double value);
  void set_count(const std::string& name, Count value) { set(name, static_cast<double>(value)); }
  bool has(const std::string& name) const;
  double get(const std::string& name, double fallback = 0.0) const;

  const std::vector<std::string>& names() const { return order_; }
  std::size_t size() const { return order_.size(); }

  Json to_json() const;
  std::string csv_header() const;
  std::string csv_row() const;

 private:
  std::vector<std::string> order_;
  std::map<std::string, double> values_;
};

// Safe ratio helpers: a zero denominator yields 0 rather than NaN, so empty
// intervals do not poison the CSV.
double safe_div(double num, double den);
// Instructions is a double because a trace without an instruction count
// derives it from a fractional instructions-per-access factor.
double per_kilo_instr(double events, double instructions);

}  // namespace cachescope
