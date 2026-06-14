#!/usr/bin/env bash
#
# Configure a Raspberry Pi Zero (the PiTrex host) as a USB CDC-ACM serial
# gadget. After a reboot the Pi's *data* micro-USB port presents:
#
#     device side : /dev/ttyGS0   <- vekterm reads this
#     host side   : /dev/ttyACM0  <- pyvterm / MAME writes this
#
# USB-CDC ignores the line rate, so no serial adapter and no exact baud are
# needed. Run with sudo. Reboot when it finishes.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Please run as root (sudo $0)" >&2
	exit 1
fi

# Raspberry Pi OS moved the boot partition to /boot/firmware; fall back to /boot.
BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
CONFIG="$BOOT/config.txt"
CMDLINE="$BOOT/cmdline.txt"

echo "Editing $CONFIG and $CMDLINE"

# 1. Enable the dwc2 USB controller in peripheral (gadget) mode.
if ! grep -q '^dtoverlay=dwc2' "$CONFIG"; then
	echo 'dtoverlay=dwc2,dr_mode=peripheral' >>"$CONFIG"
	echo "  + added dtoverlay=dwc2 to config.txt"
else
	echo "  = dwc2 overlay already present"
fi

# 2. Load the dwc2 + g_serial modules early, creating /dev/ttyGS0 at boot.
#    cmdline.txt must remain a single line.
if ! grep -q 'modules-load=dwc2,g_serial' "$CMDLINE"; then
	content="$(tr -d '\n' <"$CMDLINE")"
	printf '%s modules-load=dwc2,g_serial\n' "$content" >"$CMDLINE"
	echo "  + appended modules-load=dwc2,g_serial to cmdline.txt"
else
	echo "  = g_serial already in cmdline.txt"
fi

# 3. Stop a login console from grabbing the gadget port out from under vekterm.
systemctl mask serial-getty@ttyGS0.service 2>/dev/null || true
echo "  = masked serial-getty@ttyGS0 (so vekterm owns /dev/ttyGS0)"

echo
echo "Done. Reboot for /dev/ttyGS0 to appear:  sudo reboot"
