#!/usr/bin/env bash
# Dev helper: regenerate overlay/ + patches/ from the working port (fork) vs the
# pristine upstream ref. Run when the port changes. 
# Usage: PORT_FORK=/path/to/fork scripts/refresh.sh   (default fork below)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/config.sh"
FORK="${PORT_FORK:-/d/GIT/Fallout2PS4}"
REF="$UPSTREAM_REF"

rm -rf "$ROOT/overlay" "$ROOT/patches"
mkdir -p "$ROOT/overlay" "$ROOT/patches"

ADDED=$(git -C "$FORK" diff --name-status "$REF" ps4 | awk '$1=="A"{print $2}')
MODIFIED=$(git -C "$FORK" diff --name-status "$REF" ps4 | awk '$1=="M"{print $2}')

# overlay = added files, EXCLUDING:
#   docs/   — dev/process notes (plan, port log, smoke test). They live in the fork
#             only; the public repo carries just docs/porting-notes.md (hand-written,
#             outside overlay/ so refresh never touches it), and the built tree needs
#             no docs.
#             dev experiment, not part of the engine build, so kept out of the overlay.
ADDED_OK=$(echo "$ADDED" | grep -viE '^docs/' || true)
git -C "$FORK" archive ps4 $ADDED_OK | tar -x -C "$ROOT/overlay"

# patches = one diff per modified upstream file
n=0
for f in $MODIFIED; do
    n=$((n+1))
    name=$(printf "%04d-%s.patch" "$n" "$(echo "$f" | tr '/.' '__')")
    git -C "$FORK" diff "$REF" ps4 -- "$f" > "$ROOT/patches/$name"
done

echo "overlay: $(echo "$ADDED_OK" | wc -w) files | patches: $n"
