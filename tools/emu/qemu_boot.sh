#!/usr/bin/env bash
#
# Boot a vekterm kernel image in QEMU and assert it reaches the boot banner —
# i.e. the ARM bring-up (entry asm, VFP, MMU, clock, mini-UART) actually runs.
# This is the "it boots in an emulator" proof for the ARM side, complementing the
# draw-path emulator in this directory (see EMULATOR.md / docs/BOOT.md).
#
#   tools/emu/qemu_boot.sh [kernel.img] [machine]
#       kernel.img : default out/kernel.img
#       machine    : default raspi0 (single core — clean serial; use raspi2b for
#                    the ARMv7 kernel7.img, which also boots but whose un-parked
#                    secondary cores add abort noise)
#
# Notes / why the flags:
#  - The Pi firmware loads kernel*.img at 0x8000, but QEMU's `-kernel` loads
#    AArch32 images at 0x10000. So we place it with `-device loader,addr=0x8000`
#    and point CPU 0 there — matching the real load address.
#  - vekterm logs over the mini-UART (UART1/AUX), which is QEMU's *second* serial,
#    hence `-serial null -serial mon:stdio`.
#  - QEMU models the cartridge-bus VIA as nothing, so after the banner the kernel
#    blocks in vectrexinit() waiting on the (never-toggling) RDY line. Reaching
#    the banner is the success condition; we stop a few seconds in.
set -uo pipefail

KIMG="${1:-out/kernel.img}"
MACHINE="${2:-raspi0}"
SECS="${SECS:-5}"

if ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "SKIP: qemu-system-arm not installed"; exit 0
fi
[ -f "$KIMG" ] || { echo "FAIL: $KIMG not found (build it: make docker)"; exit 1; }

echo "==> booting $KIMG on $MACHINE (mini-UART, ${SECS}s)"
log="$(timeout "$SECS" qemu-system-arm -M "$MACHINE" -display none \
        -device loader,file="$KIMG",addr=0x8000,cpu-num=0 \
        -serial null -serial mon:stdio 2>/dev/null || true)"

echo "----- captured UART -----"
printf '%s\n' "$log" | grep -aE 'PiTrex|CLOCK|bcm2835_init' | head -8
echo "-------------------------"

# Match just "PiTrex": on multi-core machines (raspi2b) the un-parked secondary
# cores interleave prefetch-abort messages into the serial stream, which can
# split the full "PiTrex starting" string even though the kernel did banner.
if printf '%s' "$log" | grep -qa 'PiTrex'; then
    echo "PASS: kernel booted to the banner in QEMU"
    exit 0
fi
echo "FAIL: no boot banner — kernel did not reach kernelMain"
exit 1
