#!/usr/bin/env bash
# Measure the bench/ microbenchmarks on real hardware with `perf stat`, writing
# one CSV row per workload for scripts/compare_perf.py to join against the
# simulator's counters.
#
# The workload list comes from scripts/workloads.tsv, the same file
# scripts/capture_valgrind.sh uses, so the measured runs and the traced runs are
# the same problem sizes.
#
# Events the host PMU cannot provide are recorded as empty cells, never zeros.
# A zero would silently become a 0% miss rate in the comparison table; an empty
# cell shows up as "not available", which is the truth. Two distinct failures
# are both reported this way:
#   <not supported>  the PMU has no such event (LLC-load-misses under a
#                    hypervisor, for example)
#   <not counted>    the event exists but could not be scheduled
#
# Usage:
#   scripts/run_perf.sh [output_csv]
#
# Environment:
#   PERF     path to the perf binary (default: perf on PATH)
#   REPEATS  perf stat -r value (default 5)
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_CSV="${1:-$REPO_ROOT/analysis/perf_measured.csv}"
PERF="${PERF:-perf}"
REPEATS="${REPEATS:-5}"

if ! command -v "$PERF" >/dev/null 2>&1 && [ ! -x "$PERF" ]; then
  cat >&2 <<'EOF'
error: perf not found.

Install it with:   sudo apt-get install linux-tools-generic
Then:              export PERF=$(command -v perf)

Before trusting any numbers, check that this machine exposes hardware counters:

   cc -O2 -o /tmp/check_perf scripts/check_perf_support.c && /tmp/check_perf

Under a hypervisor that does not virtualise the PMU, perf installs fine but
every hardware event reports <not supported>.
EOF
  exit 1
fi

if [ ! -x "$REPO_ROOT/bench/stream" ]; then
  echo "building benchmarks..." >&2
  make -C "$REPO_ROOT/bench" || exit 1
fi

# Events are collected in groups sized to the host's general-purpose counters.
# A Skylake-class core has four; asking for nine at once makes the kernel
# multiplex them and perf then reports <not counted> for whatever it could not
# schedule. Separate exact groups beat one multiplexed group, because the counts
# are measured over the whole run rather than extrapolated from a fraction of it.
EVENT_GROUPS=(
  "cycles,instructions,branch-instructions,branch-misses"
  "L1-dcache-loads,L1-dcache-load-misses,cache-references,cache-misses"
  "LLC-load-misses,LLC-loads"
)

mkdir -p "$(dirname "$OUT_CSV")"
echo "perf     : $("$PERF" --version 2>&1 | head -1)" >&2
echo "repeats  : $REPEATS" >&2
for g in "${EVENT_GROUPS[@]}"; do
  echo "events   : $g" >&2
done
echo "output   : $OUT_CSV" >&2
echo >&2

echo "workload,binary,args,wall_seconds,cycles,instructions,branch_instructions,branch_misses,l1d_loads,l1d_load_misses,cache_references,cache_misses,llc_load_misses,llc_loads" > "$OUT_CSV"

# Reads one counter out of the merged perf CSV. Event names are matched after
# stripping perf's privilege-level suffix: ":u" is appended whenever
# perf_event_paranoid forbids counting kernel time, which is the common case.
get_event() {
  awk -F, -v want="$1" '
    {
      ev = $3
      sub(/:[ukhHGSD]+$/, "", ev)
      if (ev == want) {
        if ($1 ~ /not supported|not counted/) { print "" } else { print $1 }
        exit
      }
    }' "$2"
}

status=0
while IFS=$'\t' read -r name binary args skip; do
  case "$name" in ''|\#*|name) continue ;; esac
  echo "=== $name: $binary $args ===" >&2

  merged="$(mktemp)"
  prog_out="$(mktemp)"

  for group in "${EVENT_GROUPS[@]}"; do
    raw="$(mktemp)"
    # -x, writes machine-readable CSV to stderr: value,unit,event,...
    # shellcheck disable=SC2086
    "$PERF" stat -x, -r "$REPEATS" -e "$group" \
        "$REPO_ROOT/bench/$binary" $args > "$prog_out" 2> "$raw"
    rc=$?
    if [ "$rc" -ne 0 ]; then
      echo "  perf exited $rc for group [$group]:" >&2
      sed 's/^/    /' "$raw" | head -6 >&2
      status=1
    fi
    cat "$raw" >> "$merged"
    rm -f "$raw"
  done

  # The benchmark's own RESULT line times just the kernel under test, which is
  # tighter than perf's whole-process elapsed time.
  wall="$(sed -n 's/^RESULT.*seconds=\([0-9.][0-9.]*\).*$/\1/p' "$prog_out" | head -1)"

  cycles="$(get_event cycles "$merged")"
  instructions="$(get_event instructions "$merged")"
  br_inst="$(get_event branch-instructions "$merged")"
  br_miss="$(get_event branch-misses "$merged")"
  l1_loads="$(get_event L1-dcache-loads "$merged")"
  l1_miss="$(get_event L1-dcache-load-misses "$merged")"
  cache_ref="$(get_event cache-references "$merged")"
  cache_miss="$(get_event cache-misses "$merged")"
  llc_miss="$(get_event LLC-load-misses "$merged")"
  llc_loads="$(get_event LLC-loads "$merged")"

  echo "$name,$binary,\"$args\",$wall,$cycles,$instructions,$br_inst,$br_miss,$l1_loads,$l1_miss,$cache_ref,$cache_miss,$llc_miss,$llc_loads" >> "$OUT_CSV"

  if [ -n "$cycles" ] && [ -n "$instructions" ]; then
    echo "  IPC                    $(awk -v i="$instructions" -v c="$cycles" 'BEGIN{printf "%.4f", (c>0)?i/c:0}')" >&2
  fi
  if [ -n "$l1_loads" ] && [ -n "$l1_miss" ]; then
    echo "  L1D load miss rate     $(awk -v m="$l1_miss" -v l="$l1_loads" 'BEGIN{printf "%.2f%%", (l>0)?100*m/l:0}')" >&2
  fi
  if [ -n "$br_inst" ] && [ -n "$br_miss" ]; then
    echo "  branch mispredict rate $(awk -v m="$br_miss" -v b="$br_inst" 'BEGIN{printf "%.3f%%", (b>0)?100*m/b:0}')" >&2
  fi
  for pair in "cache-references:$cache_ref" "cache-misses:$cache_miss" "LLC-load-misses:$llc_miss"; do
    ev="${pair%%:*}"
    val="${pair#*:}"
    if [ -z "$val" ]; then
      echo "  $ev: not available on this host" >&2
    fi
  done

  rm -f "$merged" "$prog_out"
done < "$REPO_ROOT/scripts/workloads.tsv"

echo >&2
echo "wrote $OUT_CSV" >&2
exit "$status"
