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
| RX  | pin 8  = GPIO14 (TXD) — **required** (carries the flow-control "ready" byte) |
| GND | pin 6  = GND |

The adapter must be **3.3 V (never 5 V)**. **Wire RX too** (not just TX): vekterm
paces the sender with a per-frame handshake over its TX line — the Pi's 8-byte
mini-UART FIFO can't otherwise keep up while it draws, and a single dropped byte
desyncs the stream. pyvterm must run with **flow control on** (`--flow-control`,
or `flow_control=0x06` on the terminal).

The default line rate is **1,280,000 baud** — fast, and chosen because it's exact
from the pinned 256 MHz core *and* reliable over real 3.3 V USB-TTL wiring (2 Mbaud
is exact too but proved marginal — bit errors corrupt frames). A genuine FTDI
FT232R or CP2102N is recommended. To go faster/slower, rebuild with
`VEKTERM_CFLAGS='-DVT_UART_BAUD=…'` (exact at 256 MHz: 2.0M, 1.6M, 1.28M, 1.0M,
800k, 640k, 500k; or `115200` for a rock-solid slow link) and match the sender.

## Step 4 — Boot

Insert the card, power on. The Pi boots straight into vekterm. With nothing yet
connected to the UART, the Vectrex shows a `VEKTERM / WAITING FOR DATA` splash
(with the expected line settings) — that confirms the board booted and is
listening. The splash clears as soon as the first frame arrives. On the host:

```bash
pip install pyvterm
python -c "
from pyvterm import VectorTerminal, DEFAULT_SYNC_BYTE
with VectorTerminal(port='/dev/ttyUSB0', baudrate=1280000,
                    flow_control=DEFAULT_SYNC_BYTE) as vt:  # your adapter's port
    with vt.frame():
        vt.set_intensity(15)
        vt.polyline([(-200,-200),(200,-200),(200,200),(-200,200)], closed=True)
"
```

A square should appear on the Vectrex. (`flow_control` is the handshake; without
it the sender overruns the receiver and nothing draws.) pyvterm's
`examples/testpattern.py --flow-control` draws a calibration grid to start from.

## Calibration

Coordinate and intensity mapping depend on your Vectrex. These are compile-time
options; set them via `VEKTERM_CFLAGS` (Docker) or `EXTRA_CFLAGS` (local
`make baremetal`), then re-flash.

| Define | Meaning | Default |
| --- | --- | --- |
| `VT_VECTREX_MIN` / `VT_VECTREX_MAX` | Vectrex coords that device `0` / `4095` map to | `-10000` / `10000` |
| `VT_BRIGHT_SHIFT` | beam intensity = brightness (0..255) `>> N` | `1` |
| `VT_UART_BAUD` | serial line rate (must match the sender; exact rates at 256 MHz only) | `1280000` |
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
| [`config.txt`](../deploy/config.txt) | GPIO setup for the VIA, pins the core clock at 256 MHz so the mini-UART baud is exact |
| `kernel.img` | vekterm itself |

There is deliberately **no** Linux kernel, initramfs, or root filesystem.

### The serial link (why flow control)

vekterm reads a raw mini-UART with only an **8-byte RX FIFO**, and it spends most
of each frame *drawing* (not reading the UART), so at any real line rate the FIFO
would overflow and drop bytes — and the stream has no byte-level resync, so one
dropped byte corrupts everything after it. So the link is **flow-controlled**,
the way the official PiTrex `vterm` paces MAME: vekterm sends a one-byte "ready"
(`0x06`) on its TX when it can receive, and the sender transmits exactly one
frame per ready. Nothing then arrives while vekterm draws. (vekterm also drains
the FIFO into a 16 KB ring buffer and re-aligns the parser at each frame
boundary for good measure.) The whole pipeline — pyvterm's encoder + handshake,
the wire, vekterm's receive/parse/draw — is proven end-to-end off-hardware by
`make emu-e2e` (see [`EMULATOR.md`](EMULATOR.md)); the only thing that needs real
silicon is the electrical layer, which is why the default baud is a reliable
1.28M rather than the marginal 2M.

The core clock is pinned at **256 MHz** (not the more common 250) on purpose: the
mini-UART baud is `core / (8 × (divisor+1))` with the divisor truncated, and the
2 Mbaud default is only exact at 256 MHz (`256e6/(8·16)`). For the full boot
sequence, the official PiTrex/`vterm` flow, and this clocking, see
[`BOOT.md`](BOOT.md); to see what vekterm draws without hardware, see
[`EMULATOR.md`](EMULATOR.md).

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
| Splash, but blank when streaming | Sender not using flow control (`--flow-control`), or RX wire (Pi TX→adapter) missing. Both are required — see Step 3. |
| Blank/garbled at a high baud | Marginal wiring at that rate. Drop a step (e.g. 1.28M→1.0M); `-DVT_UART_BAUD=115200` is bulletproof. Match the sender. |
| Nothing on a Pi Zero 2 W | The firmware loads `kernel7.img` on that board (`config.txt` `[pi02]`). Confirm both `kernel.img` and `kernel7.img` are on the card. The Pi 4/5 are **not** supported (different firmware/base). |
| Want to test without a Vectrex | `./vekterm --dry-run` (host build) decodes and reports without any hardware. |

[hwguide]: http://www.ombertech.com/cnk/pitrex/wiki/index.php?wiki=Developer_Release_HW_Guide
