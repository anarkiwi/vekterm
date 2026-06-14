#!/usr/bin/env bash
#
# Build vekterm against libpitrex and install it as a boot service on a PiTrex.
# Run on the Pi (not cross-compiled). After this + a reboot the Pi comes up
# straight into the receiver.
#
#   PITREX_DIR   where your gtoal/pitrex checkout lives (default /home/pi/pitrex)
#   PREFIX       install prefix (default /usr/local)
set -euo pipefail

PITREX_DIR="${PITREX_DIR:-/home/pi/pitrex}"
PREFIX="${PREFIX:-/usr/local}"

# Repo root is the parent of this script's directory.
cd "$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$PITREX_DIR" ]; then
	echo "PiTrex library not found at PITREX_DIR=$PITREX_DIR" >&2
	echo "Clone gtoal/pitrex and build libpitrex first, or set PITREX_DIR." >&2
	exit 1
fi

echo "==> Building vekterm against libpitrex in $PITREX_DIR"
make pitrex PITREX_DIR="$PITREX_DIR"

echo "==> Installing binary to $PREFIX/bin/vekterm"
sudo install -m 0755 vekterm-pitrex "$PREFIX/bin/vekterm"

echo "==> Installing systemd service"
sudo install -m 0644 deploy/vekterm.service /etc/systemd/system/vekterm.service
sudo systemctl daemon-reload
sudo systemctl enable vekterm.service

echo
echo "Installed. If you have not set up the USB serial gadget yet, run:"
echo "    sudo deploy/setup-usb-gadget.sh && sudo reboot"
echo
echo "Otherwise start it now with:"
echo "    sudo systemctl start vekterm"
echo "    journalctl -u vekterm -f      # watch it run"
