#!/usr/bin/env python3
"""Cross-validate cachescope against kev-kim/cache-simulator.

The upstream simulator is the reference this project claims trace-format
compatibility with, so its load/store hit and miss counts are a hard
correctness oracle for the single-level case. This script configures cachescope
to match an upstream geometry exactly, runs both on the same trace, and
compares all six counters.

Any mismatch is a bug in cachescope, not a rounding difference: both are
counting discrete events over an identical input.

Usage:
  python3 scripts/validate_upstream.py \
      --csim ~/src/cache-simulator/csim \
      --cachescope build/cachescope \
      --traces ~/src/cache-simulator/tests

Exit status is non-zero if any configuration disagrees.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

# (sets, ways, block_bytes, alloc, write, replacement)
GEOMETRIES = [
    (256, 4, 16, "write-allocate", "write-back", "lru"),
    (256, 4, 16, "write-allocate", "write-back", "fifo"),
    (256, 1, 16, "write-allocate", "write-back", "lru"),      # direct-mapped
    (1, 256, 16, "write-allocate", "write-back", "lru"),      # fully associative
    (256, 4, 16, "write-allocate", "write-through", "lru"),
    (256, 4, 16, "no-write-allocate", "write-through", "lru"),
    (128, 8, 32, "write-allocate", "write-back", "lru"),
    (64, 2, 64, "write-allocate", "write-back", "fifo"),
]

TRACES = ["read01", "read02", "read03", "write01", "write02", "gcc", "swim"]

UPSTREAM_FIELDS = {
    "Total loads": "loads",
    "Total stores": "stores",
    "Load hits": "load_hits",
    "Load misses": "load_misses",
    "Store hits": "store_hits",
    "Store misses": "store_misses",
}


@dataclass
class Result:
    trace: str
    geometry: str
    ok: bool
    detail: str


def parse_upstream(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z ]+):\s*(\d+)\s*$", line.strip())
        if not m:
            continue
        label, value = m.group(1).strip(), int(m.group(2))
        if label in UPSTREAM_FIELDS:
            out[UPSTREAM_FIELDS[label]] = value
    return out


def run_upstream(csim: Path, trace: Path, sets: int, ways: int, block: int,
                 alloc: str, write: str, repl: str) -> dict[str, int]:
    with trace.open("rb") as f:
        proc = subprocess.run(
            [str(csim), str(sets), str(ways), str(block), alloc, write, repl],
            stdin=f, capture_output=True, text=True, check=True)
    parsed = parse_upstream(proc.stdout)
    missing = set(UPSTREAM_FIELDS.values()) - set(parsed)
    if missing:
        raise RuntimeError(f"upstream output missing fields {sorted(missing)}:\n{proc.stdout}")
    return parsed


def run_cachescope(binary: Path, trace: Path, sets: int, ways: int, block: int,
                   alloc: str, write: str, repl: str) -> dict[str, int]:
    cfg = {
        "name": f"compat-{sets}x{ways}x{block}",
        # Upstream ignores the third trace field entirely; matching that keeps
        # the comparison about cache behaviour rather than trace interpretation.
        "third_field": "ignore",
        "levels": [{
            "name": "l1d",
            "size_bytes": sets * ways * block,
            "assoc": ways,
            "block_size": block,
            "hit_latency": 1,
            "write_policy": write,
            "alloc_policy": alloc,
            "replacement": repl,
        }],
        "dram": {"latency_cycles": 100, "bytes_per_cycle": 8.0},
        "timing": {"base_cpi": 1.0, "instructions_per_access": 1.0},
    }
    with tempfile.TemporaryDirectory() as td:
        cfg_path = Path(td) / "cfg.json"
        out_path = Path(td) / "out.json"
        cfg_path.write_text(json.dumps(cfg))
        subprocess.run(
            [str(binary), "run", "-c", str(cfg_path), "-t", str(trace),
             "--json", str(out_path), "-q"],
            capture_output=True, text=True, check=True)
        counters = json.loads(out_path.read_text())["counters"]
    return {
        "loads": int(counters["l1d.loads"]),
        "stores": int(counters["l1d.stores"]),
        "load_hits": int(counters["l1d.load_hits"]),
        "load_misses": int(counters["l1d.load_misses"]),
        "store_hits": int(counters["l1d.store_hits"]),
        "store_misses": int(counters["l1d.store_misses"]),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csim", required=True, type=Path,
                    help="path to the built upstream csim binary")
    ap.add_argument("--cachescope", required=True, type=Path,
                    help="path to the built cachescope binary")
    ap.add_argument("--traces", required=True, type=Path,
                    help="directory holding the upstream .trace files")
    ap.add_argument("--markdown", type=Path,
                    help="also write a Markdown results table here")
    args = ap.parse_args()

    for p in (args.csim, args.cachescope):
        if not p.exists():
            print(f"error: {p} does not exist", file=sys.stderr)
            return 2

    results: list[Result] = []
    for trace_name in TRACES:
        trace = args.traces / f"{trace_name}.trace"
        if not trace.exists():
            print(f"skipping {trace_name}: {trace} not found", file=sys.stderr)
            continue
        for (sets, ways, block, alloc, write, repl) in GEOMETRIES:
            alloc_short = "wa" if alloc == "write-allocate" else "nwa"
            write_short = "wb" if write == "write-back" else "wt"
            label = f"{sets}x{ways}x{block}B {alloc_short}/{write_short}/{repl}"
            try:
                want = run_upstream(args.csim, trace, sets, ways, block, alloc, write, repl)
                got = run_cachescope(args.cachescope, trace, sets, ways, block,
                                     alloc, write, repl)
            except subprocess.CalledProcessError as e:
                results.append(Result(trace_name, label, False,
                                      f"process failed: {e.stderr.strip()[:200]}"))
                continue
            except RuntimeError as e:
                results.append(Result(trace_name, label, False, str(e)[:200]))
                continue

            diffs = [f"{k}: upstream={want[k]} cachescope={got[k]}"
                     for k in sorted(want) if want[k] != got[k]]
            results.append(Result(trace_name, label, not diffs, "; ".join(diffs)))

    width = max((len(r.geometry) for r in results), default=10)
    print(f"{'trace':<10} {'geometry':<{width}} result")
    print("-" * (12 + width + 8))
    failures = 0
    for r in results:
        status = "ok" if r.ok else "MISMATCH"
        print(f"{r.trace:<10} {r.geometry:<{width}} {status}")
        if not r.ok:
            failures += 1
            print(f"           -> {r.detail}")

    total = len(results)
    print()
    print(f"{total - failures}/{total} configurations match upstream exactly")

    if args.markdown:
        lines = ["| trace | geometry | load/store hit+miss counts |",
                 "|---|---|---|"]
        for r in results:
            lines.append(f"| {r.trace} | `{r.geometry}` | "
                         f"{'exact match' if r.ok else 'MISMATCH: ' + r.detail} |")
        lines.append("")
        lines.append(f"**{total - failures} of {total} configurations match "
                     f"kev-kim/cache-simulator exactly.**")
        args.markdown.write_text("\n".join(lines) + "\n")
        print(f"wrote {args.markdown}")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
