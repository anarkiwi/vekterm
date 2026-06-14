# The USB-DVG / *vecterm* protocol — receiver's view

This is the wire protocol that [`gtoal/pitrex`](https://github.com/gtoal/pitrex)
uses to let a **sender** (a custom MAME build, or
[pyvterm](https://github.com/anarkiwi/pyvterm)) drive a PiTrex/Vectrex over a
serial port. vekterm implements the **receiving** side; this document describes
what vekterm reads and how it turns it into beam motion.

The full sender-side specification — provenance, every field, and the
AdvanceMAME cross-check — lives in pyvterm's
[`docs/PROTOCOL.md`](https://github.com/anarkiwi/pyvterm/blob/main/docs/PROTOCOL.md).
The encoders in [`src/protocol.c`](../src/protocol.c) reproduce that format
exactly so the unit tests can decode a sender's bytes and assert the result.

## 1. Link

| Setting | Value |
| --- | --- |
| Device (on the Pi) | `/dev/ttyGS0` (USB-CDC gadget) or `/dev/serial0` (UART) |
| Nominal baud | `2000000` (2 Mbaud); USB-CDC ignores the line rate |
| Framing | 8 data bits, no parity, 1 stop bit, no flow control, raw mode |

vekterm configures the port this way in [`src/serial.c`](../src/serial.c) and
reads it non-blocking, feeding bytes to the parser as they arrive. A command
word may be split across reads; the parser reassembles whole words before
acting (asserted by `test_words_split_across_reads`).

## 2. Command words

Every command is a single **32-bit word, big-endian**. The top three bits
[31:29] select the command:

```
 bit  31      29 28                                              0
      +---------+------------------------------------------------+
      |  flag   |  payload (command-specific)                    |
      +---------+------------------------------------------------+
```

| Flag | Value | vekterm action |
| --- | --- | --- |
| `COMPLETE` | `0x0` | End of frame — publish it as the active frame to redraw |
| `RGB` | `0x1` | Set current intensity = brightest of r,g,b |
| `XY` | `0x2` | `blank=1` move the beam; `blank=0` draw to the point |
| `QUALITY` | `0x3` | Render hint — ignored (geometry is unaffected) |
| `FRAME` | `0x4` | Start a new frame; remember its beam-travel length |
| `CMD` | `0x5` | Device command channel — not used by the pitrex variant; ignored |
| `EXT` | `0x6` | Reserved extensions container — ignored (forward-compatible) |
| `EXIT` | `0x7` | Session over |

`RGB`, `XY`, and the payload layouts are decoded in `vt_decode_word`
([`src/protocol.c`](../src/protocol.c)):

- **RGB** — `[23:16]=r [15:8]=g [7:0]=b`. The Vectrex is monochrome, so vekterm
  takes the brightest channel as the beam intensity (`vt_brightness_from_rgb`).
  A black `(0,0,0)` colour yields intensity 0; senders pair it with the `XY`
  blank bit, so such "draws" arrive blanked and produce no lit vector.
- **XY** — `[28]=blank [27:14]=x [13:0]=y`, with `x`,`y` in device units
  `0..4095`.

## 3. Drawing semantics

vekterm keeps a **current beam position** and **current intensity** while it
parses a frame:

1. A **blanked** `XY` (`blank=1`) just repositions the beam — no vector.
2. A **lit** `XY` (`blank=0`) appends a vector from the current beam position to
   the new point at the current intensity, then advances the beam.

Because step 1 only happens when a run starts, a connected polyline costs one
reposition for the whole run and shares interior vertices
(`test_connected_polyline_shares_vertices`). The first lit `XY` of a frame with
no preceding move starts from screen center (where `v_WaitRecal` leaves the
beam).

`FRAME` begins a fresh frame and clears intensity to 0; `COMPLETE` finishes it.
vekterm buffers at most `MAX_PIPELINE` (3000) vectors per frame — matching the
receiver limit in `pitrex/vectrex/vectrexInterface.h` — and flags any overflow.

## 4. From a frame to light

A vector display has no persistence, so a completed frame becomes the **active
frame** and is re-drawn every refresh until the next `COMPLETE` arrives. On real
hardware ([`src/backend_pitrex.c`](../src/backend_pitrex.c)) each refresh is:

```c
v_WaitRecal();                          /* wait for the 50 Hz refresh, re-zero the beam */
for (each vector in the active frame)
    v_directDraw32(x0, y0, x1, y1, z);  /* draw it at intensity z */
```

Device coordinates `0..4095` are mapped linearly onto the Vectrex integrator
range (`vt_map_coord`, default `-2048..2047`, tunable with `--min/--max`), and
intensity `0..255` is shifted onto the Vectrex z-axis (`--bright-shift`).

## 5. Worked example

A single white line from host `(0,0)` to `(100,0)` — the exact bytes pyvterm
emits and that [`tests/test_frame.c`](../tests/test_frame.c) feeds the parser:

| Word | Bytes (hex) | Meaning | vekterm result |
| --- | --- | --- | --- |
| `FRAME`    | `80 00 01 90` | beam-travel = 400 | start frame |
| `RGB`      | `20 F0 F0 F0` | white (240) | intensity = 240 |
| `XY` blank | `52 00 48 02` | device `(2049, 2050)` | move beam there |
| `XY` draw  | `42 64 48 02` | device `(2449, 2050)` | **1 vector** `(2049,2050)→(2449,2050)`, z=240 |
| `QUALITY`  | `60 00 00 05` | quality 5 | ignored |
| `COMPLETE` | `00 00 00 00` | end of frame | publish frame |

Run it yourself: `./vekterm --selftest`.

## References

- Sender-side spec & provenance — pyvterm
  [`docs/PROTOCOL.md`](https://github.com/anarkiwi/pyvterm/blob/main/docs/PROTOCOL.md)
- PiTrex sender — `VMMenu/Win32/dvg/zvgFrame.c` <https://github.com/gtoal/pitrex>
- Receiver primitives — `pitrex/vectrex/vectrexInterface.h`
  (`v_WaitRecal`, `v_directDraw32`, `v_setScale`, `MAX_PIPELINE`)
- AdvanceMAME canonical sender — `advance/osd/dvg.c`
