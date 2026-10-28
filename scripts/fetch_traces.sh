#!/usr/bin/env bash
# Fetch the upstream memory traces this project is compatible with.
#
# gcc.trace and swim.trace come from kev-kim/cache-simulator, which carries no
# LICENSE file. They are therefore downloaded on demand rather than vendored
# into this repository: redistributing someone else's unlicensed files is not
# ours to do, and a fetch script costs the user one command.
#
# Credit: the simulator is by Kevin Kim; its README credits the trace files to
# Professor David Hovemeyer (Johns Hopkins University).
#
# Usage:
#   scripts/fetch_traces.sh [dest_dir]
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$REPO_ROOT/traces/upstream}"
UPSTREAM_REPO="https://github.com/kev-kim/cache-simulator.git"
WORK="${TMPDIR:-/tmp}/cachescope-upstream-traces.$$"

TRACES=(gcc swim read01 read02 read03 write01 write02)

mkdir -p "$DEST"

missing=0
for t in "${TRACES[@]}"; do
  [ -f "$DEST/$t.trace" ] || missing=1
done
if [ "$missing" -eq 0 ]; then
  echo "all traces already present in $DEST" >&2
  ls -la "$DEST"/*.trace
  exit 0
fi

if ! command -v git >/dev/null 2>&1; then
  echo "error: git is required to fetch the upstream traces" >&2
  exit 1
fi

echo "cloning $UPSTREAM_REPO (shallow)..." >&2
if ! git clone --depth 1 --quiet "$UPSTREAM_REPO" "$WORK"; then
  echo "error: clone failed. Check network access, or copy the files from" >&2
  echo "       $UPSTREAM_REPO tests/ into $DEST by hand." >&2
  rm -rf "$WORK"
  exit 1
fi

for t in "${TRACES[@]}"; do
  src="$WORK/tests/$t.trace"
  if [ -f "$src" ]; then
    cp "$src" "$DEST/$t.trace"
    printf "  %-12s %10s lines\n" "$t.trace" "$(wc -l < "$DEST/$t.trace")" >&2
  else
    echo "  warning: $t.trace not found upstream" >&2
  fi
done

# Record where the files came from, next to the files themselves.
cat > "$DEST/PROVENANCE.md" <<EOF
# Upstream trace provenance

These files were downloaded by \`scripts/fetch_traces.sh\` from:

    $UPSTREAM_REPO

at commit $(git -C "$WORK" rev-parse HEAD 2>/dev/null || echo unknown).

The upstream repository is a cache simulator by Kevin Kim. Its README credits
the trace files to Professor David Hovemeyer. The repository carries **no
LICENSE file**, so these traces are fetched on demand rather than committed to
this repository.

Format: \`<l|s> <hex address> <integer>\` per line. The upstream simulator
ignores the third field entirely (see \`csim_fns.cpp\`). In gcc.trace that field
is 0 on 42% of lines with a mean of 1.99 and a maximum of 74, which rules out a
byte count and fits "non-memory instructions since the previous access".
cachescope can read it either way: \`--third-field size\` (default) or
\`--third-field instr_gap\`.
EOF

rm -rf "$WORK"
echo >&2
echo "traces in $DEST:" >&2
ls -la "$DEST"
