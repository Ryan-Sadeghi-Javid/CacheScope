#!/usr/bin/env python3
"""Parameter sweep over cachescope configurations, writing one CSV of results.

Every sweep axis is a dotted config path and a list of values, exactly the
syntax `cachescope --set` accepts. Multiple axes form a cartesian product, so a
single invocation can cover a whole design space:

  python3 scripts/sweep.py --cachescope build/cachescope \\
      --config configs/baseline.json --trace traces/upstream/swim.trace \\
      --sweep levels.1.size_kb=128,256,512,1024,2048 \\
      --sweep levels.1.assoc=4,8,16 \\
      --out analysis/sweep_l2.csv

Each row carries the swept values alongside the metrics, so the CSV is directly
plottable without rejoining anything.

Runs are independent, so they are executed in parallel across cores. Results are
sorted back into deterministic order before writing, which keeps the CSV stable
across runs and makes diffs meaningful.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import itertools
import json
import subprocess
import sys
import tempfile
from pathlib import Path

# Metrics lifted into every row. Level-prefixed names are resolved per config,
# since level names are user-chosen.
SCALAR_METRICS = [
    "sim.instructions", "sim.accesses", "sim.cycles", "sim.cpi", "sim.ipc", "sim.amat",
    "dram.reads", "dram.writes", "dram.bytes_read", "dram.bytes_written",
    "dram.bw_utilization", "dram.avg_queue_delay",
    "branch.branches", "branch.mispredicts", "branch.accuracy", "branch.mpki",
    "branch.cpi_contribution", "branch.predictor_state_bits",
    "topdown.base_pct", "topdown.l1_stall_pct", "topdown.l2_stall_pct",
    "topdown.llc_stall_pct", "topdown.dram_latency_pct", "topdown.dram_queue_pct",
    "topdown.branch_pct",
]

PER_LEVEL_METRICS = ["accesses", "misses", "miss_rate", "mpki", "load_miss_rate",
                     "dirty_evictions", "prefetch.issued", "prefetch.useful",
                     "prefetch.accuracy", "occupancy"]


def parse_sweep(spec: str) -> tuple[str, list[str]]:
    """Parses 'levels.1.size_kb=128,256,512' into a path and a value list."""
    if "=" not in spec:
        raise ValueError(f"--sweep needs path=v1,v2,...; got {spec!r}")
    path, values = spec.split("=", 1)
    path = path.strip()
    items = [v.strip() for v in values.split(",") if v.strip()]
    if not path or not items:
        raise ValueError(f"--sweep needs a path and at least one value; got {spec!r}")
    return path, items


def run_point(args_tuple) -> dict | None:
    binary, config, trace, branch_trace, third_field, assignments, extra = args_tuple
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "summary.json"
        cmd = [str(binary), "run", "-c", str(config), "-t", str(trace),
               "-q", "--json", str(out)]
        if branch_trace:
            cmd += ["--branch-trace", str(branch_trace)]
        if third_field:
            cmd += ["--third-field", third_field]
        for path, value in assignments:
            cmd += ["--set", f"{path}={value}"]
        for e in extra:
            cmd += ["--set", e]

        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            # An invalid geometry (for example a size that is not a multiple of
            # ways*block) is a legitimate sweep outcome, not a crash. Record it
            # and carry on rather than aborting the whole grid.
            return {
                "status": "invalid",
                "error": proc.stderr.strip().splitlines()[-1][:200] if proc.stderr else "unknown",
                **{path: value for path, value in assignments},
            }
        summary = json.loads(out.read_text())

    counters = summary["counters"]
    row: dict = {"status": "ok", "error": ""}
    for path, value in assignments:
        row[path] = value

    for m in SCALAR_METRICS:
        row[m] = counters.get(m)

    for level in summary["config"]["levels"]:
        name = level["name"]
        row[f"{name}.size_bytes"] = level["size_bytes"]
        row[f"{name}.assoc"] = level["assoc"]
        row[f"{name}.block_size"] = level["block_size"]
        row[f"{name}.replacement"] = level["replacement"]
        row[f"{name}.prefetcher"] = level["prefetcher"]["kind"]
        for m in PER_LEVEL_METRICS:
            row[f"{name}.{m}"] = counters.get(f"{name}.{m}")

    row["predictor"] = summary["config"]["branch"]["predictor"]
    row["bottleneck"] = summary["topdown"]["diagnosis"]["bottleneck"]
    row["diagnosis"] = summary["topdown"]["diagnosis"]["headline"]
    row["wall_seconds"] = summary.get("performance", {}).get("wall_seconds")
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cachescope", required=True, type=Path)
    ap.add_argument("--config", required=True, type=Path)
    ap.add_argument("--trace", required=True, type=Path)
    ap.add_argument("--branch-trace", type=Path,
                    help="branch trace; required for any sweep over branch.predictor, "
                         "since without branches every predictor costs zero cycles")
    ap.add_argument("--third-field", default="", choices=["", "size", "instr_gap", "ignore"])
    ap.add_argument("--sweep", action="append", default=[], metavar="PATH=V1,V2,...",
                    help="sweep axis, repeatable; axes form a cartesian product")
    ap.add_argument("--set", action="append", default=[], dest="extra", metavar="PATH=VALUE",
                    help="fixed override applied to every point, repeatable")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--jobs", type=int, default=0, help="parallel runs (0 = CPU count)")
    args = ap.parse_args()

    for p in (args.cachescope, args.config, args.trace):
        if not p.exists():
            print(f"error: {p} does not exist", file=sys.stderr)
            return 2
    if args.branch_trace and not args.branch_trace.exists():
        print(f"error: {args.branch_trace} does not exist", file=sys.stderr)
        return 2
    if not args.sweep:
        print("error: at least one --sweep axis is required", file=sys.stderr)
        return 2

    try:
        axes = [parse_sweep(s) for s in args.sweep]
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    paths = [p for p, _ in axes]
    combos = list(itertools.product(*[vals for _, vals in axes]))
    jobs = args.jobs or None

    print(f"sweeping {len(combos)} configurations over {len(axes)} axes:", file=sys.stderr)
    for path, vals in axes:
        print(f"  {path} = {','.join(vals)}", file=sys.stderr)
    print(f"trace: {args.trace}", file=sys.stderr)

    # Guard against the easiest way to produce a meaningless predictor sweep.
    if any(p.startswith("branch.") for p in paths) and not args.branch_trace:
        print("error: sweeping a branch.* parameter without --branch-trace would give "
              "every point an identical result, because a memory trace contains no "
              "branches. Supply --branch-trace.", file=sys.stderr)
        return 2

    work = [(args.cachescope, args.config, args.trace, args.branch_trace, args.third_field,
             list(zip(paths, combo)), args.extra) for combo in combos]

    rows: list[dict] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        for i, row in enumerate(pool.map(run_point, work), start=1):
            if row is not None:
                rows.append(row)
            if i % 10 == 0 or i == len(work):
                print(f"  {i}/{len(work)} done", file=sys.stderr)

    if not rows:
        print("error: no results produced", file=sys.stderr)
        return 1

    # Stable column order: swept axes first, then everything else alphabetically.
    import csv
    seen: list[str] = ["status", "error", *paths]
    for row in rows:
        for k in row:
            if k not in seen:
                seen.append(k)
    rest = sorted(k for k in seen if k not in ("status", "error", *paths))
    fieldnames = ["status", "error", *paths, *rest]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        for row in rows:
            w.writerow(row)

    ok = [r for r in rows if r.get("status") == "ok"]
    bad = len(rows) - len(ok)
    print(f"\nwrote {args.out}: {len(ok)} valid configurations"
          + (f", {bad} rejected" if bad else ""), file=sys.stderr)

    if ok:
        best = min(ok, key=lambda r: r.get("sim.cpi") or float("inf"))
        worst = max(ok, key=lambda r: r.get("sim.cpi") or 0.0)
        def label(r):
            return " ".join(f"{p}={r[p]}" for p in paths)
        print(f"best  CPI {best['sim.cpi']:.4f}  {label(best)}", file=sys.stderr)
        print(f"worst CPI {worst['sim.cpi']:.4f}  {label(worst)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
