#include "cachescope/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace cachescope {
namespace {

bool is_power_of_two(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Accepts either "size_bytes": 32768 or the friendlier "size_kb": 32.
std::uint64_t read_size_bytes(const Json& j, const std::string& level_name) {
  if (j.has("size_bytes")) return static_cast<std::uint64_t>(j.at("size_bytes").as_int());
  if (j.has("size_kb")) return static_cast<std::uint64_t>(j.at("size_kb").as_int()) * 1024ull;
  if (j.has("size_mb")) return static_cast<std::uint64_t>(j.at("size_mb").as_int()) * 1024ull * 1024ull;
  throw ConfigError("level '" + level_name + "' needs one of size_bytes, size_kb or size_mb");
}

PrefetcherConfig read_prefetcher(const Json& j, const std::string& level_name) {
  PrefetcherConfig pf;
  const Json* p = j.find("prefetcher");
  if (p == nullptr || p->is_null()) return pf;
  // A bare string is allowed: "prefetcher": "next-line".
  if (p->is_string()) {
    if (!parse_prefetcher_kind(p->as_string(), pf.kind)) {
      throw ConfigError("level '" + level_name + "': unknown prefetcher '" + p->as_string() + "'");
    }
    return pf;
  }
  const std::string kind = p->get_string("kind", "none");
  if (!parse_prefetcher_kind(kind, pf.kind)) {
    throw ConfigError("level '" + level_name + "': unknown prefetcher '" + kind + "'");
  }
  pf.degree = static_cast<std::uint32_t>(p->get_int("degree", pf.degree));
  pf.distance = static_cast<std::uint32_t>(p->get_int("distance", pf.distance));
  pf.table_entries = static_cast<std::uint32_t>(p->get_int("table_entries", pf.table_entries));
  pf.stride_confidence = static_cast<std::uint32_t>(p->get_int("stride_confidence", pf.stride_confidence));
  pf.on_miss_only = p->get_bool("on_miss_only", pf.on_miss_only);
  return pf;
}

Json prefetcher_to_json(const PrefetcherConfig& pf) {
  Json j = Json::object();
  j.set("kind", to_string(pf.kind));
  j.set("degree", static_cast<long long>(pf.degree));
  j.set("distance", static_cast<long long>(pf.distance));
  j.set("table_entries", static_cast<long long>(pf.table_entries));
  j.set("stride_confidence", static_cast<long long>(pf.stride_confidence));
  j.set("on_miss_only", pf.on_miss_only);
  return j;
}

}  // namespace

bool parse_third_field(const std::string& s, ThirdField& out) {
  if (s == "size" || s == "bytes") { out = ThirdField::Size; return true; }
  if (s == "instr_gap" || s == "instructions" || s == "instr") { out = ThirdField::InstrGap; return true; }
  if (s == "ignore" || s == "none") { out = ThirdField::Ignore; return true; }
  return false;
}

const char* to_string(ThirdField f) {
  switch (f) {
    case ThirdField::Size: return "size";
    case ThirdField::InstrGap: return "instr_gap";
    case ThirdField::Ignore: return "ignore";
  }
  return "unknown";
}

bool parse_prefetcher_kind(const std::string& s, PrefetcherKind& out) {
  if (s == "none" || s == "off") { out = PrefetcherKind::None; return true; }
  if (s == "next-line" || s == "nextline" || s == "next_line") { out = PrefetcherKind::NextLine; return true; }
  if (s == "stride") { out = PrefetcherKind::Stride; return true; }
  return false;
}

const char* to_string(PrefetcherKind k) {
  switch (k) {
    case PrefetcherKind::None: return "none";
    case PrefetcherKind::NextLine: return "next-line";
    case PrefetcherKind::Stride: return "stride";
  }
  return "unknown";
}

bool parse_predictor_kind(const std::string& s, PredictorKind& out) {
  if (s == "bimodal") { out = PredictorKind::Bimodal; return true; }
  if (s == "gshare") { out = PredictorKind::Gshare; return true; }
  if (s == "tournament") { out = PredictorKind::Tournament; return true; }
  if (s == "perceptron") { out = PredictorKind::Perceptron; return true; }
  return false;
}

const char* to_string(PredictorKind k) {
  switch (k) {
    case PredictorKind::Bimodal: return "bimodal";
    case PredictorKind::Gshare: return "gshare";
    case PredictorKind::Tournament: return "tournament";
    case PredictorKind::Perceptron: return "perceptron";
  }
  return "unknown";
}

SimConfig SimConfig::from_json(const Json& j) {
  SimConfig c;
  c.name = j.get_string("name", c.name);

  if (j.has("third_field")) {
    const std::string s = j.at("third_field").as_string();
    if (!parse_third_field(s, c.third_field)) {
      throw ConfigError("unknown third_field '" + s + "' (want size, instr_gap or ignore)");
    }
  }
  c.random_seed = static_cast<std::uint64_t>(j.get_int("random_seed", static_cast<std::int64_t>(c.random_seed)));

  if (j.has("inclusion")) {
    const std::string s = j.at("inclusion").as_string();
    if (!parse_inclusion(s, c.inclusion)) {
      throw ConfigError("unknown inclusion '" + s + "' (want inclusive or non-inclusive)");
    }
  }

  const Json* levels = j.find("levels");
  if (levels == nullptr || !levels->is_array() || levels->items().empty()) {
    throw ConfigError("config needs a non-empty 'levels' array");
  }
  for (const Json& lj : levels->items()) {
    LevelConfig lc;
    lc.name = lj.get_string("name", "");
    if (lc.name.empty()) throw ConfigError("every level needs a 'name' (used as the counter prefix)");
    lc.size_bytes = read_size_bytes(lj, lc.name);
    lc.assoc = static_cast<std::uint32_t>(lj.get_int("assoc", lc.assoc));
    lc.block_size = static_cast<std::uint32_t>(lj.get_int("block_size", lc.block_size));
    lc.hit_latency = static_cast<Cycle>(lj.get_int("hit_latency", static_cast<std::int64_t>(lc.hit_latency)));

    const std::string wp = lj.get_string("write_policy", to_string(lc.write_policy));
    if (!parse_write_policy(wp, lc.write_policy)) {
      throw ConfigError("level '" + lc.name + "': unknown write_policy '" + wp + "'");
    }
    const std::string ap = lj.get_string("alloc_policy", to_string(lc.alloc_policy));
    if (!parse_alloc_policy(ap, lc.alloc_policy)) {
      throw ConfigError("level '" + lc.name + "': unknown alloc_policy '" + ap + "'");
    }
    const std::string rp = lj.get_string("replacement", to_string(lc.replacement));
    if (!parse_replacement(rp, lc.replacement)) {
      throw ConfigError("level '" + lc.name + "': unknown replacement '" + rp + "'");
    }
    lc.prefetcher = read_prefetcher(lj, lc.name);
    c.levels.push_back(lc);
  }

  if (const Json* d = j.find("dram")) {
    c.dram.latency_cycles = static_cast<Cycle>(d->get_int("latency_cycles", static_cast<std::int64_t>(c.dram.latency_cycles)));
    c.dram.bytes_per_cycle = d->get_double("bytes_per_cycle", c.dram.bytes_per_cycle);
    c.dram.queue_capacity = static_cast<std::uint32_t>(d->get_int("queue_capacity", c.dram.queue_capacity));
  }

  if (const Json* t = j.find("timing")) {
    c.timing.base_cpi = t->get_double("base_cpi", c.timing.base_cpi);
    c.timing.instructions_per_access = t->get_double("instructions_per_access", c.timing.instructions_per_access);
    c.timing.branch_mispredict_penalty = static_cast<Cycle>(
        t->get_int("branch_mispredict_penalty", static_cast<std::int64_t>(c.timing.branch_mispredict_penalty)));
    c.timing.mlp_overlap = t->get_double("mlp_overlap", c.timing.mlp_overlap);
  }

  if (const Json* b = j.find("branch")) {
    const std::string pk = b->get_string("predictor", to_string(c.branch.kind));
    if (!parse_predictor_kind(pk, c.branch.kind)) {
      throw ConfigError("unknown branch predictor '" + pk + "'");
    }
    c.branch.pc_index_bits = static_cast<std::uint32_t>(b->get_int("pc_index_bits", c.branch.pc_index_bits));
    c.branch.history_bits = static_cast<std::uint32_t>(b->get_int("history_bits", c.branch.history_bits));
    c.branch.chooser_index_bits = static_cast<std::uint32_t>(b->get_int("chooser_index_bits", c.branch.chooser_index_bits));
    c.branch.perceptron_index_bits = static_cast<std::uint32_t>(b->get_int("perceptron_index_bits", c.branch.perceptron_index_bits));
    c.branch.perceptron_history = static_cast<std::uint32_t>(b->get_int("perceptron_history", c.branch.perceptron_history));
    c.branch.density = b->get_double("density", c.branch.density);
  }

  if (const Json* t = j.find("telemetry")) {
    c.telemetry.interval_accesses = static_cast<std::uint64_t>(t->get_int("interval_accesses", 0));
    c.telemetry.csv_path = t->get_string("csv_path", "");
    c.telemetry.jsonl_path = t->get_string("jsonl_path", "");
    c.telemetry.summary_json_path = t->get_string("summary_json_path", "");
  }

  c.validate();
  return c;
}

SimConfig SimConfig::from_file(const std::string& path) {
  return from_json(Json::parse_file(path));
}

void SimConfig::validate() const {
  if (levels.empty()) throw ConfigError("at least one cache level is required");
  if (levels.size() > 3) {
    throw ConfigError("at most three levels are supported (L1, L2, LLC); got " +
                      std::to_string(levels.size()));
  }

  std::uint32_t prev_block = 0;
  std::uint64_t prev_size = 0;
  for (std::size_t i = 0; i < levels.size(); ++i) {
    const LevelConfig& l = levels[i];
    const std::string where = "level '" + l.name + "'";
    if (l.assoc == 0) throw ConfigError(where + ": assoc must be >= 1");
    if (!is_power_of_two(l.block_size) || l.block_size < 4) {
      throw ConfigError(where + ": block_size must be a power of two >= 4, got " +
                        std::to_string(l.block_size));
    }
    const std::uint64_t ways_bytes = static_cast<std::uint64_t>(l.assoc) * l.block_size;
    if (l.size_bytes == 0 || l.size_bytes % ways_bytes != 0) {
      throw ConfigError(where + ": size_bytes (" + std::to_string(l.size_bytes) +
                        ") must be a non-zero multiple of assoc*block_size (" +
                        std::to_string(ways_bytes) + ")");
    }
    if (!is_power_of_two(l.num_sets())) {
      throw ConfigError(where + ": number of sets must be a power of two, got " +
                        std::to_string(l.num_sets()));
    }
    if (i > 0) {
      if (l.block_size < prev_block) {
        throw ConfigError(where + ": block_size (" + std::to_string(l.block_size) +
                          ") must be >= the block size of the level above (" +
                          std::to_string(prev_block) + ")");
      }
      if (l.size_bytes < prev_size) {
        throw ConfigError(where + ": size (" + std::to_string(l.size_bytes) +
                          " B) must be >= the level above (" + std::to_string(prev_size) + " B)");
      }
    }
    prev_block = l.block_size;
    prev_size = l.size_bytes;

    for (std::size_t k = 0; k < i; ++k) {
      if (levels[k].name == l.name) throw ConfigError("duplicate level name '" + l.name + "'");
    }
    if (l.prefetcher.kind != PrefetcherKind::None) {
      if (l.prefetcher.degree == 0) throw ConfigError(where + ": prefetcher degree must be >= 1");
      if (l.prefetcher.kind == PrefetcherKind::Stride && !is_power_of_two(l.prefetcher.table_entries)) {
        throw ConfigError(where + ": stride prefetcher table_entries must be a power of two");
      }
    }
  }

  if (inclusion == InclusionPolicy::Inclusive && levels.size() < 2) {
    throw ConfigError("inclusion policy is meaningless with a single level");
  }
  if (dram.bytes_per_cycle <= 0.0) throw ConfigError("dram.bytes_per_cycle must be > 0");
  if (timing.base_cpi <= 0.0) throw ConfigError("timing.base_cpi must be > 0");
  if (timing.mlp_overlap < 0.0 || timing.mlp_overlap >= 1.0) {
    throw ConfigError("timing.mlp_overlap must be in [0, 1)");
  }
  if (third_field != ThirdField::InstrGap && timing.instructions_per_access < 1.0) {
    throw ConfigError("timing.instructions_per_access must be >= 1 (every access is an instruction)");
  }
  if (branch.pc_index_bits == 0 || branch.pc_index_bits > 24) {
    throw ConfigError("branch.pc_index_bits must be in [1, 24]");
  }
  if (branch.history_bits > 30) throw ConfigError("branch.history_bits must be <= 30");
  if (branch.chooser_index_bits == 0 || branch.chooser_index_bits > 24) {
    throw ConfigError("branch.chooser_index_bits must be in [1, 24]");
  }
  if (branch.perceptron_index_bits == 0 || branch.perceptron_index_bits > 20) {
    throw ConfigError("branch.perceptron_index_bits must be in [1, 20]");
  }
  if (branch.perceptron_history == 0 || branch.perceptron_history > 64) {
    throw ConfigError("branch.perceptron_history must be in [1, 64]");
  }
  if (branch.density <= 0.0 || branch.density > 1.0) {
    throw ConfigError("branch.density must be in (0, 1]");
  }
}

ServiceSource SimConfig::source_for_level(std::size_t index) const {
  // With fewer than three levels the last one still reports as the LLC bucket
  // only if it is actually the last level; otherwise index order decides.
  switch (index) {
    case 0: return ServiceSource::L1;
    case 1: return levels.size() == 2 ? ServiceSource::LLC : ServiceSource::L2;
    default: return ServiceSource::LLC;
  }
}

Json SimConfig::to_json() const {
  Json j = Json::object();
  j.set("name", name);
  j.set("third_field", to_string(third_field));
  j.set("inclusion", to_string(inclusion));
  j.set("random_seed", static_cast<long long>(random_seed));

  Json larr = Json::array();
  for (const LevelConfig& l : levels) {
    Json lj = Json::object();
    lj.set("name", l.name);
    lj.set("size_bytes", static_cast<long long>(l.size_bytes));
    lj.set("assoc", static_cast<long long>(l.assoc));
    lj.set("block_size", static_cast<long long>(l.block_size));
    lj.set("num_sets", static_cast<long long>(l.num_sets()));
    lj.set("hit_latency", static_cast<long long>(l.hit_latency));
    lj.set("write_policy", to_string(l.write_policy));
    lj.set("alloc_policy", to_string(l.alloc_policy));
    lj.set("replacement", to_string(l.replacement));
    lj.set("prefetcher", prefetcher_to_json(l.prefetcher));
    larr.push_back(lj);
  }
  j.set("levels", larr);

  Json dj = Json::object();
  dj.set("latency_cycles", static_cast<long long>(dram.latency_cycles));
  dj.set("bytes_per_cycle", dram.bytes_per_cycle);
  dj.set("queue_capacity", static_cast<long long>(dram.queue_capacity));
  j.set("dram", dj);

  Json tj = Json::object();
  tj.set("base_cpi", timing.base_cpi);
  tj.set("instructions_per_access", timing.instructions_per_access);
  tj.set("branch_mispredict_penalty", static_cast<long long>(timing.branch_mispredict_penalty));
  tj.set("mlp_overlap", timing.mlp_overlap);
  j.set("timing", tj);

  Json bj = Json::object();
  bj.set("predictor", to_string(branch.kind));
  bj.set("pc_index_bits", static_cast<long long>(branch.pc_index_bits));
  bj.set("history_bits", static_cast<long long>(branch.history_bits));
  bj.set("chooser_index_bits", static_cast<long long>(branch.chooser_index_bits));
  bj.set("perceptron_index_bits", static_cast<long long>(branch.perceptron_index_bits));
  bj.set("perceptron_history", static_cast<long long>(branch.perceptron_history));
  bj.set("density", branch.density);
  j.set("branch", bj);

  Json yj = Json::object();
  yj.set("interval_accesses", static_cast<long long>(telemetry.interval_accesses));
  yj.set("csv_path", telemetry.csv_path);
  yj.set("jsonl_path", telemetry.jsonl_path);
  yj.set("summary_json_path", telemetry.summary_json_path);
  j.set("telemetry", yj);

  return j;
}

// ---------------------------------------------------------------- overrides

namespace {

bool all_digits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// Infers the JSON type of a command-line value.
Json infer_value(const std::string& v) {
  if (v == "true") return Json(true);
  if (v == "false") return Json(false);
  if (v == "null") return Json();
  // Accept a number only if the whole string is consumed, so "64kb" stays a
  // string rather than silently becoming 64.
  try {
    std::size_t used = 0;
    const double d = std::stod(v, &used);
    if (used == v.size()) return Json(d);
  } catch (const std::exception&) {
    // fall through to string
  }
  return Json(v);
}

std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : path) {
    if (c == '.') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  return parts;
}

}  // namespace

void json_set_path(Json& root, const std::string& dotted_path, const std::string& value) {
  const std::vector<std::string> parts = split_path(dotted_path);
  for (const std::string& s : parts) {
    if (s.empty()) throw ConfigError("malformed override path: " + dotted_path);
  }

  Json* cur = &root;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    const std::string& seg = parts[i];
    if (all_digits(seg)) {
      if (!cur->is_array()) {
        throw ConfigError("override path '" + dotted_path + "': '" + seg +
                          "' indexes something that is not an array");
      }
      const std::size_t idx = static_cast<std::size_t>(std::stoull(seg));
      if (idx >= cur->items().size()) {
        throw ConfigError("override path '" + dotted_path + "': index " + seg + " is out of range (" +
                          std::to_string(cur->items().size()) + " elements)");
      }
      cur = &cur->item(idx);
    } else {
      cur = &cur->ref(seg);
    }
  }

  const std::string& leaf = parts.back();
  if (all_digits(leaf)) {
    if (!cur->is_array()) {
      throw ConfigError("override path '" + dotted_path + "': '" + leaf +
                        "' indexes something that is not an array");
    }
    const std::size_t idx = static_cast<std::size_t>(std::stoull(leaf));
    if (idx >= cur->items().size()) {
      throw ConfigError("override path '" + dotted_path + "': index " + leaf + " is out of range");
    }
    cur->item(idx) = infer_value(value);
  } else {
    cur->ref(leaf) = infer_value(value);
  }
}

void apply_override(Json& root, const std::string& assignment) {
  const std::size_t eq = assignment.find('=');
  if (eq == std::string::npos || eq == 0) {
    throw ConfigError("override must look like path=value, got: " + assignment);
  }
  json_set_path(root, assignment.substr(0, eq), assignment.substr(eq + 1));
}

SimConfig default_config() {
  SimConfig c;
  c.name = "default-3level";
  c.inclusion = InclusionPolicy::NonInclusive;

  LevelConfig l1;
  l1.name = "l1d";
  l1.size_bytes = 32 * 1024;
  l1.assoc = 8;
  l1.block_size = 64;
  l1.hit_latency = 4;
  l1.write_policy = WritePolicy::WriteBack;
  l1.alloc_policy = AllocPolicy::WriteAllocate;
  l1.replacement = ReplacementKind::LRU;

  LevelConfig l2;
  l2.name = "l2";
  l2.size_bytes = 256 * 1024;
  l2.assoc = 8;
  l2.block_size = 64;
  l2.hit_latency = 12;
  l2.write_policy = WritePolicy::WriteBack;
  l2.alloc_policy = AllocPolicy::WriteAllocate;
  l2.replacement = ReplacementKind::LRU;

  LevelConfig llc;
  llc.name = "llc";
  llc.size_bytes = 2 * 1024 * 1024;
  llc.assoc = 16;
  llc.block_size = 64;
  llc.hit_latency = 36;
  llc.write_policy = WritePolicy::WriteBack;
  llc.alloc_policy = AllocPolicy::WriteAllocate;
  llc.replacement = ReplacementKind::LRU;

  c.levels = {l1, l2, llc};
  c.validate();
  return c;
}

}  // namespace cachescope
