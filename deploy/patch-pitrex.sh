#!/usr/bin/env bash
#
# Apply small fixes to a gtoal/pitrex checkout so its baremetal library builds
# with a stock arm-none-eabi-gcc + newlib. Idempotent and safe to re-run; only
# touches what is needed to compile. Used by the Dockerfile and `make baremetal`.
#
#   deploy/patch-pitrex.sh <pitrex-dir>
set -euo pipefail

PITREX_DIR="${1:?usage: patch-pitrex.sh <pitrex-dir>}"

# Prepend lines to a file once, keyed by a marker so re-runs are no-ops.
prepend_once() {
	file="$1"
	marker="$2"
	shift 2
	[ -f "$file" ] || {
		echo "not a pitrex checkout (missing $file)" >&2
		exit 1
	}
	if grep -q "$marker" "$file"; then
		echo "already patched: $file"
		return
	fi
	tmp="$(mktemp)"
	{
		printf '%s\n' "$@"
		cat "$file"
	} >"$tmp"
	mv "$tmp" "$file"
	echo "patched: $file"
}

# 1) vectrexInterface.c defines MAP_FAILED only inside its Linux branch, but
#    common code references it unconditionally -> "MAP_FAILED undeclared".
prepend_once "$PITREX_DIR/pitrex/vectrex/vectrexInterface.c" 'vekterm-patch: MAP_FAILED' \
	'/* vekterm-patch: MAP_FAILED (upstream defines it only for Linux) */' \
	'#ifndef MAP_FAILED' \
	'#define MAP_FAILED ((void *) -1)' \
	'#endif'

# 2) vectors.h uses uint32_t and the BSD type u_long without including the
#    headers that declare them, so the baremetal entry fails to compile.
prepend_once "$PITREX_DIR/pitrex/baremetal/vectors.h" 'vekterm-patch: types' \
	'/* vekterm-patch: types used by this header but not included upstream */' \
	'#include <stdint.h>' \
	'typedef unsigned long u_long;'
