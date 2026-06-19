#!/usr/bin/env bash
#
# Smoke test for the off-target emulator: build it, render the idle splash and a
# known square through vekterm's real draw path + the VIA model, and assert the
# reconstructed geometry. This is the regression guard that "vekterm boots to a
# splash and draws frames" — provable with no hardware. See docs/EMULATOR.md.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="$root/out-emu"

"$here/build.sh"

echo "==> splash: render + count strokes"
"$out/vekterm-emu" --splash --out "$out/splash.ppm" 2>"$out/splash.log"
segs=$(sed -n 's/.*segments=\([0-9]*\).*/\1/p' "$out/splash.log")
echo "    splash segments=$segs"
[ "${segs:-0}" -gt 100 ] || { echo "FAIL: splash drew too few segments ($segs)"; exit 1; }

echo "==> square: drive a known frame through the real protocol parser + VIA"
"$out/gen_frame" square > "$out/square.bin"
# libpitrex prints boot chatter on stdout too; keep only the 5-field segment rows
"$out/vekterm-emu" --frame "$out/square.bin" --out "$out/square.ppm" --dump 2>"$out/square.log" \
    | grep -E '^-?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+ [0-9]+$' | sort -u > "$out/square.segs"
n=$(grep -c . "$out/square.segs")
echo "    square segments=$n"
[ "$n" -eq 4 ] || { echo "FAIL: square should be 4 segments, got $n"; cat "$out/square.segs"; exit 1; }
# corners must sit near +-5000 (device 1024/3072 mapped onto +-10000), <1% off
awk '{ for(i=1;i<=4;i++){v=$i<0?-$i:$i; if(v<4900||v>5100){print "FAIL: corner off:",$0; exit 1}} }' \
    "$out/square.segs"

echo "==> rendering PNGs (if ImageMagick present)"
if command -v convert >/dev/null 2>&1; then
    convert "$out/splash.ppm" "$out/splash.png"
    convert "$out/square.ppm" "$out/square.png"
    echo "    wrote $out/splash.png and $out/square.png"
fi

echo "PASS: splash + square reconstructed correctly"
