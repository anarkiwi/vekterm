/*
 * font.h — Vectrex-native vector text for vekterm's splash.
 *
 * A vector display draws straight beam strokes, not pixels, so a legible font
 * for it must be built from straight strokes. vt_draw_string uses a blocky
 * stroke font ported from the libpitrex "ABC_WIDE" set (the same style as the
 * Vectrex BIOS / Vectorblade title fonts) — every glyph is a handful of straight
 * segments, no curve-approximating chords. An earlier Hershey Simplex font
 * traced curves (O, S, a, e…) as many tiny chords, which the CRT renders as
 * wobble; small lines bowed and smeared. See src/font.c and src/font_data.i.
 *
 * The whole string is drawn at ONE fixed integrator scale with fixed-size timing
 * forced on every stroke — the way libpitrex's own v_printString draws text —
 * rather than letting each short stroke pick its own scale, which makes the
 * timing jump between a glyph's connected strokes and kink them into curves. The
 * font is uppercase + digits + a few symbols; lowercase folds to uppercase at
 * draw time (the splash is all-caps, including its hex build id).
 */
#ifndef VEKTERM_FONT_H
#define VEKTERM_FONT_H

#include <stdint.h>

/* Draw a NUL-terminated ASCII string as Vectrex-native vector text.
 *
 *   x, y       baseline-left of the first glyph, in 8-bit Vectrex screen coords
 *              (-128..127, +y up) — the same placement convention v_printString
 *              took, so existing splash coordinates carry over unchanged.
 *   size       nominal glyph size; cap height is ~8 grid units * size * the
 *              internal VT_FONT_SCALE. Tuned so the old textSize scale carries
 *              over (title 8, body 4, hints 3) at the same on-screen height.
 *   brightness Vectrex z-axis intensity, 0..127 (0 draws nothing).
 *
 * Lowercase folds to uppercase; characters with no glyph (outside 0x20..0x7E,
 * or printable but undefined) render as a blank space.
 */
void vt_draw_string(int8_t x, int8_t y, const char *s, uint8_t size, uint8_t brightness);

#endif /* VEKTERM_FONT_H */
