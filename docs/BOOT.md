# How a PiTrex boots baremetal — the official distribution, and vekterm

vekterm boots the same way the **official PiTrex baremetal distribution**
([gtoal/pitrex](https://github.com/gtoal/pitrex) and the newer
[malbanGit/pitrex-baremetal](https://github.com/malbanGit/pitrex-baremetal)) does,
because it is built on the same libpitrex runtime. This document records how the
official distribution boots and runs its `vterm`, and how vekterm differs — and
it explains the boot/clock/UART bug this branch fixes.

## The official distribution

### What's on the card

A PiTrex SD card is a single FAT partition with the **stock Raspberry Pi GPU
firmware** plus a baremetal kernel — no Linux:

| File | Role |
| --- | --- |
| `bootcode.bin`, `start.elf`, `fixup.dat` | Raspberry Pi GPU firmware |
| `config.txt` | clocks, GPIO setup for the cartridge bus, which kernel to load |
| `pitrex.img` / `kernel.img` (`kernel7.img` on ARMv7) | the baremetal kernel |
| `loader.pit` | a relocatable menu/loader |
| `vterm.img`, `cine.img`, `asteroids_sbt.img`, … | the individual programs |

The premise that explains everything: the Pi's own CPU is **halted** (`#HALT`
jumpered to GND) and the Pi GPIO **bit-bangs the Vectrex cartridge bus** to talk
to the real 6522 VIA inside the Vectrex at `$D000–$D00F`. There is no emulated VIA
on the Pi.

### Boot sequence

```
power on → GPU runs bootcode.bin → start.elf reads config.txt → loads the kernel
           image to 0x8000 and releases the ARM
        → the kernel framework (bareMetalMain.c kernelMain): set the ARM clock,
          bring up the mini-UART, print a banner, then call main() forever
```

In the official distribution `main()` is a **bootstrap** (`bootStrapMain.c`): it
mounts the FAT card, reads `loader.pit` to a high address, and jumps to it. The
loader (`loaderMain.c`) calls `vectrexinit(1)` + `v_init()` to bring up the
Vectrex display, plays a startup jingle, and draws a **menu on the Vectrex
screen**. Each program is its own standalone `*.img`; selecting one `f_read`s it
to `0x8000` and jumps there. So programs aren't linked together — the loader
relocates itself out of the way, then overwrites `0x8000` with the chosen image.

### Selecting and running the official `vterm`

On the menu, the **Vectrex joystick Y** scrolls the (brightness-ramped) list and
**button 3** selects; the loader loads `vterm.img` to `0x8000` and runs it.

The official `vterm` ([`vterm/vterm.c`](https://github.com/malbanGit/pitrex-baremetal/blob/main/vterm/vterm.c))
turns the PiTrex into a **live vector display driven over serial by MAME** at
**921600 baud, 8N1**. Its own framing (handshake `"PiTrex VTerm Connect"`, then
per-frame `dataMode/size` headers with optional delta/Huffman compression) carries
vector primitives (`VTERM_SINGLE_LINE`, `VTERM_START_OF_PATH`, `VTERM_DOT`,
`VTERM_BRIGHNESS`, …) that it draws with `v_directDraw32`, exactly the libpitrex
call vekterm uses.

> Note: the official `vterm` protocol is **not** the USB-DVG/vecterm protocol.
> vekterm is the receiver for the USB-DVG/vecterm protocol that
> [pyvterm](https://github.com/anarkiwi/pyvterm) (or a custom MAME build) speaks
> — a clean-room, independent counterpart. Both end up drawing through the same
> libpitrex `v_directDraw32`, which is why the same VIA/Vectrex model
> ([`tools/emu`](../tools/emu), see [`EMULATOR.md`](EMULATOR.md)) renders either.

## vekterm's boot

vekterm collapses the bootstrap+loader+menu into a single kernel: the Pi boots
**straight into the receiver**, no menu, no program selection. The framework
`kernelMain()` still runs first (clock + banner), then calls vekterm's `main()`
([`src/vekterm_baremetal.c`](../src/vekterm_baremetal.c)) which does
`vectrexinit` → `v_init` → and then loops: drain the mini-UART into the parser,
`v_WaitRecal`, and redraw the active frame (or the idle splash).

Two kernels ship because the peripheral base and CPU arch are fixed at compile
time — `kernel.img` (Pi Zero/W, BCM2835/ARMv6) and `kernel7.img` (Pi Zero 2 W,
BCM2837/ARMv7); the firmware loads the right one per `config.txt`'s `[pi0]` /
`[pi02]` filters. See [`DEPLOY.md`](DEPLOY.md) for the card layout.

## The boot/clock/UART bugs, and the fixes

Two distinct defects sat on top of each other. The first stops the board from
producing **any** output; the second lets it boot but never decode data.

### Bug 1 — no boot messages at all (the mini-UART is never initialised)

`kernelMain()` brought the mini-UART up **only inside** `if (arm_clock != target)`:

```c
int32_t arm_clock = lib_bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_ARM);
if (arm_clock != 1000000000) {            // MHZ1000 build
    lib_bcm2835_vc_set_clock_rate(ARM, 1000000000);
    RPI_AuxMiniUartInit(921600, …);        // <-- only here!
}
printf("PiTrex starting…");                // goes nowhere if the branch was skipped
```

`deploy/config.txt` sets `force_turbo=1`, so the firmware brings the Pi Zero 2 W
up at **exactly its 1 GHz turbo frequency before the kernel runs**. So
`arm_clock == 1000000000`, the branch is skipped, `RPI_AuxMiniUartInit` is never
called — the AUX mini-UART stays disabled and GPIO14/15 are never switched to
ALT5 — and every `printf` (the banner *and* `v_init`'s chatter) goes to an
unconfigured UART. **Result: a board that runs but emits nothing on the header —
"doesn't boot, no messages".** (The Pi Zero boots at 700 MHz, so the branch ran
and it worked there — which is why this looked Zero-2-W-specific.)

The fix ([`bareMetalMain.c`](../third_party/libpitrex/baremetal/bareMetalMain.c))
makes UART bring-up **unconditional** — it's independent of the CPU clock — while
the (still-conditional) clock set stays where it was.

### Bug 2 — boots, but no frames decode (data-link baud off)

The mini-UART (which vekterm uses for both banner and data) is **clocked by the
VPU/core clock**, and its baud is `core_freq / (8 × (divisor + 1))` with the
divisor **truncated to an integer**
([`RPI_AuxMiniUartInit`](../third_party/libpitrex/baremetal/rpi-aux.c)). So the
value the kernel divides by must equal the *actual* core clock, and the target
baud must land the divisor on (near) an integer:

| | core actually | banner divides from | banner result | 2 Mbaud data divides from | data result |
| --- | --- | --- | --- | --- | --- |
| original (`main`) | 250 MHz | 400 MHz assumed | **~72 kbaud — garbled** | 400 MHz assumed | **1.25 Mbaud — 37% off, dead** |
| this branch, step 1 | 250 MHz | 250 MHz | 919 kbaud ≈ 921600 ✓ | 250 MHz (÷ truncates to 14) | 2.083 Mbaud, **+4.2% — past 8N1 tolerance** |
| **this branch, fixed** | **256 MHz** | 256 MHz | 914 kbaud ≈ 921600 ✓ | 256 MHz (÷ = 15 exact) | **2.000 Mbaud — exact ✓** |

The original code assumed a 400 MHz core while `config.txt` pinned 250 MHz, so the
banner was garbled and the data link 37% off. Fixing the clock to 250 MHz made the
banner readable, but **2 Mbaud doesn't divide cleanly from 250 MHz** —
`250e6/(8·2e6) = 15.625` truncates to divisor 14, i.e. 2.083 Mbaud, a +4.2% error
8N1 framing can't tolerate. The fix pins the core at **256 MHz**, chosen so the
2 Mbaud default is exact (`256e6/(8·16) = 2,000,000`, divisor 15) while keeping
pyvterm's 2 Mbaud default and the full link bandwidth. 256 vs 250 MHz doesn't
affect booting or the VIA bus timing (the busy-wait/cycle delays derive from the
**ARM** clock, pinned at 1 GHz, not the core clock). The three places that must
agree are now consistent:

- [`deploy/config.txt`](../deploy/config.txt): `core_freq=256`
- [`src/vekterm_baremetal.c`](../src/vekterm_baremetal.c): `VT_UART_CLOCK 256000000` (data link)
- [`bareMetalMain.c`](../third_party/libpitrex/baremetal/bareMetalMain.c): effective banner clock `258048000` (≈921600 from a 256 MHz core)

If you change `VT_UART_BAUD`, change `core_freq` (and `VT_UART_CLOCK`) to keep
`core/(8·baud)` integral — e.g. `-DVT_UART_BAUD=1000000` is exact at 256 MHz, and
`115200` is ~0.1% off at any of these clocks for a slow, rock-solid link.

## Verifying the boot

- **The drawing** (vekterm boots to the splash and draws frames) is proven
  off-target by the draw-path emulator: `make emu-test`, and the rendered
  `out-emu/splash.png`. See [`EMULATOR.md`](EMULATOR.md).
- **The ARM bring-up** (entry asm → VFP → MMU → clock → mini-UART → banner) is
  proven in QEMU: [`tools/emu/qemu_boot.sh`](../tools/emu/qemu_boot.sh) loads a
  kernel at `0x8000` (the Pi firmware's load address; QEMU's own `-kernel` uses
  `0x10000`, so we place it explicitly) on the mini-UART and asserts it reaches
  `PiTrex starting…`:

  ```
  $ tools/emu/qemu_boot.sh out/kernel.img raspi0
  PiTrex starting...
  ARM CLOCK  : 700000000 MHz
  ...
  FREESTANDING: bcm2835_init()        # then blocks in vectrexinit() on the
  PASS: kernel booted to the banner   # never-toggling RDY line — no VIA in QEMU
  ```

  Two limits of QEMU here: it boots the **raw kernel without the firmware**, so it
  never reads `config.txt` and never applies `force_turbo` — the ARM comes up at
  700 MHz, not 1 GHz, so QEMU can't reproduce **Bug 1's** exact trigger. And its
  BCM2835 mini-UART model emits to the host even when the AUX/GPIO aren't
  configured, so it wouldn't show a *missing* UART even if it did. QEMU proves the
  boot *path* runs to the banner; Bug 1 is established by inspection (above) and
  on real hardware.
- **On real hardware**, wire the host's RX to the Pi's TX (GPIO14) and watch at
  **921600 8N1** for `PiTrex starting…`, the clock report, and vekterm's
  `init complete; … link -> 2000000 baud` line. With the fix the banner now
  appears on the Zero 2 W (Bug 1) and is readable (Bug 2); the link then switches
  to 2 Mbaud for data. Point pyvterm at 2000000 baud and frames should draw.
