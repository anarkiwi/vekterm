# vekterm

[![CI](https://github.com/anarkiwi/vekterm/actions/workflows/ci.yml/badge.svg)](https://github.com/anarkiwi/vekterm/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**The PiTrex-side receiver for the USB-DVG / *vecterm* serial protocol — draw
vectors streamed over a serial port onto a real [Vectrex](https://en.wikipedia.org/wiki/Vectrex).**

vekterm is the **device half** of the protocol that
[pyvterm](https://github.com/anarkiwi/pyvterm) (and a custom
[MAME](https://www.mamedev.org/) build) speaks on the **sending** half. It runs
on the [PiTrex](https://github.com/gtoal/pitrex) — a Raspberry Pi Zero riding on
a Vectrex — reads command words off a serial link, decodes them into vectors,
and drives the Vectrex beam through the PiTrex's `libpitrex` (the 6522 VIA).

```
┌─────────────────────────┐   USB-DVG / vecterm    ┌──────────────────────┐   GPIO/VIA   ┌─────────┐
│  pyvterm  ── or ──        │   protocol over a      │  vekterm  on the      │  6522 VIA    │ Vectrex │
│  a custom MAME build      │ ─────────────────────▶ │  PiTrex (this repo)   │ ───────────▶ │ CRT     │
│  (the sender)             │   serial @ 2 Mbaud     │  decodes + draws      │              │ (beam)  │
└─────────────────────────┘                         └──────────────────────┘              └─────────┘
```

It is wire-compatible with pyvterm by construction: the unit tests decode
pyvterm's exact frame bytes (the worked example in
[`docs/PROTOCOL.md`](docs/PROTOCOL.md)) back into the vectors they represent.

## How it's built

The codebase is split so that everything except the final hardware draw is pure,
portable C99 that builds and is unit-tested on any machine — the same separation
pyvterm draws between its pure `protocol`/`frame` modules and its `Transport`s.

| Module | Role | Pure? |
| --- | --- | --- |
| [`src/protocol.c`](src/protocol.c) | decode/encode 32-bit command words, colour & coordinate maths | ✅ host-testable |
| [`src/frame.c`](src/frame.c) | streaming parser: bytes → words → a frame of vectors (handles split reads, `MAX_PIPELINE`) | ✅ host-testable |
| [`src/serial.c`](src/serial.c) | open the serial port (raw 8N1, non-blocking) | POSIX |
| [`src/backend_stub.c`](src/backend_stub.c) | report frames; no hardware (host, CI, `--dry-run`) | ✅ |
| [`src/backend_pitrex.c`](src/backend_pitrex.c) | draw frames on the Vectrex via `libpitrex` | needs hardware |
| [`src/vekterm.c`](src/vekterm.c) | CLI, the read→decode→refresh loop | — |

## Build & test (no hardware needed)

```bash
make          # builds ./vekterm with the stub backend
make test     # builds and runs the unit tests
make check    # tests + a syntax check of the pitrex backend
./vekterm --selftest
```

`make` needs only a C99 compiler and GNU make. Decode a captured frame without a
Vectrex anywhere in sight:

```bash
# A single white line (the docs/PROTOCOL.md worked example) as raw bytes:
printf '\x80\x00\x01\x90\x20\xF0\xF0\xF0\x52\x00\x48\x02\x42\x64\x48\x02\x60\x00\x00\x05\x00\x00\x00\x00' > frame.bin
./vekterm --dry-run --input frame.bin --once --verbose
# frame 1: 1 vector(s), beam_travel=400
#     (2049,2050)->(2449,2050) z=240
```

`--dry-run` (and any build without `libpitrex`) uses the stub backend, so the
whole decode pipeline runs and reports on an ordinary computer. Pipe straight
from a sender, too: `python my_sender.py | ./vekterm --dry-run --input -`.

## Run in Docker

A multi-stage [`Dockerfile`](Dockerfile) compiles the receiver, runs the unit
tests, and ships a statically linked binary in a bare `scratch` image (~700 kB):

```bash
make docker                              # or: docker build -t vekterm .
docker run --rm vekterm --selftest
printf '\x80\x00\x01\x90\x20\xF0\xF0\xF0\x52\x00\x48\x02\x42\x64\x48\x02\x60\x00\x00\x05\x00\x00\x00\x00' \
  | docker run --rm -i vekterm --dry-run --input - --verbose
```

The image builds the hardware-independent (stub) server — ideal for CI, decoding
captured frames, and piping from a sender. Driving a real Vectrex needs libpitrex
and GPIO access on the Pi itself (see [Deploy](#deploy-boot-straight-into-the-receiver)),
which is the systemd path below, not a container.

## Build for the PiTrex (drives a real Vectrex)

On the Pi, with a [gtoal/pitrex](https://github.com/gtoal/pitrex) checkout whose
`libpitrex` is built:

```bash
make pitrex PITREX_DIR=/home/pi/pitrex   # produces ./vekterm-pitrex
sudo ./vekterm-pitrex --port /dev/ttyGS0
```

The pitrex backend calls the same primitives pyvterm's protocol notes reference
— `v_WaitRecal` (pace each 50 Hz refresh and re-zero the beam) and
`v_directDraw32` (draw one absolute vector at a given intensity) — re-drawing the
active frame every refresh, because a vector display has no persistence.

Coordinate and brightness mapping are tunable for your hardware:
`--min/--max` set the Vectrex range device `0..4095` maps onto, `--scale` sets
the integrator scale register, and `--bright-shift` scales 0..255 intensity onto
the Vectrex z-axis. See `./vekterm --help`.

## Deploy: boot straight into the receiver

Two scripts in [`deploy/`](deploy/) turn a Pi into an appliance that powers on
and immediately starts receiving:

```bash
sudo deploy/setup-usb-gadget.sh   # present /dev/ttyGS0 (USB-CDC serial gadget)
deploy/install.sh                 # build, install /usr/local/bin/vekterm + service
sudo reboot
```

After the reboot the host PC sees `/dev/ttyACM0`; point pyvterm at it and draw.
The full walk-through (USB-gadget vs UART wiring, the systemd unit, calibration,
and troubleshooting) is in [`docs/DEPLOY.md`](docs/DEPLOY.md).

## Documentation

- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — the wire protocol from the receiver's
  point of view, with the byte-for-byte worked example the tests assert.
- [`docs/DEPLOY.md`](docs/DEPLOY.md) — deploy a PiTrex that boots into vekterm.

## Credits

The protocol and the PiTrex platform are the work of Graham Toal and contributors
([gtoal/pitrex](https://github.com/gtoal/pitrex)); the USB-DVG drivers it mirrors
were written by Mario Montminy. vekterm is the receiving counterpart to
[pyvterm](https://github.com/anarkiwi/pyvterm), an independent clean-room
implementation of the protocol.

## License

Apache-2.0. See [LICENSE](LICENSE).
