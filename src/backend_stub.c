/*
 * Stub backend: no hardware.  It reports each frame it is handed, so the same
 * decode pipeline can be exercised on a host, in CI, and via `--dry-run` on the
 * Pi.  It is the receiver-side analogue of pyvterm's MemoryTransport.
 */
#include "backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    vt_options opt;
    vt_frame active;
    bool have_frame;
    unsigned long frames;
} stub_ctx;

static void *stub_init(const vt_options *opt)
{
    stub_ctx *c = (stub_ctx *)calloc(1, sizeof *c);
    if (c != NULL) {
        c->opt = *opt;
    }
    return c;
}

static void stub_set_frame(void *ctx, const vt_frame *frame)
{
    stub_ctx *c = (stub_ctx *)ctx;
    c->active = *frame;
    c->have_frame = true;
    c->frames++;
    printf("frame %lu: %d vector(s), beam_travel=%u%s\n", c->frames, frame->count,
           frame->total_length, frame->overflow ? " [OVERFLOW]" : "");

    if (c->opt.verbose) {
        int i;
        for (i = 0; i < frame->count; i++) {
            const vt_segment *s = &frame->segments[i];
            printf("    (%4u,%4u)->(%4u,%4u) z=%u\n", s->x0, s->y0, s->x1, s->y1, s->brightness);
        }
    }
}

static void stub_refresh(void *ctx)
{
    (void)ctx; /* No display to keep alive. */
}

static void stub_shutdown(void *ctx)
{
    free(ctx);
}

const vt_backend vt_backend_stub = {
    "stub", stub_init, stub_set_frame, stub_refresh, stub_shutdown,
};
