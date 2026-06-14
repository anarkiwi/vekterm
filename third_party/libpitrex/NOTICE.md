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

## Local modifications

Two small, guarded fixes are baked in so the sources compile with a current
`arm-none-eabi-gcc` + newlib (search for `vekterm-vendor:`):

- `vectrex/vectrexInterface.c` — fallback `#define MAP_FAILED` (upstream defines
  it only in its non-baremetal branch, but common code references it).
- `baremetal/vectors.h` — add `#include <stdint.h>` and a `u_long` typedef
  (used by the header but not included upstream).

Toolchain-compatibility *flags* (not source edits) live in the top-level
`Makefile`: `-fcommon` (GCC ≥ 10 defaults to `-fno-common`) and
`--specs=nosys.specs` (newlib syscall stubs).
