#!/usr/bin/env bash
# Measure this machine's memory latency as a function of footprint, by timing a
# dependent-load pointer chase over a single cycle of a random permutation.
#
# Because each load's address depends on the previous load's result, there is no
# memory-level parallelism to hide latency and no spatial locality for a
# prefetcher to exploit. The resulting curve therefore steps up as the footprint
# crosses each cache capacity, which makes it a direct measurement of the
# hierarchy the simulator is supposed to model.
#
# The output feeds two things: the calibrated config's latency figures, and the
# plot produced by scripts/analyze.py.
#
# Usage: scripts/measure_memory_latency.sh [output_csv] [repeats]
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_CSV="${1:-$REPO_ROOT/analysis/memory_latency_curve.csv}"
REPEATS="${2:-3}"
STEPS="${STEPS:-2000000}"

if [ ! -x "$REPO_ROOT/bench/pointer_chase" ]; then
  make -C "$REPO_ROOT/bench" pointer_chase || exit 1
fi

mkdir -p "$(dirname "$OUT_CSV")"
echo "footprint_kib,ns_per_dependent_load" > "$OUT_CSV"

echo "measuring dependent-load latency ($REPEATS repeats, $STEPS steps each)" >&2
for kib in 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536 131072 262144; do
  best=""
  for _ in $(seq 1 "$REPEATS"); do
    ns="$("$REPO_ROOT/bench/pointer_chase" "$kib" "$STEPS" |
          sed -n 's/.*ns_per_op=\([0-9.][0-9.]*\).*/\1/p')"
    [ -z "$ns" ] && continue
    # Keep the minimum: it is the run least disturbed by scheduling and
    # frequency scaling, which is what you want for a latency figure.
    if [ -z "$best" ] || awk -v a="$ns" -v b="$best" 'BEGIN{exit !(a<b)}'; then
      best="$ns"
    fi
  done
  printf "%8s KiB  %10s ns\n" "$kib" "$best" >&2
  echo "$kib,$best" >> "$OUT_CSV"
done

echo >&2
echo "wrote $OUT_CSV" >&2
