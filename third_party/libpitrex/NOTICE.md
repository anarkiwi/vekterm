# Vendored libpitrex

This directory is a vendored, self-contained copy of the PiTrex library sources
needed to build the baremetal vekterm receiver. Vendoring removes the build-time
dependency on an external checkout so vekterm can track the latest toolchain and
apply fixes in-tree rather than patching a moving upstream at build time.

## Source

- Upstream: https://github.com/gtoal/pitrex
- Commit: `af698a79249f08adddd668896e5a256e47ab4dc4`
- Subtree: the `pitrex/{pitrex,vectrex,baremetal}` directories (the per-directory
  upstream Makefiles were dropped — vekterm builds these via its own top-level
  `Makefile`).

## Components and licenses

The PiTrex baremetal stack is an assembly of several upstream projects; their
original copyright and license notices are retained in the files themselves
(e.g. `baremetal/disclaimer.txt`, and the headers under `baremetal/lib2835/`):

- **Raspberry-Pi Bare Metal Tutorials** — Copyright (c) 2013 Brian Sidebotham,
  BSD-style license (`baremetal/rpi-*`, `baremetal/baremetalEntry.S`, etc.).
- **rpidmx512** — Copyright (c) Arjan van Vught, MIT license
  (`baremetal/lib2835/`, `pitrex/bcm2835.*`).
- **FatFs** — Copyright (c) ChaN, 1-clause BSD license (the `libff12c.a` archive
  and `baremetal/lib2835/ff.h` / `diskio.h` / `integer.h`).
- **PiTrex** — the Vectrex interface and GPIO glue (`vectrex/vectrexInterface.*`,
  `vectrex/osWrapper.*`, `vectrex/baremetalUtil.*`, `pitrex/pitrexio-gpio.*`) are
  the work of Graham Toal and PiTrex contributors; the upstream repository does
  not carry an explicit license file. They are vendored here unmodified except
  for the local changes below, with attribution to the upstream project.

vekterm's own code (everything outside `third_party/`) is Apache-2.0.

## Prebuilt archives

`baremetal/*.a` (`libff12c`, `libbcm2835`, `libbob`, `libconsole`, `libdebug`,
`libhal`, `libi2c`, `libutils`, `libarm`) are prebuilt static libraries shipped
by upstream (built from the rpidmx512 / FatFs sources above). They are vendored
as-is and linked into `kernel.img`. They are frozen binaries, so they do not
recompile with the toolchain; rebuilding them from source is a possible future
step toward full source self-containment.

These are the **ARMv6 / BCM2835** archives (peripherals at `0x20000000`), for
the Pi Zero / Zero W. Upstream also ships an **ARMv7 / BCM2837** variant of each
(its `lib7/` directories, peripherals at `0x3F000000`) for the Pi Zero 2 W / 2 /
3. Those are vendored under `baremetal/lib7/` and linked into `kernel7.img`. Both
sets are byte-for-byte the upstream archives (same commit), differing only in
target CPU/base; member lists are identical.

## Local modifications

Small, guarded fixes are baked in (search for `vekterm-vendor:`):

- `vectrex/vectrexInterface.c` — fallback `#define MAP_FAILED` (upstream defines
  it only in its non-baremetal branch, but common code references it).
- `baremetal/vectors.h` — add `#include <stdint.h>` and a `u_long` typedef
  (used by the header but not included upstream).
- `pitrex/bcm2835.h` — select `BCM2835_PERI_BASE` by Pi model (`0x3F000000` when
  built `-DRPI2`/`-DRPI3`, else `0x20000000`). The baremetal `bcm2835_init()`
  points `bcm2835_peripherals` straight at this macro and there is no `/proc`
  device-tree to detect the SoC at runtime, so the BCM2837 (Pi Zero 2 W) build
  needs the base set at compile time — matching the gating already present in
  `rpi-base.h` and `lib2835/bcm2835.h`. Required for the `kernel7.img` build.
- `baremetal/bareMetalMain.c` — set the boot-banner mini-UART baud from the
  actual (config-pinned) core clock instead of the upstream 400 MHz assumption,
  and bring the mini-UART up **unconditionally** (upstream only did so when the
  ARM clock wasn't already at target, which on a `force_turbo` Pi Zero 2 W meant
  the UART was never initialised and the board emitted nothing). See
  [`docs/BOOT.md`](../../docs/BOOT.md).
- `baremetal/baremetalEntry.S` — ARMv7-only (`__arm__`/ARMv7) reset additions for
  the Pi Zero 2 W: drop from HYP mode to SVC via `eret` (the firmware launches the
  kernel in HYP and `msr CPSR_c` can't leave it), park the secondary cores, and
  enable the cycle counter via the ARMv7 PMU. ARMv6 (Pi Zero) is unchanged.
- `vectrex/baremetalUtil.h` — use the ARMv7 PMU (`c9`) for the cycle counter on
  Cortex-A; the ARM11 (`c15`) registers it used are undefined there and trapped
  as an undefined instruction. Both: see [`docs/BOOT.md`](../../docs/BOOT.md).
- `baremetal/rpi-interrupts.{c,h}` — add an optional `RPI_aux_irq_handler` hook so
  the mini-UART RX interrupt can be serviced (used opt-in by `src/uart_rx.c`); the
  vendored vector otherwise assumes only the ARM timer. Null/unused by default.
- `pitrex/pitrexio-gpio.c` — print whether `#HALT` is asserted at `vectrexinit`
  (i.e. whether a Vectrex is on the cartridge bus), a useful one-line boot
  diagnostic.
- `vectrex/vectrexInterface.h` — guard the two `WAIT_CYCLE_NANO` cycle-delay
  busy-loops (ARM inline asm) on `__arm__`, with a no-op fallback off-target.
  This lets the **off-target emulator** ([`tools/emu`](../../tools/emu)) compile
  the real drawing code on a host (x86) to render what the Vectrex would show
  (see [`docs/EMULATOR.md`](../../docs/EMULATOR.md)). The delays are pure timing;
  the ARM build is byte-for-byte unaffected.

Toolchain-compatibility *flags* (not source edits) live in the top-level
`Makefile`: `-fcommon` (GCC ≥ 10 defaults to `-fno-common`) and
`--specs=nosys.specs` (newlib syscall stubs).
