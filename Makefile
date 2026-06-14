# vekterm — USB-DVG / vecterm receiver for the PiTrex.
#
#   make            build ./vekterm with the stub backend (no hardware needed)
#   make test       build and run the unit tests
#   make check      tests + pitrex backend syntax check
#   make pitrex     build ./vekterm-pitrex linked against libpitrex (on the Pi)
#   make docker     build the container image (compiles + tests inside Docker)
#   make format     reformat sources with clang-format
#   make install    install the binary and (optionally) the systemd service
#
# The pure core (protocol + frame) has no dependencies and builds anywhere; only
# the `pitrex` target needs the PiTrex library headers and shared object.

CC      ?= cc
CSTD    ?= -std=c99
WARN    ?= -Wall -Wextra -Wpedantic -Werror
OPT     ?= -O2
CFLAGS  ?= $(OPT) $(CSTD) $(WARN) -Isrc
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

# Pure, portable core shared by the server and the tests.
CORE    := src/protocol.c src/frame.c
CORE_HDR := src/protocol.h src/frame.h

# Server: core + serial I/O + the stub (no-hardware) backend + entry point.
SERVER_SRC := src/vekterm.c src/serial.c src/backend_stub.c $(CORE)
SERVER_HDR := src/serial.h src/backend.h $(CORE_HDR)

# PiTrex build settings — override on the Pi if your checkout lives elsewhere:
#   make pitrex PITREX_DIR=/home/pi/pitrex
PITREX_DIR ?= /home/pi/pitrex
PITREX_INC ?= -I$(PITREX_DIR)/pitrex -I$(PITREX_DIR)/pitrex/vectrex -I$(PITREX_DIR)
PITREX_LIB ?= -L$(PITREX_DIR)/build -L$(PITREX_DIR)/lib -lpitrex

TESTS := tests/test_protocol tests/test_frame tests/test_coord

.PHONY: all pitrex pitrex-check test check clean format format-check docker install install-service help

DOCKER_IMAGE ?= vekterm

all: vekterm

vekterm: $(SERVER_SRC) $(SERVER_HDR)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) $(LDFLAGS)

pitrex: vekterm-pitrex

vekterm-pitrex: $(SERVER_SRC) src/backend_pitrex.c $(SERVER_HDR)
	$(CC) $(CFLAGS) -DHAVE_PITREX $(PITREX_INC) -o $@ \
		$(SERVER_SRC) src/backend_pitrex.c $(PITREX_LIB) $(LDFLAGS)

# Syntax-check the PiTrex backend against the fake header so CI catches drift
# without the real libpitrex or hardware (see tests/fake_pitrex/).
pitrex-check: src/backend_pitrex.c $(SERVER_HDR)
	$(CC) $(CFLAGS) -DHAVE_PITREX -Itests/fake_pitrex -fsyntax-only src/backend_pitrex.c
	@echo "backend_pitrex.c: syntax OK"

# Everything CI gates on, in one target.
check: test pitrex-check

# Build and run every test, failing the target if any test process exits non-zero.
test: $(TESTS)
	@fail=0; for t in $(TESTS); do \
		echo "== $$t =="; ./$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

# Each test is a standalone executable linking only the pure core.
tests/test_%: tests/test_%.c $(CORE) $(CORE_HDR) tests/vt_test.h
	$(CC) $(CFLAGS) -Itests -o $@ $< $(CORE)

FORMAT_FILES := src/*.c src/*.h tests/*.c tests/*.h tests/fake_pitrex/*.h

format:
	clang-format -i $(FORMAT_FILES)

format-check:
	clang-format --dry-run --Werror $(FORMAT_FILES)

# Build the container image (compiles + tests + static-links inside Docker).
docker:
	docker build -t $(DOCKER_IMAGE) .

install: vekterm
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 vekterm $(DESTDIR)$(BINDIR)/vekterm

install-service: install
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 deploy/vekterm.service $(DESTDIR)/etc/systemd/system/vekterm.service
	@echo "Now run: sudo systemctl enable --now vekterm"

clean:
	rm -f vekterm vekterm-pitrex $(TESTS)
	rm -f src/*.o tests/*.o

help:
	@grep -E '^#   make' Makefile | sed 's/^#   /  /'
