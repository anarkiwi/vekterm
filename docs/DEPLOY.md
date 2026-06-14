# Deploying vekterm: a PiTrex that boots straight into the receiver

vekterm deploys **baremetal** — there is no operating system. The Pi's firmware
loads `kernel.img` (which *is* vekterm) and runs it directly. Power on and the
PiTrex is immediately drawing whatever a sender streams over the serial link.

```
power on ──▶ Pi firmware (bootcode.bin, start.elf) loads kernel.img
         ──▶ vekterm runs: reads the mini-UART, draws the active frame on the
             Vectrex every refresh via libpitrex
```

## Prerequisites

- A built PiTrex: a Raspberry Pi Zero (W/WH) on the PiTrex cartridge in a
  Vectrex, per the [PiTrex Hardware Guide][hwguide].
- A microSD card.
- **Docker** (to build the image — no local toolchain needed), or
  `arm-none-eabi-gcc` + newlib for a local build.
- On the sender side: a host running pyvterm and a **3.3 V** USB-to-TTL serial
  adapter (for the UART link, below).

## Step 1 — Build the image

```bash
make docker
# -> out/kernel.img   the baremetal binary
# -> out/vekterm.img  a flashable SD-card image (firmware + config.txt + kernel.img)
```

`make docker` cross-compiles vekterm against `libpitrex` and assembles the SD
image entirely in a container. To calibrate up front (you can also do it later),
pass `--build-arg VEKTERM_CFLAGS='…'` — see [Calibration](#calibration).

## Step 2 — Flash the card

```bash
# Linux/macOS (find your card's device first; this erases it!):
sudo dd if=out/vekterm.img of=/dev/sdX bs=4M conv=fsync
```

Or use [Raspberry Pi Imager](https://www.raspberrypi.com/software/) → "Use
custom" → `out/vekterm.img`. The image is a single bootable FAT partition; the
extra space is unused.

## Step 3 — Wire the serial link (UART)

Baremetal reads the Pi's **mini-UART** (GPIO14/15). Wire the host's 3.3 V
USB-TTL adapter to the Pi header, crossing Tx↔Rx:

| Host adapter | Pi Zero header |
| --- | --- |
| TX  | pin 10 = GPIO15 (RXD) |
| RX  | pin 8  = GPIO14 (TXD) — optional, for boot diagnostics |
| GND | pin 6  = GND |

The adapter must be **3.3 V (never 5 V)** and keep up with the line rate. The
default is **2 Mbaud** (matching pyvterm's default); a genuine FTDI FT232R or
CP2102N handles it. If yours is flaky, rebuild for a slower, rock-solid link with
`VEKTERM_CFLAGS='-DVT_UART_BAUD=115200'` and set pyvterm's `baudrate=115200`.

## Step 4 — Boot

Insert the card, power on. The Pi boots straight into vekterm. On the host:

```bash
pip install pyvterm
python -c "
from pyvterm import VectorTerminal
with VectorTerminal(port='/dev/ttyUSB0', baudrate=2000000) as vt:  # your adapter's port
    with vt.frame():
        vt.set_intensity(15)
        vt.polyline([(-200,-200),(200,-200),(200,200),(-200,200)], closed=True)
"
```

A square should appear on the Vectrex.

## Calibration

Coordinate and intensity mapping depend on your Vectrex. These are compile-time
options; set them via `VEKTERM_CFLAGS` (Docker) or `EXTRA_CFLAGS` (local
`make baremetal`), then re-flash.

| Define | Meaning | Default |
| --- | --- | --- |
| `VT_VECTREX_MIN` / `VT_VECTREX_MAX` | Vectrex coords that device `0` / `4095` map to | `-10000` / `10000` |
| `VT_BRIGHT_SHIFT` | beam intensity = brightness (0..255) `>> N` | `1` |
| `VT_UART_BAUD` | serial line rate (must match the sender) | `2000000` |
| `VT_REFRESH_HZ` | redraw rate handed to libpitrex | `50` |

```bash
docker build --target artifacts --output type=local,dest=out \
  --build-arg VEKTERM_CFLAGS='-DVT_VECTREX_MAX=12000 -DVT_BRIGHT_SHIFT=2' .
```

Tip: `./vekterm --dry-run --input frame.bin -v` on your PC shows exactly which
vectors a captured stream decodes to before you commit to hardware values.

## What's on the card / how it boots

The FAT partition holds the stock Raspberry Pi firmware plus our kernel:

| File | Purpose |
| --- | --- |
| `bootcode.bin`, `start.elf`, `fixup.dat` | Raspberry Pi firmware (fetched from raspberrypi/firmware) |
| [`config.txt`](../deploy/config.txt) | GPIO setup for the VIA, pins the core clock for a stable UART |
| `kernel.img` | vekterm itself |

There is deliberately **no** Linux kernel, initramfs, or root filesystem.

## Building locally (without Docker)

libpitrex is vendored in [`third_party/libpitrex`](../third_party/libpitrex), so
no checkout is needed:

```bash
sudo apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi mtools fdisk
make image          # -> kernel.img and vekterm.img
```

The vendored sources carry small, baked-in fixes so they compile with a current
`arm-none-eabi-gcc`/newlib (a `MAP_FAILED` fallback and missing
`<stdint.h>`/`u_long` in `vectors.h`); the toolchain-compatibility flags
(`-fcommon`, `--specs=nosys.specs`) live in the `Makefile`. See
[`third_party/libpitrex/NOTICE.md`](../third_party/libpitrex/NOTICE.md) for
provenance and licensing.

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| Nothing happens at all | Re-seat the card; confirm `out/vekterm.img` flashed cleanly. Connect the host RX to Pi TX (GPIO14) to watch the "PiTrex starting…" boot message. |
| Boots but draws nothing | Calibration: widen `VT_VECTREX_MAX`, lower `VT_BRIGHT_SHIFT`. Confirm the sender is connected to **GPIO15 (RX)** and sharing **GND**. |
| Garbled / no vectors over serial | Baud mismatch or a marginal adapter at 2 Mbaud. Rebuild with `-DVT_UART_BAUD=115200` and set the sender to 115200. |
| Image won't boot on a Pi 2/3/Zero 2 | This targets the **Pi Zero / Zero W (BCM2835, ARMv6)**. Other models need different firmware/flags. |
| Want to test without a Vectrex | `./vekterm --dry-run` (host build) decodes and reports without any hardware. |

[hwguide]: http://www.ombertech.com/cnk/pitrex/wiki/index.php?wiki=Developer_Release_HW_Guide
