// cachescope -- interval sampling and output sinks.
//
// Every N accesses the engine hands the full counter table to Telemetry, which
// appends a CSV row and a JSON Lines record. The resulting files are a time
// series, so a workload whose behaviour changes partway through shows up as a
// visible step rather than being averaged into a single misleading number.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/counters.hpp"
#include "cachescope/json.hpp"

namespace cachescope {

// Compact per-interval record retained in memory for phase analysis. The full
// counter set goes to disk; only what the diagnosis needs is kept here.
struct IntervalSample {
  Count index = 0;         // 0-based interval number
  Count accesses = 0;      // cumulative demand accesses at the end of the interval
  double instructions = 0; // cumulative
  double interval_cpi = 0; // CPI over this interval alone
  double dram_bw_utilization = 0;  // over this interval alone
  double llc_mpki = 0;             // over this interval alone
  double l1_miss_rate = 0;
  double branch_mispredict_rate = 0;
};

class Telemetry {
 public:
  explicit Telemetry(const TelemetryConfig& cfg);
  ~Telemetry();

  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;

  bool sampling_enabled() const { return cfg_.interval_accesses > 0; }
  std::uint64_t interval_accesses() const { return cfg_.interval_accesses; }

  // Appends one row to each configured sink. The CSV header is written from the
  // first table seen, so all later rows must carry the same metric names.
  void write_row(const MetricTable& table);

  void record_sample(const IntervalSample& s) { samples_.push_back(s); }
  const std::vector<IntervalSample>& samples() const { return samples_; }

  // Writes the final report. Returns false if the path was configured but
  // could not be opened.
  bool write_summary(const Json& summary) const;

 private:
  TelemetryConfig cfg_;
  std::FILE* csv_ = nullptr;
  std::FILE* jsonl_ = nullptr;
  bool csv_header_written_ = false;
  std::vector<IntervalSample> samples_;
};

}  // namespace cachescope
