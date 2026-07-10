#!/bin/bash
# Builds libjbc (sandbox-escape lib) and installs it into the OpenOrbis prefix
# as libjbc.a + headers under <prefix>/usr/include/jbc/.
#
# The upstream libjbc Makefile depends on a build_rules.mk that PacBrew's
# OpenOrbis prefix does not ship, so we compile the handful of C files directly.
#
# Run once after installing the toolchain, before building the engine.
# Requires: OPENORBIS env (source $OPENORBIS/ps4vars.sh first).
set -e

: "${OPENORBIS:?source \$OPENORBIS/ps4vars.sh first}"

SRC_URL="https://github.com/cy33hc/ps4-libjbc.git"
WORK="${1:-/tmp/ps4-libjbc}"

if [ ! -d "$WORK" ]; then
    git clone --depth 1 "$SRC_URL" "$WORK"
fi
cd "$WORK"

CFLAGS="--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c \
  -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4 -D__BSD_VISIBLE -D_BSD_SOURCE \
  -isysroot $OPENORBIS -isystem $OPENORBIS/include -I$OPENORBIS/usr/include -I."

rm -f *.o libjbc.a
for src in jailbreak.c kernelrw.c utils.c; do
    clang $CFLAGS -o "${src%.c}.o" "$src"
done
llvm-ar rcs libjbc.a jailbreak.o kernelrw.o utils.o

cp libjbc.a "$OPENORBIS/usr/lib/libjbc.a"
mkdir -p "$OPENORBIS/usr/include/jbc"
cp libjbc.h jailbreak.h kernelrw.h utils.h defs.h "$OPENORBIS/usr/include/jbc/"
echo "installed libjbc -> $OPENORBIS/usr/lib/libjbc.a and usr/include/jbc/"
