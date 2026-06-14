# Deploying vekterm: a PiTrex that boots straight into the receiver

The goal: power on the PiTrex and have it come up **directly into vekterm**,
listening on a serial port, ready for a sender (pyvterm or MAME) to draw — no
login, no manual start. This is done with a USB-CDC serial gadget plus a systemd
service enabled at boot.

```
power on ──▶ kernel loads dwc2 + g_serial ──▶ /dev/ttyGS0 appears
         ──▶ systemd starts vekterm.service ──▶ vekterm reads /dev/ttyGS0
                                                 and drives the Vectrex
```

## Prerequisites

- A built PiTrex: a Raspberry Pi Zero (W/WH) seated on the PiTrex cartridge in a
  Vectrex, per the [PiTrex Hardware Guide][hwguide].
- Raspberry Pi OS (Lite is fine) on the SD card, with shell access (SSH or
  keyboard).
- A [`gtoal/pitrex`](https://github.com/gtoal/pitrex) checkout on the Pi with
  **`libpitrex` built** (its raspbian build). vekterm links against it.
- This repo checked out on the Pi.

> **Power.** When you drive the Pi from the host PC over the data USB port,
> remove the PiTrex **POWER FROM VEC.** jumper first, as the guide describes —
> the PC then supplies power. ([Hardware Guide][hwguide].)

## Step 1 — Set up the serial link (USB gadget, recommended)

The Pi Zero's *data* micro-USB port (the inner one marked **USB**, not **PWR**)
can act as a USB-CDC serial gadget. It needs no serial adapter and meets the
2 Mbaud nominal rate with room to spare, because USB-CDC ignores the line rate.

```bash
sudo deploy/setup-usb-gadget.sh
```

That script (idempotent) does three things in the boot partition
(`/boot/firmware` or `/boot`):

1. adds `dtoverlay=dwc2,dr_mode=peripheral` to `config.txt` — enable the USB
   controller in gadget mode;
2. appends `modules-load=dwc2,g_serial` to `cmdline.txt` — load the serial
   gadget at boot, creating **`/dev/ttyGS0`** on the Pi (the host PC sees
   **`/dev/ttyACM0`**);
3. masks `serial-getty@ttyGS0.service` so a login console doesn't grab the port.

Prefer the GPIO UART instead? See [UART alternative](#uart-alternative) below.

## Step 2 — Build and install vekterm

```bash
deploy/install.sh                      # PITREX_DIR defaults to /home/pi/pitrex
# or, if your pitrex checkout is elsewhere:
PITREX_DIR=/path/to/pitrex deploy/install.sh
```

`install.sh`:

1. `make pitrex PITREX_DIR=…` — builds `vekterm-pitrex` linked to `libpitrex`;
2. installs it to `/usr/local/bin/vekterm`;
3. installs [`deploy/vekterm.service`](../deploy/vekterm.service) and
   `systemctl enable`s it (so it starts on every boot).

The service runs `vekterm --port /dev/ttyGS0 --baud 2000000`, restarts on
failure (covering the brief window before the gadget device exists), and runs
with `Nice=-10` so the beam-drawing loop stays steady.

## Step 3 — Reboot

```bash
sudo reboot
```

On boot the gadget device appears and `vekterm.service` starts automatically.
That's the "boots straight into the server" state.

## Verify

On the **Pi**:

```bash
systemctl status vekterm        # should be active (running)
journalctl -u vekterm -f        # live log: "backend=pitrex source=/dev/ttyGS0", frame counts
ls -l /dev/ttyGS0               # the gadget device exists
```

On the **host PC** (with the data cable to the Pi's USB port):

```bash
ls /dev/ttyACM0                 # Linux; macOS: /dev/tty.usbmodem*
pip install pyvterm
python -c "
from pyvterm import VectorTerminal
with VectorTerminal(port='/dev/ttyACM0') as vt:
    with vt.frame():
        vt.set_intensity(15)
        vt.polyline([(-200,-200),(200,-200),(200,200),(-200,200)], closed=True)
"
```

A square should appear on the Vectrex.

## Calibration

Coordinate and intensity mapping depend on your Vectrex; tune them, then edit
the `ExecStart=` line in `/etc/systemd/system/vekterm.service` to bake in the
values (`sudo systemctl daemon-reload && sudo systemctl restart vekterm`).

| Flag | Effect | Default |
| --- | --- | --- |
| `--min N` / `--max N` | Vectrex coords that device `0` / `4095` map to | `-2048` / `2047` |
| `--scale N` | `v_setScale` integrator scale register | library default |
| `--bright-shift N` | beam intensity = `brightness >> N` (0..255 → z-axis) | `1` |

Drive a known frame while you tweak: `vekterm --dry-run --input frame.bin -v`
shows exactly what was decoded before you commit to hardware values.

## UART alternative

If you wire a **3.3 V** USB-to-TTL adapter to the Pi header (pins 1=3V3, 6=GND,
8=Tx/GPIO14, 10=Rx/GPIO15, crossing Tx↔Rx) instead of using the USB gadget:

1. Skip `setup-usb-gadget.sh`. Instead enable the UART and free it from the
   login console: set `enable_uart=1` in `config.txt`, remove any
   `console=serial0,115200` from `cmdline.txt`, and
   `sudo systemctl disable --now serial-getty@ttyAMA0.service`.
2. Point the service at it: change `ExecStart=` to
   `--port /dev/serial0`, then `daemon-reload` + `restart`.

The adapter must be **3.3 V (never 5 V)** and sustain **≥ 2 Mbaud** — a genuine
FTDI FT232R or CP2102N. Avoid CH340 / original CP2102 modules. (The USB gadget
sidesteps the line-rate question entirely, which is why it's recommended.)

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `open /dev/ttyGS0: No such file or directory` | Gadget not up yet. Confirm `dtoverlay=dwc2` + `modules-load=dwc2,g_serial`, reboot. `Restart=always` retries meanwhile. |
| Service flaps / `Restart` loops | Same as above, or the port is held by a getty — `systemctl mask serial-getty@ttyGS0.service`. |
| Host never sees `/dev/ttyACM0` | Using the **PWR** port, a power-only cable, or `dr_mode` not peripheral. Use the inner **USB** port and a data cable. |
| Permission denied on the VIA / GPIO | The service runs as `root` for hardware access; if you changed `User=`, restore it or grant GPIO access. |
| Connects but nothing draws | Calibration: widen `--min/--max`, set `--scale`, lower `--bright-shift`. Check `journalctl -u vekterm` shows non-zero frame/vector counts. |
| Want to test without a Vectrex | `vekterm --dry-run` (or any non-pitrex build) decodes and reports without touching hardware. |

## Updating / uninstalling

```bash
git pull && deploy/install.sh && sudo systemctl restart vekterm   # update

sudo systemctl disable --now vekterm                              # uninstall
sudo rm /etc/systemd/system/vekterm.service /usr/local/bin/vekterm
sudo systemctl daemon-reload
```

[hwguide]: http://www.ombertech.com/cnk/pitrex/wiki/index.php?wiki=Developer_Release_HW_Guide
