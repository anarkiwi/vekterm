#!/usr/bin/env bash
#
# End-to-end emulation: run the REAL vekterm main loop (handshake, UART receive,
# flush/resync, the real protocol parser and libpitrex draw path) against the
# software 6522 VIA + Vectrex model, and drive it with REAL pyvterm streaming
# over a PTY (its actual SerialTransport with --flow-control). Renders what the
# Vectrex would show to out-emu/e2e.png. See docs/EMULATOR.md.
#
#   tools/emu/e2e.sh [../pyvterm]
#
# Requires: a C compiler, openpty (-lutil), python3 with pyserial, and a pyvterm
# checkout (default ../pyvterm) containing examples/testpattern.py.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
pyvterm="${1:-$root/../pyvterm}"
V="$root/third_party/libpitrex"
out="$root/out-emu"
mkdir -p "$out"

if [ ! -f "$pyvterm/examples/testpattern.py" ]; then
    echo "pyvterm not found at $pyvterm (pass its path as arg 1)"; exit 1
fi
python3 -c "import serial" 2>/dev/null || { echo "pyserial required (pip install pyserial)"; exit 1; }

CC="${CC:-cc}"
CF=(-std=gnu99 -fcommon -O2 -w -DSETTINGS_DIR='"settings"'
    -I"$V" -I"$V/vectrex" -I"$V/pitrex" -I"$V/baremetal" -I"$V/baremetal/lib2835"
    -I"$root/src" -I"$here")

echo "==> compiling end-to-end emulator"
"$CC" "${CF[@]}" -c "$V/vectrex/vectrexInterface.c" -o "$out/vi.o"
"$CC" "${CF[@]}" -c "$here/via_vectrex.c"           -o "$out/via.o"
"$CC" "${CF[@]}" -c "$root/src/protocol.c"          -o "$out/protocol.o"
"$CC" "${CF[@]}" -c "$root/src/frame.c"             -o "$out/frame.o"
"$CC" "${CF[@]}" -c "$root/src/font.c"              -o "$out/font.o"
"$CC" "${CF[@]}" -Dmain=vekterm_main -c "$root/src/vekterm_baremetal.c" -o "$out/vkmain.o"
"$CC" "${CF[@]}" -c "$here/emu_vekterm.c"           -o "$out/emu_vk.o"
"$CC" "$out/vi.o" "$out/via.o" "$out/protocol.o" "$out/frame.o" "$out/font.o" "$out/vkmain.o" "$out/emu_vk.o" \
    -lutil -lpthread -lm -o "$out/emu-vekterm"

echo "==> running (real pyvterm --flow-control -> real vekterm)"
"$out/emu-vekterm" "$out/e2e.ppm" "$pyvterm"
if command -v convert >/dev/null 2>&1; then
    convert "$out/e2e.ppm" "$out/e2e.png" && echo "wrote $out/e2e.png"
fi
