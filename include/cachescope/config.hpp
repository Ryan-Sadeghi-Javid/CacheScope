// cachescope -- simulator configuration, loaded from JSON.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "cachescope/json.hpp"
#include "cachescope/types.hpp"

namespace cachescope {

class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

// How to interpret the third field of a memory-trace line.
//
// The upstream traces this project is compatible with (kev-kim/cache-simulator)
// carry a third integer that the upstream simulator ignores entirely. In
// gcc.trace it is 0 on 42% of lines with mean 1.99 and max 74, which rules out
// a byte count but fits "number of non-memory instructions since the previous
// memory access". Both readings are therefore selectable.
enum class ThirdField : std::uint8_t {
  Size,      // bytes touched by this access (default; matches the documented format)
  InstrGap,  // non-memory instructions retired since the previous access
  Ignore,    // parse and discard
};

bool parse_third_field(const std::string& s, ThirdField& out);
const char* to_string(ThirdField f);

enum class PrefetcherKind : std::uint8_t { None, NextLine, Stride };

bool parse_prefetcher_kind(const std::string& s, PrefetcherKind& out);
const char* to_string(PrefetcherKind k);

struct PrefetcherConfig {
  PrefetcherKind kind = PrefetcherKind::None;
  // Number of lines fetched per trigger.
  std::uint32_t degree = 1;
  // How many lines ahead the first prefetch targets. distance=1 means the
  // immediately following line.
  std::uint32_t distance = 1;
  // Stride detector table size (entries). Power of two.
  std::uint32_t table_entries = 64;
  // Confirmations of the same stride before the detector starts issuing.
  std::uint32_t stride_confidence = 2;
  // Trigger on demand misses only, versus on every demand access.
  bool on_miss_only = true;
};

struct LevelConfig {
  std::string name;  // also the counter prefix, e.g. "l1d"
  std::uint64_t size_bytes = 32 * 1024;
  std::uint32_t assoc = 8;
  std::uint32_t block_size = 64;
  Cycle hit_latency = 4;
  WritePolicy write_policy = WritePolicy::WriteBack;
  AllocPolicy alloc_policy = AllocPolicy::WriteAllocate;
  ReplacementKind replacement = ReplacementKind::LRU;
  PrefetcherConfig prefetcher;

  std::uint32_t num_sets() const {
    return static_cast<std::uint32_t>(size_bytes / (static_cast<std::uint64_t>(assoc) * block_size));
  }
};

struct DramConfig {
  Cycle latency_cycles = 200;
  // Sustained bandwidth ceiling. A 64-byte line at 8 B/cycle occupies the
  // memory system for 8 cycles, which is what creates queueing.
  double bytes_per_cycle = 8.0;
  // Requests allowed to be outstanding before arrivals stall; purely a
  // reporting aid (depth is tracked, not enforced as backpressure on the core).
  std::uint32_t queue_capacity = 64;
};

struct TimingConfig {
  double base_cpi = 1.0;
  // Used to convert accesses into instructions when the trace carries no
  // instruction count (ThirdField::Size or ::Ignore). Ignored when the third
  // field is an instruction gap, where the count is exact.
  double instructions_per_access = 3.0;
  Cycle branch_mispredict_penalty = 15;
  // Fraction of post-L1 memory latency hidden by memory-level parallelism and
  // out-of-order execution. 0.0 models a fully blocking machine.
  double mlp_overlap = 0.0;
};

enum class PredictorKind : std::uint8_t { Bimodal, Gshare, Tournament, Perceptron };

bool parse_predictor_kind(const std::string& s, PredictorKind& out);
const char* to_string(PredictorKind k);

struct BranchConfig {
  PredictorKind kind = PredictorKind::Gshare;
  // log2 of the bimodal / gshare pattern table entry count.
  std::uint32_t pc_index_bits = 12;
  // Global history length in bits (gshare, tournament, perceptron).
  std::uint32_t history_bits = 12;
  // Tournament chooser table, log2 entries.
  std::uint32_t chooser_index_bits = 12;
  // Perceptron: number of perceptrons (log2) and history length.
  std::uint32_t perceptron_index_bits = 8;
  std::uint32_t perceptron_history = 24;

  // Fraction of retired instructions that are conditional branches.
  //
  // A branch trace and a memory trace are separate files describing separate
  // instruction streams, so the two cannot simply be stepped together. Instead
  // the branch trace yields a mispredict *rate* (a property of the predictor on
  // that trace) and this density converts it into a cycle cost against the
  // memory workload's instruction count:
  //
  //   branch_stall = instructions * density * mispredict_rate * penalty
  //
  // 0.2 is a conventional figure for integer code. Set it to the measured
  // branch rate of the workload under study for a sharper estimate.
  double density = 0.2;
};

struct TelemetryConfig {
  // Emit a counter snapshot every N memory accesses. 0 disables sampling.
  std::uint64_t interval_accesses = 0;
  std::string csv_path;
  std::string jsonl_path;
  // Final full report.
  std::string summary_json_path;
};

struct SimConfig {
  std::string name = "unnamed";
  std::vector<LevelConfig> levels;
  InclusionPolicy inclusion = InclusionPolicy::NonInclusive;
  DramConfig dram;
  TimingConfig timing;
  BranchConfig branch;
  TelemetryConfig telemetry;
  ThirdField third_field = ThirdField::Size;
  std::uint64_t random_seed = 1234567;

  static SimConfig from_json(const Json& j);
  static SimConfig from_file(const std::string& path);
  Json to_json() const;

  // Throws ConfigError describing the first problem found.
  void validate() const;

  // Maps a level index onto the top-down attribution bucket.
  ServiceSource source_for_level(std::size_t index) const;
};

// Default configuration used when no file is supplied: 32 KiB L1D, 256 KiB L2,
// 2 MiB LLC, all 64-byte lines, 8 B/cycle DRAM.
SimConfig default_config();

// Applies "levels.1.size_kb=512" style overrides to a parsed config document,
// so a sweep can vary one parameter without writing a config file per point.
// Path segments are object keys, except an all-digit segment which indexes an
// array. The value's type is inferred: true/false become bools, anything that
// parses fully as a number becomes a number, everything else stays a string.
// Throws ConfigError on a malformed path or an out-of-range array index.
void json_set_path(Json& root, const std::string& dotted_path, const std::string& value);

// Splits "path=value" and applies it. Throws ConfigError if there is no '='.
void apply_override(Json& root, const std::string& assignment);

}  // namespace cachescope
