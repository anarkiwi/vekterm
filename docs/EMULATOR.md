# Emulating the PiTrex + Vectrex off-target

vekterm draws on a Vectrex by driving a **6522 VIA** that, on a real PiTrex, is
genuine Vectrex silicon on the cartridge bus. That makes "does it draw the right
thing?" hard to answer without a Vectrex on the bench — and it was the root of a
lot of unproductive on-hardware UART debugging.

This directory ([`tools/emu`](../tools/emu)) removes the hardware from the loop.
It runs vekterm's **real** draw path — the vendored libpitrex
[`vectrexInterface.c`](../third_party/libpitrex/vectrex/vectrexInterface.c), the
VIA register macros, the vector font, and the real
[`protocol.c`](../src/protocol.c)/[`frame.c`](../src/frame.c) parser — on top of a
**software model of the 6522 VIA and the Vectrex analog vector generator**. Every
beam move the code makes is reconstructed into line segments and rendered to an
image: what the Vectrex CRT *would* show. No Pi, no QEMU, no Vectrex.

```
   vekterm's real draw code                      this emulator
 ┌───────────────────────────┐   vectrexwrite() ┌──────────────────────────┐
 │ v_printString / v_directDraw32              │ │ software 6522 VIA +       │
 │ → SET_YSH16/SET_XSH16/T1/CB2  ─────────────▶ │ Vectrex integrators       │ → segments → PPM/PNG
 │   (libpitrex, unmodified)   │  vectrexread() │ (via_vectrex.c)           │
 └───────────────────────────┘ ◀───────────────└──────────────────────────┘
```

## Quick start

```bash
make emu                              # build out-emu/vekterm-emu (+ gen_frame)
out-emu/vekterm-emu --splash --out out-emu/splash.ppm   # the idle splash
out-emu/gen_frame square > sq.bin
out-emu/vekterm-emu --frame sq.bin --out out-emu/square.ppm
convert out-emu/splash.ppm out-emu/splash.png           # optional, ImageMagick

make emu-test                         # build + render + assert the geometry
```

Modes: `--splash` (the `VEKTERM / WAITING FOR DATA` idle screen vekterm shows
before the first frame), `--frame f.bin` (decode a USB-DVG/vecterm byte stream
and draw it), `--pipeline` (use libpitrex's default pipeline path instead of the
direct path — both reconstruct identical geometry), `--dump` (print the
reconstructed segments), `--fixed` (render on a nominal Vectrex-screen canvas
rather than fitting the content).

## How it boots vekterm without a Pi

`vekterm-emu` mirrors [`vekterm_baremetal.c`](../src/vekterm_baremetal.c)'s
startup and draw loop: `vectrexinit(1)` → `v_setName` → `v_init` → `v_setRefresh`
→ draw the splash (or a decoded frame). The only things it substitutes for the Pi
are the four libpitrex I/O symbols, which [`via_vectrex.c`](../tools/emu/via_vectrex.c)
implements against the model instead of GPIO:

| Symbol | On the Pi | In the emulator |
| --- | --- | --- |
| `vectrexwrite(addr,data)` | bit-bang a VIA register over the cartridge bus, busy-wait on RDY | feed the software VIA + integrators |
| `vectrexread(addr)` | bit-bang a read, busy-wait on RDY | return VIA state; flags read "ready" so wait-loops fall through |
| `vectrexinit()` | bring up GPIO, check `#HALT` | no-op (returns "VIA present") |
| `vectrexwrite_short()` | fast-path write | same as `vectrexwrite` |

Everything above that line — the drawing — is the unmodified libpitrex code that
runs on the Pi. The host build compiles `vectrexInterface.c` via its Linux (non
`FREESTANDING`) path, which uses the same VIA register sequences but no ARM inline
assembly; the only vendored change is a `__arm__` guard around two cycle-delay
busy-loops (pure timing, no effect on geometry — see
[`third_party/libpitrex/NOTICE.md`](../third_party/libpitrex/NOTICE.md)).

## End-to-end: real pyvterm → real vekterm (`make emu-e2e`)

The splash/frame modes above drive the draw path directly. To prove the *whole*
pipeline — including the receive side and the flow-control handshake — there is a
second harness ([`tools/emu/emu_vekterm.c`](../tools/emu/emu_vekterm.c)) that runs
the **actual `src/vekterm_baremetal.c` main loop** unmodified (the handshake, the
UART receive, the flush/resync, the real parser, the real draw) against the VIA
model, and connects it over a **PTY** to **real pyvterm** (its real
`SerialTransport` with flow control on, forked as a subprocess). Bytes flow
pyvterm → PTY → vekterm exactly as over a wire; only the lowest-level shims (the
mini-UART/`uart_rx` read/write the PTY, the "system timer" reads the host clock)
are substituted.

```bash
make emu-e2e        # needs ../pyvterm + python3 + pyserial; writes out-emu/e2e.png
```

If the streamed pattern renders, the entire software path is correct and any
on-hardware trouble is the electrical layer (baud/signal integrity) — which is
exactly how the "blank screen while streaming" issue was isolated to a too-fast
baud rather than a logic bug.

## The VIA + Vectrex model

The Vectrex has no framebuffer: one 6522 VIA feeds an 8-bit DAC, a mux, two
**integrators** (X and Y) and a Z (brightness) sample/hold. The CPU sets *rates*
and *durations*; the analog hardware integrates rate × time into beam *position*.
[`via_vectrex.c`](../tools/emu/via_vectrex.c) models exactly that, keyed off the
register writes libpitrex makes to draw one vector:

| What libpitrex writes | VIA effect | Model |
| --- | --- | --- |
| `Port B = 0x80`, `Port A = y` | mux → Y integrator, set Y rate | `y_rate = (int8)y` |
| `Port B = 0x81`, `Port A = x` | mux off, DAC drives X directly | `x_rate = (int8)x` |
| `Port A`, `Port B = 0x84` | mux → Z, latch brightness | `intensity = data` |
| write `T1` high byte | start Timer 1 → drop `/RAMP`, beam integrates for `scale` ticks | arm a ramp of `scale + SCALE_STRENGTH_DIF` ticks |
| `VIA_cntl = 0xEE` / `0xCE` | CB2 = `/BLANK` high/low → beam on/off | `beam_on = …` |
| `VIA_cntl = 0xCC` | CA2 = `/ZERO` low → integrators to 0 | snap beam to centre |

When a ramp is committed the beam advances by `rate × ticks` on each axis; if the
beam was on and Z is non-zero, that move is emitted as a lit segment. Because
libpitrex programs the per-axis strength as `delta / (scale + SCALE_STRENGTH_DIF)`,
integrating over `scale + SCALE_STRENGTH_DIF` ticks inverts it and recovers the
original endpoint to within the integer-DAC quantisation — the same quantisation
the real hardware draws with.

### Why this is faithful, not hand-wavy

`make emu-test` drives a **known square** (device corners 1024/3072, which
[`vt_map_coord`](../src/protocol.c) maps onto ±~5000 of the Vectrex range) through
the real protocol parser and the real draw code, and asserts the reconstruction
comes back as four segments whose corners sit within 1% of ±5000. A square in →
a square out, at the right coordinates, is the proof the VIA model is correct; the
splash then renders the vector font through the same path.

## What it can and can't prove

- **Can:** that vekterm's drawing is correct — the idle splash, and that a USB-DVG
  byte stream decodes and draws as the right vectors. Both the direct and the
  default pipeline draw paths are exercised and produce identical geometry.
- **Can't:** the cartridge-bus electrical timing or the firmware/clock config
  (`force_turbo`, the exact mini-UART baud), which live below `vectrexwrite()` and
  only exist on real silicon. The **ARM bring-up to the boot banner** *is*
  reproducible, but with a different tool — QEMU, driven by
  [`tools/emu/qemu_boot.sh`](../tools/emu/qemu_boot.sh) (load at `0x8000`,
  mini-UART on the second serial). QEMU runs the raw kernel without the firmware,
  so it can't apply `config.txt`'s `force_turbo`; that and its lenient mini-UART
  model are why the boot/clock bugs are established by inspection rather than by
  QEMU. See [`BOOT.md`](BOOT.md).
