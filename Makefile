# vekterm — USB-DVG / vecterm receiver for the PiTrex.
#
# Host (no hardware) — for development, tests, and inspecting a sender:
#   make            build ./vekterm (stub backend: decodes + reports)
#   make test       build and run the unit tests
#   make check      alias for `make test`
#   make format     reformat sources with clang-format
#   make docker     build everything in Docker and export kernel.img + vekterm.img
#
# PiTrex (the deployable artifacts) — needs arm-none-eabi-gcc + newlib:
#   make baremetal  build ./kernel.img (boots straight into vekterm, no Linux)
#   make image      build ./vekterm.img (a flashable SD card image)
#
# libpitrex is vendored in third_party/libpitrex (no external checkout), so the
# baremetal build is self-contained. `make docker` runs it for you. See
# docs/DEPLOY.md.

# ---- host toolchain ------------------------------------------------------
CC      ?= cc
CSTD    ?= -std=c99
WARN    ?= -Wall -Wextra -Wpedantic -Werror
OPT     ?= -O2
CFLAGS  ?= $(OPT) $(CSTD) $(WARN) -Isrc
LDFLAGS ?=

# Optional AddressSanitizer + UBSanitizer for the host build and tests.  Run the
# whole suite under them with `make test SAN=1` (CI does this); they catch the
# out-of-bounds and overflow bugs the parser is being hardened against.
SAN ?= 0
ifeq ($(SAN),1)
CFLAGS  += -g -fsanitize=address,undefined -fno-sanitize-recover=all
LDFLAGS += -fsanitize=address,undefined
endif

# Pure, portable core shared by the host tool, the tests, and the baremetal app.
CORE     := src/protocol.c src/frame.c
CORE_HDR := src/protocol.h src/frame.h

# Host tool: core + serial input + stub backend + entry point.
HOST_SRC := src/vekterm.c src/serial.c src/backend_stub.c $(CORE)
HOST_HDR := src/serial.h src/backend.h $(CORE_HDR)

TESTS := tests/test_protocol tests/test_frame tests/test_coord tests/test_parser

.PHONY: all test check clean format format-check docker baremetal baremetal7 kernels image help emu emu-test emu-e2e

all: vekterm

# ---- off-target emulator (the VIA/Vectrex model + vekterm's real draw path) --
# Runs the vendored libpitrex drawing code against a software 6522 VIA + Vectrex
# vector generator, reconstructing what the CRT would show — no ARM toolchain, no
# QEMU, no Vectrex. `make emu` builds it; `make emu-test` renders + checks the
# splash and a known square. See docs/EMULATOR.md.
emu:
	tools/emu/build.sh

emu-e2e:
	tools/emu/e2e.sh

emu-test:
	tools/emu/selftest.sh

vekterm: $(HOST_SRC) $(HOST_HDR)
	$(CC) $(CFLAGS) -o $@ $(HOST_SRC) $(LDFLAGS)

check: test

# Build and run every test, failing if any test process exits non-zero.
test: $(TESTS)
	@fail=0; for t in $(TESTS); do \
		echo "== $$t =="; ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

# Each test is a standalone executable linking only the pure core.
tests/test_%: tests/test_%.c $(CORE) $(CORE_HDR) tests/vt_test.h
	$(CC) $(CFLAGS) -Itests -o $@ $< $(CORE)

FORMAT_FILES := src/*.c src/*.h tests/*.c tests/*.h

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

# Build everything in Docker and export the artifacts to ./out.
docker:
	docker build --target artifacts --output type=local,dest=out .
	@echo "exported out/kernel.img, out/kernel7.img and out/vekterm.img"

# ---- baremetal build (the deployable PiTrex kernel images) ---------------
# Self-contained: compiles vekterm + the vendored libpitrex sources with
# arm-none-eabi-gcc and links the vendored pitrex archives + linker script,
# then objcopy to a raw kernel image. No external checkout, no build-time
# patching (fixes are baked into third_party/libpitrex; see its NOTICE.md).
#
# Two kernels are built, one per Pi family, because the peripheral base address
# (and CPU arch) differ and are baked in at compile time:
#   kernel.img  -- Pi Zero / Zero W  (BCM2835, ARMv6, peripherals @ 0x20000000)
#   kernel7.img -- Pi Zero 2 W / 2/3 (BCM2837, ARMv7, peripherals @ 0x3F000000)
# The Pi firmware auto-selects by SoC (see deploy/config.txt: [pi0]/[pi02]).
# Each variant links the matching prebuilt HAL archives (lib/ vs lib7/ upstream;
# vendored here as baremetal/ and baremetal/lib7/).
VENDOR         := third_party/libpitrex
BM_CC          ?= arm-none-eabi-gcc
BM_OBJCOPY     ?= arm-none-eabi-objcopy

BM_PITREX_DIR  := $(VENDOR)/pitrex
BM_VECTREX_DIR := $(VENDOR)/vectrex
BM_LIB_DIR     := $(VENDOR)/baremetal

# EXTRA_CFLAGS lets you calibrate without editing source, e.g.:
#   make baremetal EXTRA_CFLAGS='-DVT_VECTREX_MAX=12000 -DVT_UART_BAUD=115200'
EXTRA_CFLAGS ?=

# Newlib's default syscall stubs (_kill, _getpid, ...) for symbols cstubs.c
# doesn't provide; the driver inserts libnosys after libc (link order matters).
BM_SYSLIBS := --specs=nosys.specs

# Prebuilt pitrex archives (FatFs, bcm2835, the baremetal HAL).
BM_LIBS := -lm -lff12c -ldebug -lhal -lutils -lconsole -lff12c -lbob -li2c -lbcm2835 -larm

# Sources to compile, grouped by directory.
BM_PITREX_C  := bcm2835 pitrexio-gpio
BM_VECTREX_C := vectrexInterface osWrapper baremetalUtil
# The baremetal runtime: C files compiled as C (only baremetalEntry.S is asm).
BM_LOADER_C  := bareMetalMain cstubs rpi-armtimer rpi-aux rpi-gpio rpi-interrupts rpi-systimer
BM_APP       := protocol frame uart_rx vekterm_baremetal

baremetal:  kernel.img
baremetal7: kernel7.img
kernels:    kernel.img kernel7.img

# bm_variant: instantiate a full per-Pi build.  Args:
#   $(1) variant tag / object dir   $(2) -march/-mtune/-mfpu flags
#   $(3) -D model (sets peripheral base via rpi-base.h / bcm2835.h gating)
#   $(4) archive -L dir (lib/ vs lib7/)   $(5) output image name
# -fcommon: the pitrex sources predate GCC 10 and rely on tentative-definition
# merging (shared globals like errno/settings); GCC now defaults to -fno-common.
# SETTINGS_DIR is where vectrexInterface reads/writes settings on the SD root.
define bm_variant
BMB_$(1) := build.baremetal/$(1)/
BM_CFLAGS_$(1) := -Ofast -Isrc -fcommon \
	-I$$(VENDOR) -I$$(BM_LIB_DIR)/lib2835 -I$$(BM_LIB_DIR) \
	-mfloat-abi=hard -nostartfiles $(2) \
	$(3) -DFREESTANDING -DPITREX_DEBUG -DMHZ1000 -DLOADER_START=0x4000000 \
	-DSETTINGS_DIR='"settings"' \
	$$(EXTRA_CFLAGS)
BM_OBJS_$(1) := $$(addprefix $$(BMB_$(1)), baremetalEntry.o \
	$$(addsuffix .o, $$(BM_LOADER_C) $$(BM_PITREX_C) $$(BM_VECTREX_C) $$(BM_APP)))

$$(BMB_$(1)):
	mkdir -p $$@

$$(BMB_$(1))%.o: $$(BM_LIB_DIR)/%.c | $$(BMB_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -c $$< -o $$@
$$(BMB_$(1))%.o: $$(BM_PITREX_DIR)/%.c | $$(BMB_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -c $$< -o $$@
$$(BMB_$(1))%.o: $$(BM_VECTREX_DIR)/%.c | $$(BMB_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -c $$< -o $$@
$$(BMB_$(1))%.o: src/%.c $$(CORE_HDR) | $$(BMB_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -c $$< -o $$@
$$(BMB_$(1))baremetalEntry.o: $$(BM_LIB_DIR)/baremetalEntry.S | $$(BMB_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -D__ASSEMBLY__ -c $$< -o $$@

$(5): $$(BM_OBJS_$(1))
	$$(BM_CC) $$(BM_CFLAGS_$(1)) -o $$(BMB_$(1))vekterm.elf \
		$$(BM_OBJS_$(1)) \
		-L$(4) $$(BM_LIBS) $$(BM_SYSLIBS) $$(BM_LIB_DIR)/linkerHeapDefBoot.ld
	$$(BM_OBJCOPY) $$(BMB_$(1))vekterm.elf -O binary $(5)
	@echo "built $(5) ($$$$(wc -c < $(5)) bytes)"
endef

# Pi Zero / Zero W: ARMv6, BCM2835 base 0x20000000 (RPI0 -> rpi-base.h default).
$(eval $(call bm_variant,v6,-mfpu=vfp -march=armv6zk -mtune=arm1176jzf-s,-DRPI0,$(BM_LIB_DIR),kernel.img))
# Pi Zero 2 W / 2 / 3: ARMv7, BCM2837 base 0x3F000000 (RPI2 flips the base in
# rpi-base.h, lib2835/bcm2835.h and pitrex/bcm2835.h). Mirrors upstream lib7.
$(eval $(call bm_variant,v7,-mfpu=neon-vfpv4 -march=armv7-a -mtune=cortex-a7,-DRPI2,$(BM_LIB_DIR)/lib7,kernel7.img))

# ---- SD card image -------------------------------------------------------
# One flashable image carries both kernels; the Pi firmware picks per SoC.
image: vekterm.img

vekterm.img: kernel.img kernel7.img deploy/build-image.sh deploy/config.txt
	deploy/build-image.sh kernel.img kernel7.img vekterm.img

clean:
	rm -f vekterm $(TESTS)
	rm -f kernel.img kernel7.img vekterm.img
	rm -rf build.baremetal
	rm -rf out-emu
	rm -rf vekterm.dSYM tests/*.dSYM

help:
	@grep -E '^#   make' Makefile | sed 's/^#   /  /'
