#!/usr/bin/env bash
#
# Build the off-target vekterm emulator: vekterm's real draw path (the vendored
# libpitrex vectrexInterface.c + the real protocol/frame parser) linked against
# the software 6522 VIA + Vectrex vector-generator model. No ARM toolchain, no
# QEMU, no Vectrex — it reconstructs what the Vectrex CRT would show.
#
#   tools/emu/build.sh                 # -> out-emu/vekterm-emu, out-emu/gen_frame
#
# See docs/EMULATOR.md.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
V="$root/third_party/libpitrex"
out="$root/out-emu"
mkdir -p "$out"

CC="${CC:-cc}"
CF=(-std=gnu99 -fcommon -O2 -w
    -DSETTINGS_DIR='"settings"'
    -I"$V" -I"$V/vectrex" -I"$V/pitrex" -I"$V/baremetal" -I"$V/baremetal/lib2835"
    -I"$root/src" -I"$here")

echo "==> compiling"
"$CC" "${CF[@]}" -c "$V/vectrex/vectrexInterface.c" -o "$out/vi.o"
"$CC" "${CF[@]}" -c "$here/via_vectrex.c"           -o "$out/via.o"
"$CC" "${CF[@]}" -c "$here/emu_main.c"              -o "$out/main.o"
"$CC" "${CF[@]}" -c "$root/src/protocol.c"          -o "$out/protocol.o"
"$CC" "${CF[@]}" -c "$root/src/frame.c"             -o "$out/frame.o"

echo "==> linking"
"$CC" "$out/vi.o" "$out/via.o" "$out/main.o" "$out/protocol.o" "$out/frame.o" \
    -lm -o "$out/vekterm-emu"
"$CC" "${CF[@]}" "$here/gen_frame.c" "$root/src/protocol.c" -o "$out/gen_frame"

echo "==> built $out/vekterm-emu and $out/gen_frame"
