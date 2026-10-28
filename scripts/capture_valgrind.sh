#!/usr/bin/env bash
# Capture memory traces from the bench/ microbenchmarks using Valgrind lackey,
# and convert them to cachescope's trace format.
#
# Lackey instruments every memory reference, which costs roughly 100x in
# runtime and produces roughly 40 bytes of log per reference. The default
# problem sizes below are therefore much smaller than the ones used for
# wall-clock measurement: the point of a traced run is the access *pattern*,
# not the timing. Sizes are chosen so each trace lands in the low millions of
# records.
#
# Usage:
#   scripts/capture_valgrind.sh [output_dir]
#
# Environment:
#   VALGRIND      path to the valgrind binary (default: valgrind on PATH)
#   VALGRIND_LIB  needed only for a relocated valgrind install
#   KEEP_RAW      set to 1 to keep the multi-GB raw lackey logs
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-$REPO_ROOT/traces/bench}"
RAW_DIR="${TMPDIR:-/tmp}/cachescope-lackey.$$"
VALGRIND="${VALGRIND:-valgrind}"
CONVERT="$REPO_ROOT/scripts/lackey_to_cachescope.py"

if ! command -v "$VALGRIND" >/dev/null 2>&1 && [ ! -x "$VALGRIND" ]; then
  cat >&2 <<EOF
error: valgrind not found (tried '$VALGRIND').

Install it with:        sudo apt-get install valgrind
Or, without root:       apt-get download valgrind
                        dpkg-deb -x valgrind_*.deb \$HOME/.local/valgrind
                        export VALGRIND=\$HOME/.local/valgrind/usr/bin/valgrind
                        export VALGRIND_LIB=\$HOME/.local/valgrind/usr/libexec/valgrind
EOF
  exit 1
fi

if [ ! -x "$REPO_ROOT/bench/stream" ]; then
  echo "building benchmarks..." >&2
  make -C "$REPO_ROOT/bench" || exit 1
fi

mkdir -p "$OUT_DIR" "$RAW_DIR"
echo "valgrind : $("$VALGRIND" --version)" >&2
echo "raw logs : $RAW_DIR" >&2
echo "output   : $OUT_DIR" >&2
echo >&2

# Workloads come from scripts/workloads.tsv so that the traced runs and the
# perf-measured runs in scripts/run_perf.sh can never drift apart.
WORKLOADS=()
while IFS=$'	' read -r name binary args skip; do
  case "$name" in ''|'#'*|name) continue;; esac
  WORKLOADS+=("$name|$binary|$args|$skip")
done < "$REPO_ROOT/scripts/workloads.tsv"

if [ ${#WORKLOADS[@]} -eq 0 ]; then
  echo "error: no workloads parsed from scripts/workloads.tsv" >&2
  exit 1
fi


status=0
for entry in "${WORKLOADS[@]}"; do
  IFS='|' read -r name binary args skip <<< "$entry"
  raw="$RAW_DIR/$name.raw"
  out="$OUT_DIR/$name.trace"

  echo "=== $name: $binary $args ===" >&2
  # shellcheck disable=SC2086
  "$VALGRIND" --tool=lackey --trace-mem=yes --log-file="$raw" \
      "$REPO_ROOT/bench/$binary" $args > "$RAW_DIR/$name.stdout" 2>"$RAW_DIR/$name.stderr"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "  valgrind exited $rc; see $RAW_DIR/$name.stderr" >&2
    status=1
    continue
  fi

  python3 "$CONVERT" "$raw" -o "$out" --third-field instr_gap --skip "$skip" || status=1
  if [ -f "$out" ]; then
    echo "  $(wc -l < "$out") records -> $out" >&2
  fi
  [ "${KEEP_RAW:-0}" = "1" ] || rm -f "$raw"
  echo >&2
done

if [ "${KEEP_RAW:-0}" != "1" ]; then
  rm -rf "$RAW_DIR"
else
  echo "raw logs kept in $RAW_DIR" >&2
fi

echo "done. Simulate with:" >&2
echo "  cachescope run -c configs/baseline.json --third-field instr_gap \\" >&2
echo "      -t $OUT_DIR/stream.trace" >&2
exit $status
