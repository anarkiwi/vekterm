/*
 * font.c — Vectrex-native vector font + renderer for vekterm.
 *
 * Glyph data: ported from the libpitrex "ABC_WIDE" vector font
 * (third_party/libpitrex/vectrex/vectorFont.i) — the same lineage of blocky,
 * straight-stroke fonts used by Vectrex titles (e.g. Malban's Vectorblade,
 * github.com/malbanGit/Vectorblade, whose vectorString.asm draws an "Alphabet"
 * table of short straight strokes). A vector display has no pixels: every glyph
 * is a handful of straight beam strokes, so a font for it must BE straight
 * strokes. The earlier Hershey Simplex font traced each curve (O, C, S, a, e…)
 * as a dozen tiny chords; on the CRT those chords read as wobble and the small
 * build-id line bowed and smeared. This font has no curves — letters are 4..13
 * points of straight segments on a 4-wide x 8-tall grid, baseline at y=0, +y up.
 *
 * The original .i data is relative (pattern, dy, dx) deltas blown up by a
 * constant; tools/.../conv.py flattens it to absolute (x, y) points with a
 * (-1, -1) pen-up sentinel — the format this renderer already consumed. Only
 * uppercase + digits + a few symbols exist (lowercase folds to uppercase at draw
 * time, which suits the all-caps splash and its hex build id). ':' and '-' are
 * hand-added in the same grid; see conv.py.
 *
 * Renderer: vt_draw_string scales each point into the Vectrex integrator range
 * and draws every stroke with v_directDraw32 — the same per-segment path the
 * received frames use, where each segment gets its own SET_OPTIMAL_SCALE so a
 * stroke is timed to its own length and stays straight; see font.h.
 */
#include "font.h"

/* v_directDraw32 lives in the vendored libpitrex (vectrexInterface.c). Declared
 * locally so this file needs neither the VIA register macros (baremetal) nor the
 * full interface header (the off-target emulator declares it the same way). */
void v_directDraw32(int32_t xStart, int32_t yStart, int32_t xEnd, int32_t yEnd, uint8_t brightness);

/* Integrator units per font unit, per `size` step. The glyph grid is 8 units
 * tall; 21 puts a size-8 capital at ~8*8*21 ~= 1344 units tall, matching the old
 * splash title height so existing VT_SPLASH_*_SIZE values carry over unchanged.
 * Override with -DVT_FONT_SCALE to recalibrate without editing source. */
#ifndef VT_FONT_SCALE
#define VT_FONT_SCALE 21
#endif

/* 8-bit screen coord -> integrator input coord (v_printString used the same
 * x*128 mapping, so splash placements carry over unchanged). */
#define VT_FONT_POS 128

#include "font_data.i"

#define VT_FONT_FIRST 0x20
#define VT_FONT_LAST 0x7E

void vt_draw_string(int8_t x, int8_t y, const char *s, uint8_t size, uint8_t brightness)
{
    int32_t penx = (int32_t)x * VT_FONT_POS;
    int32_t baseY = (int32_t)y * VT_FONT_POS;
    int32_t mul = (int32_t)size * VT_FONT_SCALE;

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        const vfont_glyph_t *g;
        int32_t prevx = 0, prevy = 0;
        int pen_up = 1;
        int i;

        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 0x20); /* no lowercase glyphs -> fold to caps */
        if (c < VT_FONT_FIRST || c > VT_FONT_LAST)
            c = VT_FONT_FIRST; /* unknown -> blank space */
        g = &vfont[c - VT_FONT_FIRST];

        for (i = 0; i < g->count; i++) {
            int hx = g->points[2 * i];
            int hy = g->points[2 * i + 1];
            int32_t sx, sy;

            if (hx == -1 && hy == -1) {
                pen_up = 1; /* lift pen, next point is a move */
                continue;
            }
            sx = penx + (int32_t)hx * mul;
            sy = baseY + (int32_t)hy * mul;
            if (!pen_up)
                v_directDraw32(prevx, prevy, sx, sy, brightness);
            pen_up = 0;
            prevx = sx;
            prevy = sy;
        }
        penx += (int32_t)g->width * mul;
    }
}
