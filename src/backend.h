/*
 * Output backend interface (host build).
 *
 * A backend consumes decoded frames. The host build provides one — the stub,
 * which reports frames without hardware — so the decode pipeline can run on a
 * PC and in CI. The Vectrex-driving receiver is the separate baremetal build.
 */
#ifndef VEKTERM_BACKEND_H
#define VEKTERM_BACKEND_H

#include "frame.h"

/* Tunables passed to a backend at init (see src/vekterm.c for defaults/CLI). */
typedef struct {
    int vectrex_min;  /* device 0     maps to this Vectrex coordinate */
    int vectrex_max;  /* device 4095  maps to this Vectrex coordinate */
    int scale;        /* v_setScale value (<=0 leaves the library default) */
    int bright_shift; /* beam intensity = segment brightness >> bright_shift */
    bool verbose;
} vt_options;

typedef struct {
    const char *name;
    void *(*init)(const vt_options *opt);                /* returns a backend context */
    void (*set_frame)(void *ctx, const vt_frame *frame); /* new active frame */
    void (*refresh)(void *ctx);                          /* (re)draw the active frame */
    void (*shutdown)(void *ctx);
} vt_backend;

/*
 * The host build ships only the stub backend: it decodes the protocol and
 * reports frames, for development, CI, and inspecting a sender's stream. The
 * deployable receiver that drives a real Vectrex is the baremetal build
 * (src/vekterm_baremetal.c, `make baremetal`), which talks to vectrexInterface
 * directly rather than through this interface.
 */
extern const vt_backend vt_backend_stub;

#endif /* VEKTERM_BACKEND_H */
