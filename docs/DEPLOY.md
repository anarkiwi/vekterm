# Deploying vekterm: a PiTrex that boots straight into the receiver

vekterm deploys **baremetal** — there is no operating system. The Pi's firmware
loads a vekterm kernel and runs it directly. Power on and the PiTrex is
immediately drawing whatever a sender streams over the serial link.

The image carries **two** kernels because the peripheral base address and CPU
arch differ per Pi family and are fixed at compile time; the firmware loads the
right one for the board:

- `kernel.img`  — Pi Zero / Zero W (BCM2835, ARMv6, peripherals `0x20000000`)
- `kernel7.img` — Pi Zero 2 W / 2 / 3 (BCM2837, ARMv7, peripherals `0x3F000000`)

```
power on ──▶ Pi firmware (bootcode.bin, start.elf) reads config.txt and loads
             kernel.img (Pi Zero) or kernel7.img (Pi Zero 2 W) per the SoC
         ──▶ vekterm runs: reads the mini-UART, draws the active frame on the
             Vectrex every refresh via libpitrex
```

## Prerequisites

- A built PiTrex: a Raspberry Pi Zero (W/WH) **or Pi Zero 2 W** on the PiTrex
  cartridge in a Vectrex, per the [PiTrex Hardware Guide][hwguide].
- A microSD card.
- **Docker** (to build the image — no local toolchain needed), or
  `arm-none-eabi-gcc` + newlib for a local build.
- On the sender side: a host running pyvterm and a **3.3 V** USB-to-TTL serial
  adapter (for the UART link, below).

## Step 1 — Build the image

```bash
make docker
# -> out/kernel.img   the baremetal binary (Pi Zero / Zero W)
# -> out/kernel7.img  the baremetal binary (Pi Zero 2 W / 2 / 3)
# -> out/vekterm.img  a flashable SD-card image (firmware + config.txt + both kernels)
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

Insert the card, power on. The Pi boots straight into vekterm. With nothing yet
connected to the UART, the Vectrex shows a `VEKTERM / WAITING FOR DATA` splash
(with the expected line settings) — that confirms the board booted and is
listening. The splash clears as soon as the first frame arrives. On the host:

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
| No splash, blank screen | The board isn't running (or can't drive the Vectrex). Re-seat the card; confirm `out/vekterm.img` flashed cleanly. Connect the host RX to Pi TX (GPIO14) to watch the "PiTrex starting…" boot message. |
| Splash shows but no vectors after connecting | The receiver is alive; the link or the sender is the issue. Confirm the sender is on **GPIO15 (RX)**, sharing **GND**, at the baud shown on the splash. |
| Splash and vectors but mispositioned/dim | Calibration: widen `VT_VECTREX_MAX`, lower `VT_BRIGHT_SHIFT`. |
| Garbled / no vectors over serial | Baud mismatch or a marginal adapter at 2 Mbaud. Rebuild with `-DVT_UART_BAUD=115200` and set the sender to 115200. |
| Nothing on a Pi Zero 2 W | The firmware loads `kernel7.img` on that board (`config.txt` `[pi02]`). Confirm both `kernel.img` and `kernel7.img` are on the card. The Pi 4/5 are **not** supported (different firmware/base). |
| Want to test without a Vectrex | `./vekterm --dry-run` (host build) decodes and reports without any hardware. |

[hwguide]: http://www.ombertech.com/cnk/pitrex/wiki/index.php?wiki=Developer_Release_HW_Guide
