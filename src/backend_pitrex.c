/*
 * PiTrex backend: draws frames on a real Vectrex through libpitrex.
 *
 * Built only when HAVE_PITREX is defined (`make pitrex`).  It is the device-side
 * mirror of the primitives pyvterm's docs reference: v_WaitRecal paces each
 * 50 Hz refresh and zeroes the beam, and v_directDraw32 draws one absolute
 * vector at a given intensity.  A vector display has no persistence, so the
 * server calls refresh() every loop to re-draw the active frame until a new one
 * arrives.
 */
#include "backend.h"

#include <stdint.h>
#include <stdlib.h>

#include <vectrexInterface.h>

typedef struct {
    vt_options opt;
    vt_frame active;
    bool have_frame;
} pitrex_ctx;

static void *pitrex_init(const vt_options *opt)
{
    pitrex_ctx *c = (pitrex_ctx *)calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    c->opt = *opt;

    v_init();
    if (opt->scale > 0) {
        v_setScale((uint16_t)opt->scale);
    }
    return c;
}

static void pitrex_set_frame(void *ctx, const vt_frame *frame)
{
    pitrex_ctx *c = (pitrex_ctx *)ctx;
    c->active = *frame;
    c->have_frame = true;
}

static void pitrex_refresh(void *ctx)
{
    pitrex_ctx *c = (pitrex_ctx *)ctx;
    const vt_options *opt = &c->opt;
    int i;

    /* Wait for the start of a refresh; this also recalibrates and re-zeroes
     * the beam at screen center. */
    v_WaitRecal();
    if (!c->have_frame) {
        return;
    }

    for (i = 0; i < c->active.count; i++) {
        const vt_segment *s = &c->active.segments[i];
        int32_t x0 = vt_map_coord(s->x0, opt->vectrex_min, opt->vectrex_max);
        int32_t y0 = vt_map_coord(s->y0, opt->vectrex_min, opt->vectrex_max);
        int32_t x1 = vt_map_coord(s->x1, opt->vectrex_min, opt->vectrex_max);
        int32_t y1 = vt_map_coord(s->y1, opt->vectrex_min, opt->vectrex_max);
        uint8_t z = (uint8_t)(s->brightness >> opt->bright_shift);
        v_directDraw32(x0, y0, x1, y1, z);
    }
}

static void pitrex_shutdown(void *ctx)
{
    free(ctx);
}

const vt_backend vt_backend_pitrex = {
    "pitrex", pitrex_init, pitrex_set_frame, pitrex_refresh, pitrex_shutdown,
};
