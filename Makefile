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

# Pure, portable core shared by the host tool, the tests, and the baremetal app.
CORE     := src/protocol.c src/frame.c
CORE_HDR := src/protocol.h src/frame.h

# Host tool: core + serial input + stub backend + entry point.
HOST_SRC := src/vekterm.c src/serial.c src/backend_stub.c $(CORE)
HOST_HDR := src/serial.h src/backend.h $(CORE_HDR)

TESTS := tests/test_protocol tests/test_frame tests/test_coord

.PHONY: all test check clean format format-check docker baremetal image help

all: vekterm

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
	@echo "exported out/kernel.img and out/vekterm.img"

# ---- baremetal build (the deployable PiTrex kernel image) ----------------
# Self-contained: compiles vekterm + the vendored libpitrex sources with
# arm-none-eabi-gcc and links the vendored pitrex archives + linker script,
# then objcopy to a raw kernel image. No external checkout, no build-time
# patching (fixes are baked into third_party/libpitrex; see its NOTICE.md).
VENDOR         := third_party/libpitrex
BM_CC          ?= arm-none-eabi-gcc
BM_OBJCOPY     ?= arm-none-eabi-objcopy

BM_PITREX_DIR  := $(VENDOR)/pitrex
BM_VECTREX_DIR := $(VENDOR)/vectrex
BM_LIB_DIR     := $(VENDOR)/baremetal
BMB            := build.baremetal/

# EXTRA_CFLAGS lets you calibrate without editing source, e.g.:
#   make baremetal EXTRA_CFLAGS='-DVT_VECTREX_MAX=12000 -DVT_UART_BAUD=115200'
# -fcommon: the pitrex sources predate GCC 10 and rely on tentative-definition
# merging (shared globals like errno/settings); GCC now defaults to -fno-common.
EXTRA_CFLAGS ?=
# SETTINGS_DIR is where vectrexInterface reads/writes settings on the SD root
# (build-image.sh creates this dir); upstream passes it via its makefile.
BM_CFLAGS := -Ofast -Isrc -fcommon \
	-I$(VENDOR) -I$(BM_LIB_DIR)/lib2835 -I$(BM_LIB_DIR) -L$(BM_LIB_DIR) \
	-mfloat-abi=hard -nostartfiles -mfpu=vfp -march=armv6zk -mtune=arm1176jzf-s \
	-DRPI0 -DFREESTANDING -DPITREX_DEBUG -DMHZ1000 -DLOADER_START=0x4000000 \
	-DSETTINGS_DIR='"settings"' \
	$(EXTRA_CFLAGS)

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
BM_APP       := protocol frame vekterm_baremetal

BM_PITREX_OBJS  := $(addprefix $(BMB), $(addsuffix .o, $(BM_PITREX_C)))
BM_VECTREX_OBJS := $(addprefix $(BMB), $(addsuffix .o, $(BM_VECTREX_C)))
BM_LOADER_OBJS  := $(addprefix $(BMB), $(addsuffix .o, $(BM_LOADER_C)))
BM_APP_OBJS     := $(addprefix $(BMB), $(addsuffix .o, $(BM_APP)))
BM_ALL_OBJS := $(BMB)baremetalEntry.o $(BM_LOADER_OBJS) $(BM_PITREX_OBJS) \
	$(BM_VECTREX_OBJS) $(BM_APP_OBJS)

baremetal: kernel.img

$(BMB):
	mkdir -p $(BMB)

$(BM_PITREX_OBJS): $(BMB)%.o: $(BM_PITREX_DIR)/%.c | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

$(BM_VECTREX_OBJS): $(BMB)%.o: $(BM_VECTREX_DIR)/%.c | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

$(BM_LOADER_OBJS): $(BMB)%.o: $(BM_LIB_DIR)/%.c | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

$(BM_APP_OBJS): $(BMB)%.o: src/%.c $(CORE_HDR) | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

$(BMB)baremetalEntry.o: $(BM_LIB_DIR)/baremetalEntry.S | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -D__ASSEMBLY__ -c $< -o $@

kernel.img: $(BM_ALL_OBJS)
	$(BM_CC) $(BM_CFLAGS) -o $(BMB)vekterm.elf \
		$(BM_ALL_OBJS) \
		$(BM_LIBS) $(BM_SYSLIBS) $(BM_LIB_DIR)/linkerHeapDefBoot.ld
	$(BM_OBJCOPY) $(BMB)vekterm.elf -O binary kernel.img
	@echo "built kernel.img ($$(wc -c < kernel.img) bytes)"

# ---- SD card image -------------------------------------------------------
image: vekterm.img

vekterm.img: kernel.img deploy/build-image.sh deploy/config.txt
	deploy/build-image.sh kernel.img vekterm.img

clean:
	rm -f vekterm $(TESTS)
	rm -f kernel.img vekterm.img
	rm -rf $(BMB)

help:
	@grep -E '^#   make' Makefile | sed 's/^#   /  /'
