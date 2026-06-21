/*
 * font.h — Hershey Simplex vector text for vekterm.
 *
 * Replaces the libpitrex BIOS stroke font (v_printString) used for the splash.
 * The BIOS font is uppercase-only on a coarse ~15-unit grid, and its glyph
 * renderer (v_Draw_VLp) draws every stroke of a glyph at one shared integrator
 * scale — short strokes finish before the integrators settle, so straight
 * segments come out visibly bent.
 *
 * vt_draw_string emits each stroke through v_directDraw32, the same per-segment
 * draw path the received frames already use: every segment gets its own optimal
 * integrator scale (SET_OPTIMAL_SCALE), so strokes are timed to their own length
 * and stay straight. The Hershey Simplex font (Dr. A. V. Hershey, NBS, public
 * domain) is the de-facto standard for stroke/vector displays and carries the
 * full printable ASCII range including lowercase. See src/font.c.
 */
#ifndef VEKTERM_FONT_H
#define VEKTERM_FONT_H

#include <stdint.h>

/* Draw a NUL-terminated ASCII string as Hershey Simplex vector text.
 *
 *   x, y       baseline-left of the first glyph, in 8-bit Vectrex screen coords
 *              (-128..127, +y up) — the same placement convention v_printString
 *              took, so existing splash coordinates carry over unchanged.
 *   size       nominal glyph size; ~1 cap-height unit per `size` step. Matches
 *              the old textSize scale closely (title 8, body 4, hints 3).
 *   brightness Vectrex z-axis intensity, 0..127 (0 draws nothing).
 *
 * Characters outside printable ASCII (0x20..0x7E) render as a blank space.
 */
void vt_draw_string(int8_t x, int8_t y, const char *s, uint8_t size, uint8_t brightness);

#endif /* VEKTERM_FONT_H */
