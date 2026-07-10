#!/usr/bin/env bash
# On-demand build: reconstruct the tree and build the PS4 pkg.
# Prereq: source $OPENORBIS/ps4vars.sh first (OpenOrbis toolchain).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${OPENORBIS:?source \$OPENORBIS/ps4vars.sh first}"

bash "$ROOT/scripts/apply.sh" "$ROOT/tree"

cd "$ROOT/tree"
unset LDFLAGS
bash os/ps4/build.sh clean

pkg="$(ls build-ps4/*.pkg | head -1)"
cp "$pkg" "$ROOT/"
echo
echo "=== DONE ==="
cat "$ROOT/build-manifest.txt"
echo "pkg -> $ROOT/$(basename "$pkg")"
