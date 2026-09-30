#!/usr/bin/env bash
# Reconstruct a buildable tree = pristine fallout2-ce (cloned at the target ref)
# + our overlay files + our hook patches .
# Usage: scripts/apply.sh [TREE_DIR]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/config.sh"
TREE="${1:-$ROOT/tree}"

echo ">> clone upstream @ ${UPSTREAM_REF}"
rm -rf "$TREE"
git clone -q "$UPSTREAM_URL" "$TREE"
git -C "$TREE" checkout -q --detach "$UPSTREAM_REF"   # real .git so CE's gitver.cmake works

echo ">> overlay (new files)"
cp -a "$ROOT/overlay/." "$TREE/"
# Overlay shell scripts must be LF (a CRLF os/ps4/build.sh breaks bash). .gitattributes
# keeps committed files LF; normalize here too so any stray CRLF can't break the build.
find "$TREE/os" -name '*.sh' -exec sed -i 's/\r$//' {} + 2>/dev/null || true

# Optional custom app icon: if you drop a 512x512 icon0.png at the repo root (it's
# gitignored, so the repo stays clean), feed it to the build — packaging.cmake picks
# up $TREE/icon0.png and uses it instead of the committed placeholder.
if [ -f "$ROOT/icon0.png" ]; then
    cp "$ROOT/icon0.png" "$TREE/icon0.png"
    echo ">> custom app icon: $ROOT/icon0.png"
fi

echo ">> patches (${ROOT}/patches)"
for p in "$ROOT"/patches/*.patch; do
    git -C "$TREE" apply --whitespace=nowarn "$p" || { echo "PATCH FAILED: $p"; exit 1; }
done


# per-build snapshot of exactly what we built against
git -C "$TREE" log -1 --date=iso --pretty="upstream_commit=%H%nupstream_date=%cd" > "$ROOT/build-manifest.txt"
echo "OK: tree ready at $TREE"
