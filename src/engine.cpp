#include "cachescope/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace cachescope {
namespace {

std::string pct(double fraction) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%%", fraction * 100.0);
  return buf;
}

std::string fixed(double v, int places = 3) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.*f", places, v);
  return buf;
}

}  // namespace

Engine::Engine(const SimConfig& cfg)
    : cfg_(cfg),
      hierarchy_(cfg),
      predictor_(make_predictor(cfg.branch)),
      telemetry_(cfg.telemetry) {
  next_sample_at_ = cfg_.telemetry.interval_accesses;
}

// ---------------------------------------------------------------- branch pass

void Engine::step_branch(const BranchRef& ref) { predictor_->tick(ref.pc, ref.taken); }

void Engine::run_branch_trace(BranchTraceReader& reader) {
  BranchRef ref;
  while (reader.next(ref)) step_branch(ref);
}

double Engine::mispredict_rate() const {
  const BranchStats& bs = predictor_->stats();
  return safe_div(static_cast<double>(bs.mispredicts), static_cast<double>(bs.branches));
}

double Engine::branch_stall_cycles(double instructions) const {
  return instructions * cfg_.branch.density * mispredict_rate() *
         static_cast<double>(cfg_.timing.branch_mispredict_penalty);
}

// ---------------------------------------------------------------- memory pass

void Engine::step(const MemRef& ref) {
  ++records_;

  // Instructions attributable to this record.
  double instrs;
  if (cfg_.third_field == ThirdField::InstrGap) {
    // The gap counts non-memory instructions since the previous access; the
    // access itself is one more instruction.
    instrs = static_cast<double>(ref.instr_gap) + 1.0;
  } else {
    instrs = cfg_.timing.instructions_per_access;
  }
  instructions_ += instrs;

  const double base_cycles = instrs * cfg_.timing.base_cpi;
  cycles_.base += base_cycles;
  cycle_ += static_cast<Cycle>(base_cycles);

  if (cfg_.third_field == ThirdField::Size && hierarchy_.crosses_line(ref.addr, ref.size)) {
    hierarchy_.note_line_crossing();
  }

  const MemOutcome out = hierarchy_.access(ref.type, ref.addr, cycle_);
  ++accesses_;
  total_access_latency_ += out.total_latency;

  // An L1 hit is treated as fully pipelined. Anything deeper exposes every
  // latency it traversed, including the L1 lookup it wasted.
  if (!out.hit_l1) {
    const double scale = 1.0 - cfg_.timing.mlp_overlap;
    cycles_.l1_stall += static_cast<double>(out.l1_latency) * scale;
    cycles_.l2_stall += static_cast<double>(out.l2_latency) * scale;
    cycles_.llc_stall += static_cast<double>(out.llc_latency) * scale;
    cycles_.dram_latency_stall += static_cast<double>(out.dram_fixed_latency) * scale;
    cycles_.dram_queue_stall += static_cast<double>(out.dram_queue_delay) * scale;

    const double stall = static_cast<double>(out.total_latency) * scale;
    cycle_ += static_cast<Cycle>(stall);
  }

  if (telemetry_.sampling_enabled() && accesses_ >= next_sample_at_) {
    sample_interval();
    next_sample_at_ += cfg_.telemetry.interval_accesses;
  }
}

void Engine::run_memory_trace(MemTraceReader& reader) {
  MemRef ref;
  while (reader.next(ref)) step(ref);
}

void Engine::finish() {
  // Emit a final row so the time series always ends at the true totals, even
  // when the trace length is not a multiple of the sampling interval.
  if (telemetry_.sampling_enabled() && accesses_ > prev_.accesses) {
    sample_interval();
  }
}

void Engine::sample_interval() {
  MetricTable t = metrics();

  // Per-interval deltas sit alongside the monotonic counters: the cumulative
  // values answer "what happened overall", the deltas reveal phase behaviour.
  const double d_instr = instructions_ - prev_.instructions;
  const double total_cycles = cycles_.total() + branch_stall_cycles(instructions_);
  const double d_cycles = total_cycles - prev_.cycles;
  const Count d_dram_bytes = hierarchy_.dram().stats().bytes_total() - prev_.dram_bytes;

  const Cache& l1 = hierarchy_.level(0);
  const Cache& last = hierarchy_.level(hierarchy_.num_levels() - 1);
  const Count d_l1_acc = l1.stats().demand_accesses() - prev_.l1_accesses;
  const Count d_l1_miss = l1.stats().demand_misses() - prev_.l1_misses;
  const Count d_llc_miss = last.stats().demand_misses() - prev_.llc_misses;

  const double interval_cpi = safe_div(d_cycles, d_instr);
  const double interval_bw =
      hierarchy_.dram().bw_utilization_window(d_dram_bytes, static_cast<Cycle>(d_cycles));
  const double interval_llc_mpki = per_kilo_instr(static_cast<double>(d_llc_miss), d_instr);
  const double interval_l1_mr =
      safe_div(static_cast<double>(d_l1_miss), static_cast<double>(d_l1_acc));

  t.set("interval.index", static_cast<double>(interval_index_));
  t.set("interval.accesses", static_cast<double>(accesses_ - prev_.accesses));
  t.set("interval.instructions", d_instr);
  t.set("interval.cycles", d_cycles);
  t.set("interval.cpi", interval_cpi);
  t.set("interval.dram_bytes", static_cast<double>(d_dram_bytes));
  t.set("interval.dram_bw_utilization", interval_bw);
  t.set("interval.llc_mpki", interval_llc_mpki);
  t.set("interval.l1d_miss_rate", interval_l1_mr);

  telemetry_.write_row(t);

  IntervalSample s;
  s.index = interval_index_;
  s.accesses = accesses_;
  s.instructions = instructions_;
  s.interval_cpi = interval_cpi;
  s.dram_bw_utilization = interval_bw;
  s.llc_mpki = interval_llc_mpki;
  s.l1_miss_rate = interval_l1_mr;
  s.branch_mispredict_rate = mispredict_rate();
  telemetry_.record_sample(s);

  prev_.accesses = accesses_;
  prev_.instructions = instructions_;
  prev_.cycles = total_cycles;
  prev_.dram_bytes = hierarchy_.dram().stats().bytes_total();
  prev_.l1_accesses = l1.stats().demand_accesses();
  prev_.l1_misses = l1.stats().demand_misses();
  prev_.llc_misses = last.stats().demand_misses();
  ++interval_index_;
}

// ---------------------------------------------------------------- metrics

MetricTable Engine::metrics() const {
  MetricTable t;

  const double branch_stall = branch_stall_cycles(instructions_);
  const double total_cycles = cycles_.total() + branch_stall;

  t.set("sim.trace_records", static_cast<double>(records_));
  t.set("sim.accesses", static_cast<double>(accesses_));
  t.set("sim.instructions", instructions_);
  t.set("sim.cycles", total_cycles);
  t.set("sim.cpi", safe_div(total_cycles, instructions_));
  t.set("sim.ipc", safe_div(instructions_, total_cycles));
  t.set("sim.amat", safe_div(static_cast<double>(total_access_latency_),
                             static_cast<double>(accesses_)));
  t.set("sim.line_crossing_accesses", static_cast<double>(hierarchy_.line_crossing_accesses()));

  for (std::size_t i = 0; i < hierarchy_.num_levels(); ++i) {
    const Cache& c = hierarchy_.level(i);
    const LevelStats& s = c.stats();
    const std::string p = c.config().name + ".";

    t.set_count(p + "loads", s.loads);
    t.set_count(p + "stores", s.stores);
    t.set_count(p + "accesses", s.demand_accesses());
    t.set_count(p + "load_hits", s.load_hits);
    t.set_count(p + "load_misses", s.load_misses);
    t.set_count(p + "store_hits", s.store_hits);
    t.set_count(p + "store_misses", s.store_misses);
    t.set_count(p + "hits", s.demand_hits());
    t.set_count(p + "misses", s.demand_misses());
    t.set(p + "miss_rate", safe_div(static_cast<double>(s.demand_misses()),
                                    static_cast<double>(s.demand_accesses())));
    t.set(p + "load_miss_rate",
          safe_div(static_cast<double>(s.load_misses), static_cast<double>(s.loads)));
    t.set(p + "store_miss_rate",
          safe_div(static_cast<double>(s.store_misses), static_cast<double>(s.stores)));
    t.set(p + "mpki", per_kilo_instr(static_cast<double>(s.demand_misses()), instructions_));
    t.set_count(p + "fills", s.fills);
    t.set_count(p + "evictions", s.evictions);
    t.set_count(p + "dirty_evictions", s.dirty_evictions);
    t.set_count(p + "writebacks_received", s.writebacks_received);
    t.set_count(p + "back_invalidations", s.back_invalidations);
    t.set_count(p + "prefetch.issued", s.prefetch_issued);
    t.set_count(p + "prefetch.fills", s.prefetch_fills);
    t.set_count(p + "prefetch.useful", s.prefetch_useful);
    t.set_count(p + "prefetch.dropped", s.prefetch_dropped);
    t.set(p + "prefetch.accuracy", safe_div(static_cast<double>(s.prefetch_useful),
                                            static_cast<double>(s.prefetch_fills)));
    const double capacity_lines =
        static_cast<double>(c.num_sets()) * static_cast<double>(c.assoc());
    t.set(p + "occupancy", safe_div(static_cast<double>(c.valid_lines()), capacity_lines));
  }

  const DramStats& d = hierarchy_.dram().stats();
  t.set_count("dram.reads", d.reads);
  t.set_count("dram.writes", d.writes);
  t.set_count("dram.requests", d.requests());
  t.set_count("dram.bytes_read", d.bytes_read);
  t.set_count("dram.bytes_written", d.bytes_written);
  t.set_count("dram.bytes_total", d.bytes_total());
  t.set("dram.bw_utilization",
        hierarchy_.dram().bw_utilization(static_cast<Cycle>(total_cycles)));
  t.set_count("dram.queue_delay_cycles", d.queue_delay_cycles);
  t.set("dram.avg_queue_delay", safe_div(static_cast<double>(d.queue_delay_cycles),
                                         static_cast<double>(d.requests())));
  t.set_count("dram.busy_cycles", d.busy_cycles);
  t.set_count("dram.max_queue_depth", d.max_queue_depth);
  t.set_count("dram.requests_at_capacity", d.requests_at_capacity);
  t.set("dram.mpki", per_kilo_instr(static_cast<double>(d.requests()), instructions_));

  const BranchStats& b = predictor_->stats();
  t.set_count("branch.branches", b.branches);
  t.set_count("branch.mispredicts", b.mispredicts);
  t.set_count("branch.taken", b.taken);
  t.set("branch.taken_rate",
        safe_div(static_cast<double>(b.taken), static_cast<double>(b.branches)));
  t.set("branch.accuracy", b.branches == 0 ? 0.0 : 1.0 - mispredict_rate());
  t.set("branch.mispredict_rate", mispredict_rate());
  // Mispredicts per kilo-instruction of the *simulated workload*, derived from
  // the measured rate and the configured branch density.
  t.set("branch.mpki", 1000.0 * cfg_.branch.density * mispredict_rate());
  t.set("branch.cpi_contribution", safe_div(branch_stall, instructions_));
  t.set("branch.predictor_state_bits", static_cast<double>(predictor_->state_bits()));

  // Top-down buckets, as absolute cycles and as a share of the total.
  t.set("topdown.base_cycles", cycles_.base);
  t.set("topdown.l1_stall_cycles", cycles_.l1_stall);
  t.set("topdown.l2_stall_cycles", cycles_.l2_stall);
  t.set("topdown.llc_stall_cycles", cycles_.llc_stall);
  t.set("topdown.dram_latency_cycles", cycles_.dram_latency_stall);
  t.set("topdown.dram_queue_cycles", cycles_.dram_queue_stall);
  t.set("topdown.branch_cycles", branch_stall);
  t.set("topdown.base_pct", 100.0 * safe_div(cycles_.base, total_cycles));
  t.set("topdown.l1_stall_pct", 100.0 * safe_div(cycles_.l1_stall, total_cycles));
  t.set("topdown.l2_stall_pct", 100.0 * safe_div(cycles_.l2_stall, total_cycles));
  t.set("topdown.llc_stall_pct", 100.0 * safe_div(cycles_.llc_stall, total_cycles));
  t.set("topdown.dram_latency_pct", 100.0 * safe_div(cycles_.dram_latency_stall, total_cycles));
  t.set("topdown.dram_queue_pct", 100.0 * safe_div(cycles_.dram_queue_stall, total_cycles));
  t.set("topdown.branch_pct", 100.0 * safe_div(branch_stall, total_cycles));

  return t;
}

// ---------------------------------------------------------------- top-down

TopDownReport Engine::topdown() const {
  TopDownReport r;
  r.cycles = cycles_;
  r.cycles.branch_stall = branch_stall_cycles(instructions_);
  r.total_cycles = r.cycles.total();
  r.instructions = instructions_;
  r.cpi = safe_div(r.total_cycles, instructions_);
  r.ipc = safe_div(instructions_, r.total_cycles);
  r.amat = safe_div(static_cast<double>(total_access_latency_), static_cast<double>(accesses_));

  const double total = r.total_cycles;
  if (total <= 0.0) {
    r.diagnosis.headline = "No cycles simulated: the trace produced no accesses.";
    r.diagnosis.bottleneck = "none";
    return r;
  }

  struct Bucket {
    const char* label;
    const char* key;
    double cycles;
  };
  const Bucket buckets[] = {
      {"base work", "base", r.cycles.base},
      {"L1 stalls", "l1", r.cycles.l1_stall},
      {"L2 stalls", "l2", r.cycles.l2_stall},
      {"LLC stalls", "llc", r.cycles.llc_stall},
      {"DRAM latency stalls", "dram-latency", r.cycles.dram_latency_stall},
      {"DRAM bandwidth queueing", "dram-bandwidth", r.cycles.dram_queue_stall},
      {"branch mispredicts", "branch", r.cycles.branch_stall},
  };

  // The verdict names the dominant *stall*, not the dominant bucket. Base work
  // is the irreducible floor of the workload: a run that spends 51% of its
  // cycles retiring instructions and 49% waiting on DRAM is a memory problem,
  // and calling it "compute-bound" because 51 > 49 would send the reader off to
  // optimise the one thing they cannot remove.
  const double stall_cycles = r.cycles.memory_stall() + r.cycles.branch_stall;
  const double stall_share = stall_cycles / total;

  const Bucket* worst_stall = &buckets[1];
  for (const Bucket& b : buckets) {
    if (std::string(b.key) == "base") continue;
    if (b.cycles > worst_stall->cycles) worst_stall = &b;
  }

  const double overall_bw = hierarchy_.dram().bw_utilization(static_cast<Cycle>(total));

  // Locate the interval with the highest DRAM bandwidth utilisation. Phase
  // behaviour is the whole reason interval sampling exists, so the headline
  // names the phase rather than quoting only the run-wide average.
  const std::vector<IntervalSample>& samples = telemetry_.samples();
  const IntervalSample* peak = nullptr;
  for (const IntervalSample& s : samples) {
    if (peak == nullptr || s.dram_bw_utilization > peak->dram_bw_utilization) peak = &s;
  }
  const double peak_bw = peak != nullptr ? peak->dram_bw_utilization : overall_bw;

  // Bandwidth saturation is called out separately from the cycle buckets: a
  // workload can sit at the bandwidth ceiling during one phase while the
  // run-wide queueing average stays unremarkable.
  const bool bandwidth_saturated =
      std::string(worst_stall->key) == "dram-bandwidth" || peak_bw >= 0.80;

  std::ostringstream head;
  const double worst_share = worst_stall->cycles / total;
  if (stall_share < 0.15) {
    head << "Compute-bound: only " << pct(stall_share)
         << " of cycles are stalls, the rest is base work";
    r.diagnosis.bottleneck = "base";
  } else if (bandwidth_saturated) {
    head << "Memory-bound: DRAM bandwidth " << pct(peak_bw) << " utilized";
    if (peak != nullptr && samples.size() > 1) {
      head << " during phase " << (peak->index + 1) << " of " << samples.size();
    }
    r.diagnosis.bottleneck = "dram-bandwidth";
  } else {
    const std::string key = worst_stall->key;
    if (key == "dram-latency") {
      head << "Memory-bound on DRAM latency: " << pct(worst_share)
           << " of cycles waiting at " << hierarchy_.dram().config().latency_cycles
           << "-cycle memory latency, LLC MPKI "
           << fixed(per_kilo_instr(
                        static_cast<double>(
                            hierarchy_.level(hierarchy_.num_levels() - 1).stats().demand_misses()),
                        instructions_),
                    2);
    } else if (key == "llc" || key == "l2") {
      head << "Cache-capacity-bound: " << pct(worst_share) << " of cycles in "
           << worst_stall->label << ", so the working set overflows the level above";
    } else if (key == "branch") {
      head << "Branch-bound: " << pct(worst_share) << " of cycles lost to mispredicts at "
           << pct(1.0 - mispredict_rate()) << " predictor accuracy";
    } else {
      head << "L1-bound: " << pct(worst_share)
           << " of cycles in L1 miss handling, but misses are absorbed before DRAM";
    }
    r.diagnosis.bottleneck = key;
  }
  r.diagnosis.headline = head.str();

  {
    std::ostringstream os;
    os << "Stalls account for " << pct(stall_share) << " of cycles; base work is "
       << pct(r.cycles.base / total) << ".";
    r.diagnosis.findings.push_back(os.str());
  }

  // Supporting findings, ordered by cycle share.
  std::vector<const Bucket*> ordered;
  for (const Bucket& b : buckets) ordered.push_back(&b);
  std::sort(ordered.begin(), ordered.end(),
            [](const Bucket* a, const Bucket* b) { return a->cycles > b->cycles; });
  for (const Bucket* b : ordered) {
    if (b->cycles <= 0.0) continue;
    std::ostringstream os;
    os << b->label << ": " << pct(b->cycles / total) << " of cycles ("
       << static_cast<long long>(b->cycles) << " cycles)";
    r.diagnosis.findings.push_back(os.str());
  }

  const DramStats& d = hierarchy_.dram().stats();
  if (d.requests() > 0) {
    std::ostringstream os;
    os << "DRAM: " << d.requests() << " requests, " << d.bytes_total() << " bytes, "
       << pct(overall_bw) << " of peak bandwidth, average queue delay "
       << fixed(safe_div(static_cast<double>(d.queue_delay_cycles),
                         static_cast<double>(d.requests())), 2)
       << " cycles";
    r.diagnosis.findings.push_back(os.str());
  }
  if (peak != nullptr && samples.size() > 1) {
    std::ostringstream os;
    os << "Peak phase: interval " << (peak->index + 1) << " of " << samples.size() << " ran at CPI "
       << fixed(peak->interval_cpi) << " with DRAM bandwidth " << pct(peak->dram_bw_utilization)
       << " and LLC MPKI " << fixed(peak->llc_mpki, 2);
    r.diagnosis.findings.push_back(os.str());
  }
  if (predictor_->stats().branches > 0) {
    std::ostringstream os;
    os << "Branches: " << predictor_->stats().branches << " predicted by "
       << predictor_->name() << " at " << pct(1.0 - mispredict_rate()) << " accuracy ("
       << predictor_->state_bits() << " bits of state)";
    r.diagnosis.findings.push_back(os.str());
  }

  return r;
}

Json TopDownReport::to_json() const {
  Json j = Json::object();
  j.set("total_cycles", total_cycles);
  j.set("instructions", instructions);
  j.set("cpi", cpi);
  j.set("ipc", ipc);
  j.set("amat_cycles", amat);

  Json c = Json::object();
  c.set("base", cycles.base);
  c.set("l1_stall", cycles.l1_stall);
  c.set("l2_stall", cycles.l2_stall);
  c.set("llc_stall", cycles.llc_stall);
  c.set("dram_latency_stall", cycles.dram_latency_stall);
  c.set("dram_queue_stall", cycles.dram_queue_stall);
  c.set("branch_stall", cycles.branch_stall);
  j.set("cycles", c);

  Json pcts = Json::object();
  const double t = total_cycles <= 0 ? 1.0 : total_cycles;
  pcts.set("base", 100.0 * cycles.base / t);
  pcts.set("l1_stall", 100.0 * cycles.l1_stall / t);
  pcts.set("l2_stall", 100.0 * cycles.l2_stall / t);
  pcts.set("llc_stall", 100.0 * cycles.llc_stall / t);
  pcts.set("dram_latency_stall", 100.0 * cycles.dram_latency_stall / t);
  pcts.set("dram_queue_stall", 100.0 * cycles.dram_queue_stall / t);
  pcts.set("branch_stall", 100.0 * cycles.branch_stall / t);
  j.set("percent_of_cycles", pcts);

  Json d = Json::object();
  d.set("headline", diagnosis.headline);
  d.set("bottleneck", diagnosis.bottleneck);
  Json f = Json::array();
  for (const std::string& s : diagnosis.findings) f.push_back(s);
  d.set("findings", f);
  j.set("diagnosis", d);

  return j;
}

Json Engine::summary() const {
  Json j = Json::object();
  j.set("config", cfg_.to_json());
  j.set("counters", metrics().to_json());
  j.set("topdown", topdown().to_json());

  Json intervals = Json::array();
  for (const IntervalSample& s : telemetry_.samples()) {
    Json o = Json::object();
    o.set("index", static_cast<long long>(s.index));
    o.set("accesses", static_cast<long long>(s.accesses));
    o.set("instructions", s.instructions);
    o.set("cpi", s.interval_cpi);
    o.set("dram_bw_utilization", s.dram_bw_utilization);
    o.set("llc_mpki", s.llc_mpki);
    o.set("l1d_miss_rate", s.l1_miss_rate);
    intervals.push_back(o);
  }
  j.set("intervals", intervals);
  return j;
}

// ---------------------------------------------------------------- text report

void Engine::print_report(std::FILE* out) const {
  const TopDownReport r = topdown();
  const MetricTable t = metrics();

  std::fprintf(out, "cachescope: %s\n", cfg_.name.c_str());
  std::fprintf(out, "%s\n", std::string(72, '=').c_str());
  std::fprintf(out, "trace records   %12llu\n", static_cast<unsigned long long>(records_));
  std::fprintf(out, "instructions    %12.0f\n", instructions_);
  std::fprintf(out, "cycles          %12.0f\n", r.total_cycles);
  std::fprintf(out, "CPI             %12.4f   (IPC %.4f)\n", r.cpi, r.ipc);
  std::fprintf(out, "AMAT            %12.4f cycles\n", r.amat);
  std::fprintf(out, "\n");

  std::fprintf(out, "%-6s %12s %12s %10s %10s %12s\n", "level", "accesses", "misses", "miss%",
               "MPKI", "writebacks");
  for (std::size_t i = 0; i < hierarchy_.num_levels(); ++i) {
    const Cache& c = hierarchy_.level(i);
    const LevelStats& s = c.stats();
    std::fprintf(out, "%-6s %12llu %12llu %9.2f%% %10.3f %12llu\n", c.config().name.c_str(),
                 static_cast<unsigned long long>(s.demand_accesses()),
                 static_cast<unsigned long long>(s.demand_misses()),
                 100.0 * safe_div(static_cast<double>(s.demand_misses()),
                                  static_cast<double>(s.demand_accesses())),
                 per_kilo_instr(static_cast<double>(s.demand_misses()), instructions_),
                 static_cast<unsigned long long>(s.dirty_evictions));
  }
  std::fprintf(out, "\n");

  const DramStats& d = hierarchy_.dram().stats();
  std::fprintf(out, "DRAM  requests %llu  read %llu B  write %llu B  bw %.1f%%  avg queue %.2f cyc\n",
               static_cast<unsigned long long>(d.requests()),
               static_cast<unsigned long long>(d.bytes_read),
               static_cast<unsigned long long>(d.bytes_written),
               100.0 * t.get("dram.bw_utilization"), t.get("dram.avg_queue_delay"));

  if (predictor_->stats().branches > 0) {
    std::fprintf(out, "Branch %s  %llu branches  accuracy %.3f%%  CPI contribution %.4f\n",
                 predictor_->name(),
                 static_cast<unsigned long long>(predictor_->stats().branches),
                 100.0 * t.get("branch.accuracy"), t.get("branch.cpi_contribution"));
  }
  std::fprintf(out, "\n");

  std::fprintf(out, "Top-down cycle breakdown\n");
  std::fprintf(out, "%s\n", std::string(72, '-').c_str());
  struct Row { const char* label; double cycles; };
  const Row rows[] = {
      {"base work", r.cycles.base},
      {"L1 stall", r.cycles.l1_stall},
      {"L2 stall", r.cycles.l2_stall},
      {"LLC stall", r.cycles.llc_stall},
      {"DRAM latency", r.cycles.dram_latency_stall},
      {"DRAM bandwidth queue", r.cycles.dram_queue_stall},
      {"branch mispredict", r.cycles.branch_stall},
  };
  for (const Row& row : rows) {
    const double share = safe_div(row.cycles, r.total_cycles);
    const int bars = static_cast<int>(share * 40.0 + 0.5);
    std::fprintf(out, "%-22s %6.2f%% %14.0f  %s\n", row.label, share * 100.0, row.cycles,
                 std::string(static_cast<std::size_t>(bars), '#').c_str());
  }
  std::fprintf(out, "\n");

  std::fprintf(out, "Diagnosis: %s\n", r.diagnosis.headline.c_str());
  for (const std::string& f : r.diagnosis.findings) {
    std::fprintf(out, "  - %s\n", f.c_str());
  }
}

}  // namespace cachescope
