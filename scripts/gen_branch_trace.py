#!/usr/bin/env python3
"""Generate branch traces in cachescope's format: "<hex pc> <t|n>" per line.

The patterns are chosen so that the four predictors separate from each other.
A trace made only of loop branches makes every predictor look perfect; a trace
made only of coin flips makes them all look equally bad. The interesting cases
are the ones in between:

  loop        A backward branch taken (trip-1) times then falling through.
              Any predictor with a saturating counter handles this.
  alternating Strict T,N,T,N. Invisible to bimodal (its single counter just
              oscillates) and trivial for anything with one bit of history.
  periodic    A fixed repeating pattern of period `period`. Needs at least
              `period` bits of history to be learned exactly.
  correlated  Two branches where the second always follows the first. Only a
              predictor with global history can exploit it.
  biased      Random with a fixed taken probability. Saturating counters settle
              on the majority direction; the residual error is irreducible.
  random      Unbiased coin flips. Nothing can beat 50%, so this is the floor.
  mixed       All of the above interleaved at realistic proportions. This is
              the default and the one worth quoting accuracy numbers from.

Usage:
  python3 scripts/gen_branch_trace.py --pattern mixed --count 200000 \
      --out traces/branch/sample.trace
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

# Base PC for synthesised branches. Byte-granular and deliberately not
# 4-byte-aligned everywhere, so that predictors indexing on low PC bits behave
# as they would on x86 rather than getting an artificially clean spread.
PC_BASE = 0x400000


def pc_for(slot: int) -> int:
    """A distinct, non-uniformly-spaced PC per logical branch site."""
    return PC_BASE + slot * 0x17 + (slot % 3)


def gen_loop(n: int, rng: random.Random, trip: int = 20) -> list[tuple[int, bool]]:
    pc = pc_for(1)
    out = []
    while len(out) < n:
        for i in range(trip):
            out.append((pc, i != trip - 1))
            if len(out) >= n:
                break
    return out[:n]


def gen_alternating(n: int, rng: random.Random) -> list[tuple[int, bool]]:
    pc = pc_for(2)
    return [(pc, i % 2 == 0) for i in range(n)]


def gen_periodic(n: int, rng: random.Random, period: int = 5) -> list[tuple[int, bool]]:
    pc = pc_for(3)
    # A fixed, non-trivial pattern rather than a run of T followed by a run of N.
    pattern = [(i * i) % period < period // 2 for i in range(period)]
    return [(pc, pattern[i % period]) for i in range(n)]


def gen_correlated(n: int, rng: random.Random) -> list[tuple[int, bool]]:
    # Models:  flag = cond();  if (flag) {...}  ... later ...  if (flag) {...}
    # The second branch is perfectly predictable from the first, but only if the
    # predictor remembers what the first one did.
    pc_a, pc_b = pc_for(4), pc_for(5)
    out = []
    while len(out) < n:
        flag = rng.random() < 0.5
        out.append((pc_a, flag))
        out.append((pc_b, flag))
    return out[:n]


def gen_biased(n: int, rng: random.Random, p_taken: float = 0.9) -> list[tuple[int, bool]]:
    pc = pc_for(6)
    return [(pc, rng.random() < p_taken) for _ in range(n)]


def gen_random(n: int, rng: random.Random) -> list[tuple[int, bool]]:
    pc = pc_for(7)
    return [(pc, rng.random() < 0.5) for _ in range(n)]


def gen_sorted_vs_unsorted(n: int, rng: random.Random) -> list[tuple[int, bool]]:
    """The classic 'why is processing a sorted array faster' branch.

    First half walks sorted data, so the data-dependent branch is taken for a
    long run and then not taken for a long run. Second half walks the same
    values shuffled, making the same branch unpredictable.
    """
    pc = pc_for(8)
    half = n // 2
    values = [rng.randrange(256) for _ in range(half)]
    out = [(pc, v >= 128) for v in sorted(values)]
    rng.shuffle(values)
    out += [(pc, v >= 128) for v in values]
    return out[:n]


GENERATORS = {
    "loop": gen_loop,
    "alternating": gen_alternating,
    "periodic": gen_periodic,
    "correlated": gen_correlated,
    "biased": gen_biased,
    "random": gen_random,
    "sorted-vs-unsorted": gen_sorted_vs_unsorted,
}

# Proportions for the "mixed" pattern. Loop branches dominate real code, which
# is why overall accuracy is high even when some sites are badly predicted.
MIX = [
    ("loop", 0.40),
    ("correlated", 0.18),
    ("periodic", 0.12),
    ("biased", 0.12),
    ("alternating", 0.08),
    ("sorted-vs-unsorted", 0.06),
    ("random", 0.04),
]


def gen_mixed(n: int, rng: random.Random) -> list[tuple[int, bool]]:
    # Build each component stream, then interleave them in small bursts so the
    # trace looks like a program moving between regions of code rather than a
    # concatenation of unrelated phases.
    streams = []
    for name, share in MIX:
        count = max(1, int(n * share))
        streams.append(GENERATORS[name](count, rng))

    out: list[tuple[int, bool]] = []
    cursors = [0] * len(streams)
    weights = [share for _, share in MIX]
    while len(out) < n:
        live = [i for i, s in enumerate(streams) if cursors[i] < len(s)]
        if not live:
            break
        i = rng.choices(live, weights=[weights[j] for j in live])[0]
        burst = rng.randint(4, 24)
        chunk = streams[i][cursors[i]:cursors[i] + burst]
        cursors[i] += len(chunk)
        out.extend(chunk)
    return out[:n]


GENERATORS["mixed"] = gen_mixed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pattern", default="mixed", choices=sorted(GENERATORS),
                    help="branch behaviour to synthesise (default: mixed)")
    ap.add_argument("--count", type=int, default=200_000, help="number of branches")
    ap.add_argument("--seed", type=int, default=20251028,
                    help="PRNG seed; the same seed always produces the same trace")
    ap.add_argument("--out", type=Path, required=True, help="output trace path ('-' for stdout)")
    args = ap.parse_args()

    if args.count <= 0:
        print("error: --count must be positive", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)
    branches = GENERATORS[args.pattern](args.count, rng)

    header = (f"# cachescope branch trace\n"
              f"# pattern={args.pattern} count={len(branches)} seed={args.seed}\n"
              f"# format: <hex pc> <t|n>\n")
    body = "".join(f"0x{pc:x} {'t' if taken else 'n'}\n" for pc, taken in branches)

    if str(args.out) == "-":
        sys.stdout.write(header + body)
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(header + body)
        taken = sum(1 for _, t in branches if t)
        print(f"wrote {args.out}: {len(branches)} branches, "
              f"{taken} taken ({100.0 * taken / len(branches):.1f}%), "
              f"pattern={args.pattern}, seed={args.seed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
