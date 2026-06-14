# vekterm

[![CI](https://github.com/anarkiwi/vekterm/actions/workflows/ci.yml/badge.svg)](https://github.com/anarkiwi/vekterm/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**A baremetal PiTrex receiver for the USB-DVG / *vecterm* serial protocol — flash
an SD card, power on, and the Pi boots straight into drawing vectors streamed
over a serial port onto a real [Vectrex](https://en.wikipedia.org/wiki/Vectrex).**

vekterm is the **device half** of the protocol that
[pyvterm](https://github.com/anarkiwi/pyvterm) (or a custom
[MAME](https://www.mamedev.org/) build) speaks on the **sending** half. It runs
**baremetal** on the [PiTrex](https://github.com/gtoal/pitrex) — no operating
system — reading command words off the serial UART, decoding them into vectors,
and driving the Vectrex beam through the PiTrex's `libpitrex` (the 6522 VIA).

```
┌─────────────────────────┐   USB-DVG / vecterm    ┌──────────────────────┐   GPIO/VIA   ┌─────────┐
│  pyvterm  ── or ──        │   protocol over a      │  vekterm (baremetal)  │  6522 VIA    │ Vectrex │
│  a custom MAME build      │ ─────────────────────▶ │  on the PiTrex        │ ───────────▶ │ CRT     │
│  (the sender)             │   serial UART          │  decodes + draws      │              │ (beam)  │
└─────────────────────────┘                         └──────────────────────┘              └─────────┘
```

It is wire-compatible with pyvterm by construction: the unit tests decode
pyvterm's exact frame bytes (the worked example in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)) back into the vectors they represent.

## How it's built

The protocol decode is pure, portable C99 that builds and is unit-tested on any
machine; only the I/O is hardware-specific. The deployable artifact is a
baremetal `kernel.img`.

| Module | Role | Pure? |
| --- | --- | --- |
| [`src/protocol.c`](src/protocol.c) | decode/encode 32-bit command words, colour & coordinate maths | ✅ host-tested |
| [`src/frame.c`](src/frame.c) | streaming parser: bytes → words → a frame of vectors (split reads, `MAX_PIPELINE`) | ✅ host-tested |
| [`src/vekterm_baremetal.c`](src/vekterm_baremetal.c) | **the deployable app**: read the mini-UART, draw via `v_WaitRecal` + `v_directDraw32` | runs on the Pi |
| [`src/vekterm.c`](src/vekterm.c) + [`serial.c`](src/serial.c) + [`backend_stub.c`](src/backend_stub.c) | a host dev tool that decodes + reports a stream (CI, `--dry-run`) | ✅ |

## Build & test the decoder (no hardware)

```bash
make test        # run the unit tests
make             # build ./vekterm, the host decode/report tool
./vekterm --selftest
```

Decode a captured frame without a Vectrex anywhere in sight:

```bash
# A single white line (the docs/PROTOCOL.md worked example) as raw bytes:
printf '\x80\x00\x01\x90\x20\xF0\xF0\xF0\x52\x00\x48\x02\x42\x64\x48\x02\x60\x00\x00\x05\x00\x00\x00\x00' > frame.bin
./vekterm --dry-run --input frame.bin --verbose
# frame 1: 1 vector(s), beam_travel=400
#     (2049,2050)->(2449,2050) z=240
```

## Build the deployable artifacts (in Docker)

Everything — the cross-compile against `libpitrex` and the SD image — happens in
a container, so you need no local ARM toolchain:

```bash
make docker
# -> out/kernel.img   the baremetal binary (boots straight into vekterm)
# -> out/vekterm.img  a flashable SD-card image (firmware + config + kernel)
```

Under the hood the [`Dockerfile`](Dockerfile) clones gtoal/pitrex, cross-compiles
the baremetal receiver with `arm-none-eabi-gcc` against `libpitrex`
([`make baremetal`](Makefile)), and assembles a bootable FAT image with
[`deploy/build-image.sh`](deploy/build-image.sh) (unprivileged: `sfdisk` +
`mtools`, no loop mounts). Calibrate for your display without editing source:

```bash
docker build --target artifacts --output type=local,dest=out \
  --build-arg VEKTERM_CFLAGS='-DVT_VECTREX_MAX=12000 -DVT_UART_BAUD=115200' .
```

(Building locally instead of in Docker? `make baremetal PITREX_DIR=/path/to/pitrex`
needs `arm-none-eabi-gcc` + newlib; see [`docs/DEPLOY.md`](docs/DEPLOY.md).)

## Deploy: a PiTrex that boots into the receiver

```bash
make docker
# flash out/vekterm.img to a microSD (e.g. with Raspberry Pi Imager or):
#   sudo dd if=out/vekterm.img of=/dev/sdX bs=4M conv=fsync
```

Put the card in the PiTrex's Pi, wire the sender to the Pi's UART (GPIO14/15 at
3.3 V), and power on — it boots straight into vekterm, no login, no OS. Point
pyvterm at the other end of that serial link and draw. The full walk-through
(UART wiring, the `config.txt`, calibration, and how it boots) is in
[`docs/DEPLOY.md`](docs/DEPLOY.md).

## Documentation

- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — the wire protocol from the receiver's
  point of view, with the byte-for-byte worked example the tests assert.
- [`docs/DEPLOY.md`](docs/DEPLOY.md) — build, flash, wire, and boot a PiTrex into
  vekterm.

## Credits

The protocol and the PiTrex platform are the work of Graham Toal and contributors
([gtoal/pitrex](https://github.com/gtoal/pitrex)); the USB-DVG drivers it mirrors
were written by Mario Montminy. vekterm is the receiving counterpart to
[pyvterm](https://github.com/anarkiwi/pyvterm), an independent clean-room
implementation of the protocol.

## License

Apache-2.0. See [LICENSE](LICENSE).
