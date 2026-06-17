#!/usr/bin/env bash
#
# Assemble a flashable Raspberry Pi SD-card image that boots straight into
# vekterm (baremetal — no operating system).
#
# Fully unprivileged: it partitions a plain file with sfdisk and populates a FAT
# partition with mtools, so there are no loop mounts and no root required. That
# makes it safe to run inside Docker or CI.
#
#   deploy/build-image.sh kernel.img vekterm.img
#
# Env overrides: RPI_FW_REF (firmware branch/tag, default "stable"),
# IMG_SIZE_MB (default 64).
set -euo pipefail

KERNEL="${1:?usage: build-image.sh <kernel.img> <kernel7.img> <out.img>}"
KERNEL7="${2:?usage: build-image.sh <kernel.img> <kernel7.img> <out.img>}"
OUT="${3:?usage: build-image.sh <kernel.img> <kernel7.img> <out.img>}"

# bootcode.bin + start.elf + fixup.dat from raspberrypi/firmware. This pre-Pi4
# firmware set boots every model we target: the BCM2835 Pi Zero/W (loads
# kernel.img) and the BCM2837 Pi Zero 2 W (loads kernel7.img).
RPI_FW_REF="${RPI_FW_REF:-stable}"
FW_BASE="https://github.com/raspberrypi/firmware/raw/${RPI_FW_REF}/boot"
FW_FILES="bootcode.bin start.elf fixup.dat"

IMG_SIZE_MB="${IMG_SIZE_MB:-64}"
PART_OFFSET_SECTORS=2048 # 1 MiB
PART_OFFSET=$((PART_OFFSET_SECTORS * 512))

here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "==> Fetching Raspberry Pi firmware (${RPI_FW_REF})"
for f in $FW_FILES; do
	curl -fsSL "$FW_BASE/$f" -o "$work/$f"
done

echo "==> Staging boot files"
cp "$KERNEL"  "$work/kernel.img"
cp "$KERNEL7" "$work/kernel7.img"
cp "$here/config.txt" "$work/config.txt"

echo "==> Creating ${IMG_SIZE_MB}MiB image with a bootable FAT partition"
rm -f "$OUT"
truncate -s "${IMG_SIZE_MB}M" "$OUT"
sfdisk "$OUT" >/dev/null <<EOF
label: dos
start=${PART_OFFSET_SECTORS}, type=c, bootable
EOF

echo "==> Formatting + populating the partition (mtools, unprivileged)"
mformat -i "$OUT@@${PART_OFFSET}" -F -v VEKTERM ::
mcopy -i "$OUT@@${PART_OFFSET}" \
	"$work/bootcode.bin" "$work/start.elf" "$work/fixup.dat" \
	"$work/config.txt" "$work/kernel.img" "$work/kernel7.img" ::/
# vectrexInterface mounts the SD on v_init(); give it the dirs it may look for.
mmd -i "$OUT@@${PART_OFFSET}" ::/settings ::/ini 2>/dev/null || true

echo "==> Done: $OUT"
mdir -i "$OUT@@${PART_OFFSET}" ::/
