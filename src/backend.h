/*
 * Output backend interface.
 *
 * A backend turns decoded frames into light.  The server keeps one "active"
 * frame and asks the backend to refresh it ~50 times a second (a vector display
 * has no persistence, so each frame is re-drawn until the next one arrives).
 *
 *   - vt_backend_stub   records/prints frames; needs no hardware (host, dry-run)
 *   - vt_backend_pitrex drives the Vectrex via libpitrex (built with HAVE_PITREX)
 *
 * This mirrors pyvterm's Transport split (MemoryTransport vs SerialTransport):
 * the decode pipeline never needs to know which backend is in use.
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

/* Always available — host builds, dry runs, and `--dry-run` on the Pi. */
extern const vt_backend vt_backend_stub;

#ifdef HAVE_PITREX
/* Real hardware; only linked when built against libpitrex (`make pitrex`). */
extern const vt_backend vt_backend_pitrex;
#endif

#endif /* VEKTERM_BACKEND_H */
