#!/usr/bin/env bash
# Measure simulator throughput in millions of memory accesses per second.
#
# The upstream traces are only a few hundred thousand records, which run in well
# under a tenth of a second: far too short to measure stably against process
# start-up and page-cache effects. This script concatenates the trace until it
# is large enough that a run takes roughly a second, then reports the best of N
# repetitions. The best, not the mean: it is the run least disturbed by
# scheduling noise, and throughput is a capability claim, not an average.
#
# Usage: scripts/bench_throughput.sh [binary] [trace] [repeats] [copies]
set -u

BIN="${1:-build/cachescope}"
TRACE="${2:-traces/upstream/gcc.trace}"
REPEATS="${3:-5}"
COPIES="${4:-10}"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN is not executable" >&2
  exit 1
fi
if [ ! -f "$TRACE" ]; then
  echo "error: trace $TRACE not found (run scripts/fetch_traces.sh)" >&2
  exit 1
fi

BIG="$(mktemp)"
trap 'rm -f "$BIG"' EXIT
for _ in $(seq 1 "$COPIES"); do
  cat "$TRACE" >> "$BIG"
done

records=$(wc -l < "$BIG")
echo "binary   : $BIN" >&2
echo "trace    : $TRACE x $COPIES = $records records" >&2
echo "repeats  : $REPEATS" >&2
echo >&2

best_rate=0
best_secs=0
for i in $(seq 1 "$REPEATS"); do
  line="$("$BIN" run -t "$BIG" 2>/dev/null | tail -1)"
  secs="$(printf '%s' "$line" | sed -n 's/.* in \([0-9.][0-9.]*\) s .*/\1/p')"
  rate="$(printf '%s' "$line" | sed -n 's/.*(\([0-9.][0-9.]*\) M accesses\/s).*/\1/p')"
  if [ -z "$rate" ]; then
    echo "  run $i: could not parse simulator output: $line" >&2
    continue
  fi
  printf "  run %d: %8s s  %8s M accesses/s\n" "$i" "$secs" "$rate" >&2
  if awk -v a="$rate" -v b="$best_rate" 'BEGIN{exit !(a>b)}'; then
    best_rate="$rate"
    best_secs="$secs"
  fi
done

echo >&2
echo "THROUGHPUT best=${best_rate} M_accesses_per_second records=${records} seconds=${best_secs}"
