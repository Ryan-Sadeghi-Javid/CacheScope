// cachescope -- command-line entry point.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "cachescope/config.hpp"
#include "cachescope/engine.hpp"
#include "cachescope/json.hpp"
#include "cachescope/predictor.hpp"
#include "cachescope/trace.hpp"

namespace {

using namespace cachescope;

const char* kUsage =
    "cachescope -- trace-driven memory hierarchy and branch prediction simulator\n"
    "\n"
    "USAGE\n"
    "  cachescope run    [options] --trace <file>\n"
    "  cachescope branch [options] --trace <file>\n"
    "  cachescope config [options]\n"
    "  cachescope --help\n"
    "\n"
    "COMMANDS\n"
    "  run       Simulate a memory trace through the cache hierarchy.\n"
    "  branch    Evaluate branch predictors on a branch trace.\n"
    "  config    Print the effective configuration as JSON and exit.\n"
    "\n"
    "COMMON OPTIONS\n"
    "  -c, --config <file>     JSON configuration file (default: built-in 3-level config)\n"
    "  -t, --trace <file>      Trace file, or - for standard input\n"
    "  -s, --set <path=value>  Override one config field, repeatable.\n"
    "                          e.g. --set levels.1.size_kb=512 --set branch.predictor=gshare\n"
    "      --json <file>       Write the full JSON summary here\n"
    "  -q, --quiet             Suppress the human-readable report\n"
    "      --help              Show this help\n"
    "\n"
    "run OPTIONS\n"
    "      --branch-trace <f>  Also evaluate a branch trace; its mispredict rate\n"
    "                          feeds the CPI stack (see branch.density in the config)\n"
    "      --csv <file>        Interval telemetry as CSV\n"
    "      --jsonl <file>      Interval telemetry as JSON Lines\n"
    "      --interval <n>      Sample every n accesses (0 disables, default from config)\n"
    "      --limit <n>         Stop after n trace records (0 = no limit)\n"
    "      --third-field <k>   size | instr_gap | ignore (overrides the config)\n"
    "\n"
    "branch OPTIONS\n"
    "      --predictor <k>     bimodal | gshare | tournament | perceptron\n"
    "      --compare           Evaluate all four predictors on the same trace\n"
    "\n"
    "EXAMPLES\n"
    "  cachescope run -t traces/upstream/swim.trace --csv out.csv --interval 20000\n"
    "  cachescope run -t traces/upstream/gcc.trace --set levels.1.size_kb=1024\n"
    "  cachescope branch -t traces/branch/sample.trace --compare\n";

struct Options {
  std::string command;
  std::string config_path;
  std::string trace_path;
  std::string branch_trace_path;
  std::string json_path;
  std::string csv_path;
  std::string jsonl_path;
  std::string third_field;
  std::string predictor;
  std::vector<std::string> overrides;
  long long interval = -1;  // -1 => leave the config value alone
  long long limit = 0;
  bool quiet = false;
  bool compare = false;
  bool help = false;
};

[[noreturn]] void fail(const std::string& msg) {
  std::fprintf(stderr, "cachescope: %s\n", msg.c_str());
  std::exit(2);
}

// Returns the value for a flag that requires one, advancing the index.
std::string need_value(int argc, char** argv, int& i, const char* flag) {
  if (i + 1 >= argc) fail(std::string("option ") + flag + " requires a value");
  return argv[++i];
}

Options parse_args(int argc, char** argv) {
  Options o;
  int start = 1;
  if (argc > 1 && argv[1][0] != '-') {
    o.command = argv[1];
    start = 2;
  }

  for (int i = start; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      o.help = true;
    } else if (a == "-c" || a == "--config") {
      o.config_path = need_value(argc, argv, i, "--config");
    } else if (a == "-t" || a == "--trace") {
      o.trace_path = need_value(argc, argv, i, "--trace");
    } else if (a == "--branch-trace") {
      o.branch_trace_path = need_value(argc, argv, i, "--branch-trace");
    } else if (a == "--json") {
      o.json_path = need_value(argc, argv, i, "--json");
    } else if (a == "--csv") {
      o.csv_path = need_value(argc, argv, i, "--csv");
    } else if (a == "--jsonl") {
      o.jsonl_path = need_value(argc, argv, i, "--jsonl");
    } else if (a == "--interval") {
      o.interval = std::stoll(need_value(argc, argv, i, "--interval"));
    } else if (a == "--limit") {
      o.limit = std::stoll(need_value(argc, argv, i, "--limit"));
    } else if (a == "--third-field") {
      o.third_field = need_value(argc, argv, i, "--third-field");
    } else if (a == "--predictor") {
      o.predictor = need_value(argc, argv, i, "--predictor");
    } else if (a == "--compare") {
      o.compare = true;
    } else if (a == "-s" || a == "--set") {
      o.overrides.push_back(need_value(argc, argv, i, "--set"));
    } else if (a == "-q" || a == "--quiet") {
      o.quiet = true;
    } else {
      fail("unknown option: " + a);
    }
  }
  return o;
}

// Builds the configuration: file (or built-in default), then --set overrides,
// then the convenience flags that map onto config fields.
SimConfig build_config(const Options& o) {
  Json doc = o.config_path.empty() ? default_config().to_json()
                                   : Json::parse_file(o.config_path);

  for (const std::string& ov : o.overrides) apply_override(doc, ov);

  if (!o.third_field.empty()) json_set_path(doc, "third_field", o.third_field);
  if (!o.predictor.empty()) json_set_path(doc, "branch.predictor", o.predictor);
  if (!o.csv_path.empty()) json_set_path(doc, "telemetry.csv_path", o.csv_path);
  if (!o.jsonl_path.empty()) json_set_path(doc, "telemetry.jsonl_path", o.jsonl_path);
  if (!o.json_path.empty()) json_set_path(doc, "telemetry.summary_json_path", o.json_path);
  if (o.interval >= 0) {
    json_set_path(doc, "telemetry.interval_accesses", std::to_string(o.interval));
  }

  return SimConfig::from_json(doc);
}

int cmd_config(const Options& o) {
  const SimConfig cfg = build_config(o);
  std::printf("%s\n", cfg.to_json().dump(2).c_str());
  return 0;
}

int cmd_run(const Options& o) {
  if (o.trace_path.empty()) fail("run needs --trace <file> (use - for standard input)");
  const SimConfig cfg = build_config(o);

  Engine engine(cfg);

  // The branch pass runs first: its mispredict rate is an input to the CPI
  // stack reported for the memory pass.
  if (!o.branch_trace_path.empty()) {
    BranchTraceReader br(o.branch_trace_path);
    engine.run_branch_trace(br);
    if (br.malformed() > 0 && !o.quiet) {
      std::fprintf(stderr, "cachescope: skipped %llu malformed branch lines (first: %s)\n",
                   static_cast<unsigned long long>(br.malformed()), br.first_error().c_str());
    }
  }

  MemTraceReader reader(o.trace_path, cfg.third_field);
  const auto t0 = std::chrono::steady_clock::now();

  MemRef ref;
  long long seen = 0;
  while (reader.next(ref)) {
    engine.step(ref);
    if (o.limit > 0 && ++seen >= o.limit) break;
  }
  engine.finish();

  const auto t1 = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(t1 - t0).count();

  if (reader.malformed() > 0) {
    std::fprintf(stderr, "cachescope: skipped %llu malformed trace lines (first: %s)\n",
                 static_cast<unsigned long long>(reader.malformed()),
                 reader.first_error().c_str());
  }

  // Attach throughput to the summary so the performance claim in the README is
  // always reproducible from a real run.
  Json summary = engine.summary();
  Json perf = Json::object();
  perf.set("wall_seconds", seconds);
  perf.set("accesses", static_cast<long long>(engine.accesses()));
  perf.set("accesses_per_second",
           seconds > 0 ? static_cast<double>(engine.accesses()) / seconds : 0.0);
  perf.set("million_accesses_per_second",
           seconds > 0 ? static_cast<double>(engine.accesses()) / seconds / 1e6 : 0.0);
  summary.set("performance", perf);

  if (!cfg.telemetry.summary_json_path.empty()) {
    if (!engine.telemetry().write_summary(summary)) {
      fail("cannot write summary JSON to " + cfg.telemetry.summary_json_path);
    }
  }

  if (!o.quiet) {
    engine.print_report(stdout);
    std::printf("\nsimulated %llu accesses in %.3f s (%.2f M accesses/s)\n",
                static_cast<unsigned long long>(engine.accesses()), seconds,
                seconds > 0 ? static_cast<double>(engine.accesses()) / seconds / 1e6 : 0.0);
  }
  return 0;
}

int cmd_branch(const Options& o) {
  if (o.trace_path.empty()) fail("branch needs --trace <file>");
  const SimConfig cfg = build_config(o);

  std::vector<PredictorKind> kinds;
  if (o.compare) {
    kinds = {PredictorKind::Bimodal, PredictorKind::Gshare, PredictorKind::Tournament,
             PredictorKind::Perceptron};
  } else {
    kinds = {cfg.branch.kind};
  }

  // Load the trace once; every predictor must see exactly the same sequence.
  std::vector<BranchRef> refs;
  {
    BranchTraceReader br(o.trace_path);
    BranchRef r;
    while (br.next(r)) {
      refs.push_back(r);
      if (o.limit > 0 && static_cast<long long>(refs.size()) >= o.limit) break;
    }
    if (br.malformed() > 0) {
      std::fprintf(stderr, "cachescope: skipped %llu malformed branch lines (first: %s)\n",
                   static_cast<unsigned long long>(br.malformed()), br.first_error().c_str());
    }
  }
  if (refs.empty()) fail("branch trace contained no usable records: " + o.trace_path);

  // With no memory trace there is no instruction count, so it is derived from
  // the branch count and the configured branch density.
  const double instructions = static_cast<double>(refs.size()) / cfg.branch.density;

  Json out = Json::object();
  out.set("trace", o.trace_path);
  out.set("branches", static_cast<long long>(refs.size()));
  out.set("assumed_branch_density", cfg.branch.density);
  out.set("assumed_instructions", instructions);
  Json arr = Json::array();

  if (!o.quiet) {
    std::printf("branch predictor comparison on %s (%zu branches)\n", o.trace_path.c_str(),
                refs.size());
    std::printf("%s\n", std::string(78, '=').c_str());
    std::printf("%-12s %12s %10s %9s %10s %12s\n", "predictor", "mispredicts", "accuracy", "MPKI",
                "CPI cost", "state bits");
  }

  for (PredictorKind k : kinds) {
    auto pred = make_predictor(k, cfg.branch);
    const auto t0 = std::chrono::steady_clock::now();
    for (const BranchRef& r : refs) pred->tick(r.pc, r.taken);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const BranchStats& s = pred->stats();
    const double rate =
        safe_div(static_cast<double>(s.mispredicts), static_cast<double>(s.branches));
    const double accuracy = 1.0 - rate;
    const double mpki = per_kilo_instr(static_cast<double>(s.mispredicts), instructions);
    const double cpi_cost = safe_div(
        static_cast<double>(s.mispredicts) *
            static_cast<double>(cfg.timing.branch_mispredict_penalty),
        instructions);

    if (!o.quiet) {
      std::printf("%-12s %12llu %9.3f%% %9.3f %10.4f %12zu\n", pred->name(),
                  static_cast<unsigned long long>(s.mispredicts), accuracy * 100.0, mpki, cpi_cost,
                  pred->state_bits());
    }

    Json e = Json::object();
    e.set("predictor", pred->name());
    e.set("branches", static_cast<long long>(s.branches));
    e.set("mispredicts", static_cast<long long>(s.mispredicts));
    e.set("taken", static_cast<long long>(s.taken));
    e.set("accuracy", accuracy);
    e.set("mispredict_rate", rate);
    e.set("mpki", mpki);
    e.set("cpi_contribution", cpi_cost);
    e.set("state_bits", static_cast<long long>(pred->state_bits()));
    e.set("wall_seconds", seconds);
    e.set("million_branches_per_second",
          seconds > 0 ? static_cast<double>(s.branches) / seconds / 1e6 : 0.0);
    arr.push_back(e);
  }
  out.set("results", arr);

  if (!o.json_path.empty()) {
    std::FILE* f = std::fopen(o.json_path.c_str(), "wb");
    if (f == nullptr) fail("cannot write JSON to " + o.json_path);
    const std::string text = out.dump(2);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fputc('\n', f);
    std::fclose(f);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options o = parse_args(argc, argv);

  if (o.help || o.command.empty()) {
    std::fputs(kUsage, o.help ? stdout : stderr);
    return o.help ? 0 : 2;
  }

  try {
    if (o.command == "run") return cmd_run(o);
    if (o.command == "branch") return cmd_branch(o);
    if (o.command == "config") return cmd_config(o);
    fail("unknown command '" + o.command + "' (expected run, branch or config)");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "cachescope: %s\n", e.what());
    return 1;
  }
}
