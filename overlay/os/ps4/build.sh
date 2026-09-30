#!/bin/bash
# One-shot PS4 engine build: configure with the OpenOrbis toolchain, compile,
# and produce the fpkg. Run from the repo root.
#
#   source $OPENORBIS/ps4vars.sh
#   os/ps4/build.sh            # configure + build + package
#   os/ps4/build.sh clean      # wipe the build dir first
#
# Extra CMake flags can be passed via PS4_EXTRA_CMAKE if needed.
#
# Output: build-ps4/<CONTENT_ID>.pkg
set -e

: "${OPENORBIS:?source \$OPENORBIS/ps4vars.sh first}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/build-ps4"

if [ "$1" = "clean" ]; then
    rm -rf "$BUILD"
fi

# libjbc must be installed into the prefix before configuring.
if [ ! -f "$OPENORBIS/usr/lib/libjbc.a" ]; then
    echo "libjbc not found — running os/ps4/build_libjbc.sh"
    "$ROOT/os/ps4/build_libjbc.sh"
fi

# ps4vars.sh sets LDFLAGS with crt1.o which conflicts with the toolchain file;
# the toolchain clears it, but unset here too for a clean configure.
unset LDFLAGS

"$OPENORBIS/usr/bin/openorbis-cmake" -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFALLOUT_VENDORED=OFF \
    ${PS4_EXTRA_CMAKE:-}

cmake --build "$BUILD" -j"$(nproc)"

echo
echo "=== PS4 build artifacts ==="
find "$BUILD" -maxdepth 1 -name '*.pkg' -o -maxdepth 1 -name 'eboot.bin' | sort
