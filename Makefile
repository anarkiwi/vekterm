# vekterm — USB-DVG / vecterm receiver for the PiTrex.
#
# Host (no hardware) — for development, tests, and inspecting a sender:
#   make            build ./vekterm (stub backend: decodes + reports)
#   make test       build and run the unit tests
#   make check      alias for `make test`
#   make format     reformat sources with clang-format
#   make docker     build everything in Docker and export kernel.img + vekterm.img
#
# PiTrex (the deployable artifacts) — needs arm-none-eabi-gcc + a pitrex checkout:
#   make baremetal  build ./kernel.img (boots straight into vekterm, no Linux)
#   make image      build ./vekterm.img (a flashable SD card image)
#
# `make docker` runs the baremetal + image build for you in a container, so you
# don't need the cross-toolchain locally. See docs/DEPLOY.md.

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

.PHONY: all test check clean format format-check docker baremetal image bm-pitrexlib help

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

# Build everything in Docker and export the artifacts to ./out (no local
# cross-toolchain needed).
docker:
	docker build --target artifacts --output type=local,dest=out .
	@echo "exported out/kernel.img and out/vekterm.img"

# ---- baremetal build (the deployable PiTrex kernel image) ----------------
# Mirrors gtoal/pitrex's hello_world/Makefile.baremetal: build the pitrex
# library objects via the pitrex tree's own makefiles, compile the baremetal
# runtime + the vekterm core + the baremetal entry point, link with the pitrex
# baremetal libs + linker script, then objcopy to a raw kernel image.
PITREX_DIR     ?= ./pitrex
BM_CC          ?= arm-none-eabi-gcc
BM_OBJCOPY     ?= arm-none-eabi-objcopy

BM_LIB_DIR     := $(PITREX_DIR)/pitrex/baremetal
BM_PITREX_DIR  := $(PITREX_DIR)/pitrex/pitrex
BM_VECTREX_DIR := $(PITREX_DIR)/pitrex/vectrex
BMB            := build.baremetal/

# -fcommon: the pitrex sources predate GCC 10 and rely on tentative-definition
# merging (shared globals like errno/settings across files); GCC now defaults to
# -fno-common, so restore the old behaviour.
# EXTRA_CFLAGS lets you calibrate without editing source, e.g.:
#   make baremetal EXTRA_CFLAGS='-DVT_VECTREX_MAX=12000 -DVT_UART_BAUD=115200'
EXTRA_CFLAGS ?=
BM_CFLAGS := -Ofast -Isrc -fcommon \
	-I$(PITREX_DIR)/pitrex -I$(BM_LIB_DIR)/lib2835 -I$(BM_LIB_DIR) -L$(BM_LIB_DIR) \
	-mfloat-abi=hard -nostartfiles -mfpu=vfp -march=armv6zk -mtune=arm1176jzf-s \
	-DRPI0 -DFREESTANDING -DPITREX_DEBUG -DMHZ1000 -DLOADER_START=0x4000000 \
	$(EXTRA_CFLAGS)

# Newlib's default syscall stubs (_kill, _getpid, ...) for symbols cstubs.c
# doesn't provide. Use the specs file so the driver inserts libnosys after libc
# (link order matters); cstubs.o still wins for _write/_sbrk.
BM_SYSLIBS := --specs=nosys.specs

# pitrex library objects, built by the pitrex tree's own baremetal makefiles.
BM_LIB_OBJS := \
	$(BM_PITREX_DIR)/$(BMB)bcm2835.o \
	$(BM_PITREX_DIR)/$(BMB)pitrexio-gpio.o \
	$(BM_VECTREX_DIR)/$(BMB)vectrexInterface.o \
	$(BM_VECTREX_DIR)/$(BMB)osWrapper.o \
	$(BM_VECTREX_DIR)/$(BMB)baremetalUtil.o

# Baremetal runtime/loader objects. We compile these ourselves: the upstream
# piTrexBoot Makefile compiles the C files with -D__ASSEMBLY__, which hides the
# libc type declarations on a current toolchain. Only the .S is real assembly.
BM_LOADER_C    := bareMetalMain cstubs rpi-armtimer rpi-aux rpi-gpio rpi-interrupts rpi-systimer
BM_LOADER_OBJS := $(addprefix $(BMB), $(addsuffix .o, $(BM_LOADER_C)))

# vekterm's own objects: the pure core + the baremetal entry point.
BM_APP      := protocol frame vekterm_baremetal
BM_APP_OBJS := $(addprefix $(BMB), $(addsuffix .o, $(BM_APP)))

BM_LIBS := -lm -lff12c -ldebug -lhal -lutils -lconsole -lff12c -lbob -li2c -lbcm2835 -larm

baremetal: kernel.img

$(BMB):
	mkdir -p $(BMB)

# Patch the pitrex checkout once (idempotent) so it builds with a current
# arm-none-eabi-gcc; the stamp makes every object depend on "patched".
$(BMB)patch.stamp: deploy/patch-pitrex.sh | $(BMB)
	@test -d $(BM_VECTREX_DIR) || { \
		echo "PITREX_DIR=$(PITREX_DIR) is not a pitrex checkout."; \
		echo "Clone https://github.com/gtoal/pitrex there, or use 'make docker'."; \
		exit 1; }
	sh deploy/patch-pitrex.sh $(PITREX_DIR)
	touch $@

# pitrex library objects via the pitrex tree's own makefiles. Inject -fcommon
# through CC since their makefiles hardcode CFLAGS.
bm-pitrexlib: $(BMB)patch.stamp
	$(MAKE) -C $(BM_PITREX_DIR) -f Makefile.baremetal CC='$(BM_CC) -fcommon' all
	$(MAKE) -C $(BM_VECTREX_DIR) -f Makefile.baremetal CC='$(BM_CC) -fcommon' all

# Loader C files: compiled as C (no -D__ASSEMBLY__).
$(BM_LOADER_OBJS): $(BMB)%.o: $(BM_LIB_DIR)/%.c $(BMB)patch.stamp | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

# The one genuine assembly source.
$(BMB)baremetalEntry.o: $(BM_LIB_DIR)/baremetalEntry.S | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -D__ASSEMBLY__ -c $< -o $@

# vekterm objects.
$(BM_APP_OBJS): $(BMB)%.o: src/%.c $(CORE_HDR) | $(BMB)
	$(BM_CC) $(BM_CFLAGS) -c $< -o $@

kernel.img: bm-pitrexlib $(BMB)baremetalEntry.o $(BM_LOADER_OBJS) $(BM_APP_OBJS)
	$(BM_CC) $(BM_CFLAGS) -o $(BMB)vekterm.elf \
		$(BMB)baremetalEntry.o $(BM_LOADER_OBJS) \
		$(BM_LIB_OBJS) \
		$(BM_APP_OBJS) \
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
