#!/usr/bin/env python3
"""Generate src/font_data.i — vekterm's Vectrex-native vector glyph table.

Ports the libpitrex "ABC_WIDE" font (third_party/libpitrex/vectrex/vectorFont.i)
into the absolute-point format src/font.c consumes. The .i data is relative
(pattern, dy, dx) deltas scaled by a constant BLOW_UP; we flatten each glyph to
absolute (x, y) points (dropping BLOW_UP — the renderer applies its own scale),
inserting a (-1, -1) pen-up sentinel before every non-drawn move. Only the glyphs
the .i defines (A-Z, 0-9, space, '.', '!', '<', '>') plus hand-added ':' and '-'
are populated; everything else is a blank advance. font.c folds lowercase to
uppercase at draw time, so no lowercase glyphs are needed.

    python3 tools/font/gen_font.py        # rewrites src/font_data.i

Re-run after editing vectorFont.i or the hand-added glyphs below.
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "third_party", "libpitrex", "vectrex", "vectorFont.i")
OUT = os.path.join(ROOT, "src", "font_data.i")

PENUP = "U"  # sentinel; emitted as (-1, -1)

HEADER = """\
/*
 * font_data.i — Vectrex-native vector glyph table for vekterm. GENERATED.
 *
 * Ported from third_party/libpitrex/vectrex/vectorFont.i ("ABC_WIDE") by
 * tools/font/gen_font.py. Each glyph is a list of absolute (x, y) points on a
 * 4-wide x 8-tall grid (baseline y=0, +y up); a (-1, -1) point lifts the pen
 * (move, don't draw) before the next point. `width` is the advance to the next
 * glyph in grid units. Indexed by (ASCII - 0x20), 0x20..0x7E; undefined glyphs
 * are blank (advance only). Regenerate, don't hand-edit; see gen_font.py.
 */
"""


def parse_glyphs(text):
    glyphs = {}
    for m in re.finditer(r"signed char ABC_(\d+)\[\]=\s*\{(.*?)\};", text, re.S):
        trips = []
        for line in m.group(2).splitlines():
            tm = re.search(
                r"\(signed char\)\s*(0x[0-9A-Fa-f]+|[+-]?\d+)\s*,"
                r"\s*([+-]?0x[0-9A-Fa-f]+)\*BLOW_UP\s*,"
                r"\s*([+-]?0x[0-9A-Fa-f]+)\*BLOW_UP",
                line,
            )
            if tm:
                pat = (int(tm.group(1), 16) if tm.group(1).lower().startswith("0x")
                       else int(tm.group(1)))
                trips.append((pat, int(tm.group(2), 16), int(tm.group(3), 16)))
        glyphs[int(m.group(1))] = trips
    return glyphs


def convert(trips):
    """Relative (pat, dy, dx) deltas -> (absolute points, advance width)."""
    cx = cy = 0
    pts = [(0, 0)]  # initial pen position (renderer loads it without drawing)
    for pat, dy, dx in trips:
        cx, cy = cx + dx, cy + dy
        if pat & 0x80:  # high bit set -> beam on -> draw
            pts.append((cx, cy))
        else:           # beam off -> pen-up move
            pts.append(PENUP)
            pts.append((cx, cy))
    width = cx  # net horizontal advance = final pen x
    while len(pts) >= 2 and pts[-2] == PENUP:  # trailing moves draw nothing
        pts = pts[:-2]
    return pts, width


def flatten(pts):
    out = []
    for p in pts:
        out += [-1, -1] if p == PENUP else [p[0], p[1]]
    return out


# ASCII code -> ABC_n index in vectorFont.i.
slot = {ord("A") + i: i for i in range(26)}
slot[ord(".")] = 26
slot[ord(" ")] = 27
slot[ord("!")] = 28
for i, ch in enumerate("123456789"):
    slot[ord(ch)] = 29 + i
slot[ord("0")] = 38
slot[ord("<")] = 39
slot[ord(">")] = 40

# Hand-authored glyphs absent from vectorFont.i, in the same baseline-0 grid.
hand = {
    ord("-"): ([(0, 0), PENUP, (0, 4), (4, 4)], 6),
    ord(":"): ([(0, 1), (1, 1), (1, 2), (0, 2), (0, 1),
                PENUP, (0, 5), (1, 5), (1, 6), (0, 6), (0, 5)], 6),
}


def main():
    glyphs = parse_glyphs(open(SRC).read())
    rows = []
    maxlen = 0
    for code in range(0x20, 0x7F):
        if code in hand:
            pts, w = hand[code]
        elif code in slot:
            pts, w = convert(glyphs[slot[code]])
        else:
            pts, w = [], 6
        f = flatten(pts)
        maxlen = max(maxlen, len(f))
        label = {" ": "space", "\\": "backslash"}.get(chr(code), chr(code))
        body = ", ".join(str(v) for v in f) if f else "0"
        rows.append((len(f) // 2, w, body, label))

    with open(OUT, "w") as out:
        out.write(HEADER)
        out.write("#define VT_GLYPH_POINTS %d\n" % maxlen)
        out.write("typedef struct {\n")
        out.write("    uint8_t count; /* number of (x, y) points (incl. pen-up sentinels) */\n")
        out.write("    uint8_t width; /* advance to next glyph, in font units */\n")
        out.write("    int8_t points[VT_GLYPH_POINTS];\n")
        out.write("} vfont_glyph_t;\n\n")
        out.write("static const vfont_glyph_t vfont[] = {\n")
        for cnt, w, body, label in rows:
            out.write("    {%d, %d, {%s}}, /* %s */\n" % (cnt, w, body, label))
        out.write("};\n")
    print("wrote %s (%d glyphs, max %d points)" % (OUT, len(rows), maxlen // 2))


if __name__ == "__main__":
    main()
