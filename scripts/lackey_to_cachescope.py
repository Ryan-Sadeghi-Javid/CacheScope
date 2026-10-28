#!/usr/bin/env python3
"""Convert Valgrind lackey --trace-mem output into cachescope trace format.

Lackey emits one record per memory reference:

    I  0401f540,3        instruction fetch at 0x401f540, 3 bytes
     L 1fff000108,8      data load
     S 1fff000100,8      data store
     M 1fff0000c8,8      modify: a load and a store to the same address

cachescope simulates a data cache, so instruction fetches are not emitted as
accesses. They are far from useless though: counting the `I` records between
consecutive data references gives the exact number of non-memory instructions
executed in between, which is precisely what the third trace field means in
`instr_gap` mode. That turns an estimated instruction count into a measured
one, and so turns an estimated CPI into a much better grounded one.

An `M` record becomes two cachescope records, a load then a store, because that
is what the cache actually sees.

Output format (compatible with kev-kim/cache-simulator):

    l 0x1fff000108 8

Usage:
  valgrind --tool=lackey --trace-mem=yes --log-file=raw.txt ./bench/stream 1024 1
  python3 scripts/lackey_to_cachescope.py raw.txt -o traces/bench/stream.trace

  # then, to use the measured instruction counts:
  cachescope run -t traces/bench/stream.trace --third-field instr_gap
"""

from __future__ import annotations

import argparse
import gzip
import re
import sys
from pathlib import Path

# Lackey trace lines. Addresses are bare hex; sizes are decimal.
RECORD = re.compile(r"^\s*([ILSM])\s+([0-9a-fA-F]+),(\d+)\s*$")


def open_maybe_gzip(path: Path, mode: str):
    if str(path) == "-":
        return sys.stdin if "r" in mode else sys.stdout
    if path.suffix == ".gz":
        return gzip.open(path, mode + "t")
    return open(path, mode)


def convert(src, dst, third_field: str, limit: int, skip: int) -> dict[str, int]:
    stats = {
        "lines": 0,
        "instr_fetches": 0,
        "loads": 0,
        "stores": 0,
        "modifies": 0,
        "records_written": 0,
        "unparsed": 0,
    }
    instr_gap = 0
    written = 0
    skipped = 0

    write = dst.write
    for line in src:
        stats["lines"] += 1
        m = RECORD.match(line)
        if m is None:
            # Valgrind's own ==pid== banner lines and anything else.
            if line.lstrip().startswith("=="):
                continue
            stats["unparsed"] += 1
            continue

        op, addr_hex, size_str = m.group(1), m.group(2), m.group(3)

        if op == "I":
            stats["instr_fetches"] += 1
            instr_gap += 1
            continue

        # Skip the program's start-up references if asked: dynamic-loader and
        # libc initialisation can dominate a short benchmark's trace.
        if skipped < skip:
            skipped += 1
            instr_gap = 0
            continue

        size = int(size_str)
        third = instr_gap if third_field == "instr_gap" else size
        instr_gap = 0

        if op == "L":
            stats["loads"] += 1
            write(f"l 0x{addr_hex} {third}\n")
            written += 1
        elif op == "S":
            stats["stores"] += 1
            write(f"s 0x{addr_hex} {third}\n")
            written += 1
        else:  # 'M' = read-modify-write: the cache sees a load then a store.
            stats["modifies"] += 1
            write(f"l 0x{addr_hex} {third}\n")
            # The store half follows immediately, with no instructions between.
            write(f"s 0x{addr_hex} {0 if third_field == 'instr_gap' else size}\n")
            written += 2

        if limit and written >= limit:
            break

    stats["records_written"] = written
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path,
                    help="lackey log file (may be .gz), or - for standard input")
    ap.add_argument("-o", "--out", type=Path, required=True,
                    help="output trace path (may be .gz), or - for standard output")
    ap.add_argument("--third-field", choices=["instr_gap", "size"], default="instr_gap",
                    help="what to put in the third column (default: instr_gap, the measured "
                         "instruction count between accesses)")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N output records (0 = no limit)")
    ap.add_argument("--skip", type=int, default=0,
                    help="discard the first N data references, to drop process start-up")
    args = ap.parse_args()

    if not (str(args.input) == "-" or args.input.exists()):
        print(f"error: {args.input} does not exist", file=sys.stderr)
        return 2

    if str(args.out) != "-":
        args.out.parent.mkdir(parents=True, exist_ok=True)

    src = open_maybe_gzip(args.input, "r")
    dst = open_maybe_gzip(args.out, "w")
    try:
        stats = convert(src, dst, args.third_field, args.limit, args.skip)
    finally:
        if src is not sys.stdin:
            src.close()
        if dst is not sys.stdout:
            dst.close()

    total_refs = stats["loads"] + stats["stores"] + stats["modifies"]
    print(f"{args.input} -> {args.out}", file=sys.stderr)
    print(f"  lackey lines      {stats['lines']}", file=sys.stderr)
    print(f"  instruction fetch {stats['instr_fetches']}", file=sys.stderr)
    print(f"  loads             {stats['loads']}", file=sys.stderr)
    print(f"  stores            {stats['stores']}", file=sys.stderr)
    print(f"  modifies (L+S)    {stats['modifies']}", file=sys.stderr)
    print(f"  records written   {stats['records_written']}", file=sys.stderr)
    if stats["unparsed"]:
        print(f"  unparsed lines    {stats['unparsed']}", file=sys.stderr)
    if total_refs:
        ratio = stats["instr_fetches"] / total_refs
        print(f"  instructions per data reference {ratio:.3f}", file=sys.stderr)
    if args.third_field == "instr_gap":
        print("  run with: cachescope run --third-field instr_gap", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
