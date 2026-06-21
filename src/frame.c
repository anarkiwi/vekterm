#include "frame.h"

#include <string.h>

/* The XY field is 14 bits (0..16383), but the device grid is only 0..4095.
 * Clamp a decoded coordinate into the valid device range so a buggy or hostile
 * sender can never push an out-of-range point into a frame buffer; downstream
 * consumers then never have to range-check the stored vectors. */
static uint16_t clamp_dev_coord(uint16_t v)
{
    return v > VT_DVG_RES_MAX ? (uint16_t)VT_DVG_RES_MAX : v;
}

/* Clamp a (possibly negative or oversized) computed coordinate to 0..4095.  EXT
 * subtypes synthesise coordinates from signed steps and displacements, so unlike
 * a 14-bit XY field they can fall below 0 too. */
static uint16_t clamp_dev_int(int v)
{
    if (v < VT_DVG_RES_MIN) {
        return (uint16_t)VT_DVG_RES_MIN;
    }
    if (v > VT_DVG_RES_MAX) {
        return (uint16_t)VT_DVG_RES_MAX;
    }
    return (uint16_t)v;
}

/* Big-endian field readers for byte-packed EXT payloads. */
static uint16_t rd_u16(const uint8_t *b, uint32_t off)
{
    return (uint16_t)(((uint16_t)b[off] << 8) | b[off + 1]);
}

static int rd_i16(const uint8_t *b, uint32_t off)
{
    return (int)(int16_t)rd_u16(b, off);
}

void vt_parser_reset_frame(vt_parser *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->frame.count = 0;
    parser->frame.total_length = 0;
    parser->frame.monochrome = false;
    parser->frame.overflow = false;
    parser->brightness = 0;
    parser->have_pos = false;
    parser->cur_x = VT_DEV_CENTER;
    parser->cur_y = VT_DEV_CENTER;
}

void vt_parser_init(vt_parser *parser, vt_sink sink)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof *parser);
    parser->sink = sink;
    vt_parser_reset_frame(parser);
}

void vt_parser_resync(vt_parser *parser)
{
    /* Drop any partial word so the next byte starts a fresh 4-byte word. Used at
     * a known frame boundary (the flow-control handshake) to recover word
     * alignment if the stream ever slipped a byte.  Any half-gathered EXT
     * payload is dropped too, for the same reason. */
    if (parser != NULL) {
        parser->nbytes = 0;
        parser->ext_remaining = 0;
        parser->ext_len = 0;
        parser->ext_overflow = false;
    }
}

/* Append one lit vector (x0,y0)->(x1,y1) at intensity z, advancing the beam.
 * The single segment-producing primitive: plain XY draws and every EXT subtype
 * funnel through it, so the pipeline cap and beam tracking live in one place. */
static void add_segment(vt_parser *parser, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                        uint8_t z)
{
    if (parser->frame.count < VT_MAX_PIPELINE) {
        vt_segment *seg = &parser->frame.segments[parser->frame.count++];
        seg->x0 = x0;
        seg->y0 = y0;
        seg->x1 = x1;
        seg->y1 = y1;
        seg->brightness = z;
    } else {
        parser->frame.overflow = true;
    }
    parser->cur_x = x1;
    parser->cur_y = y1;
    parser->have_pos = true;
}

static void emit_lit(vt_parser *parser, uint16_t x, uint16_t y)
{
    uint16_t sx = (uint16_t)(parser->have_pos ? parser->cur_x : VT_DEV_CENTER);
    uint16_t sy = (uint16_t)(parser->have_pos ? parser->cur_y : VT_DEV_CENTER);
    add_segment(parser, sx, sy, x, y, parser->brightness);
}

/* --- EXT subtype expanders (see docs/PROTOCOL-EXTENSIONS.md) ------------- */

/* HEIGHTFIELD: a function over a regular grid; X implicit, Y = baseline +
 * displacement.  One scan line is one polyline; intensity 0 breaks the run. */
static void dispatch_heightfield(vt_parser *parser, const uint8_t *buf, uint32_t len)
{
    uint8_t flags;
    uint16_t cols, rows;
    int x0, x_step, y0, y_step;
    uint16_t y_scale;
    uint8_t bright;
    bool has_intensity, serpentine;
    uint32_t n, need, r;
    const uint8_t *disp;
    const uint8_t *inten;

    if (len < 16) {
        return; /* truncated header */
    }
    flags = buf[0];
    cols = rd_u16(buf, 1);
    rows = rd_u16(buf, 3);
    x0 = rd_i16(buf, 5);
    x_step = rd_i16(buf, 7);
    y0 = rd_i16(buf, 9);
    y_step = rd_i16(buf, 11);
    y_scale = rd_u16(buf, 13);
    bright = buf[15];
    has_intensity = (flags & 0x01u) != 0;
    serpentine = (flags & 0x02u) != 0;

    n = (uint32_t)cols * rows;
    need = 16u + n + (has_intensity ? n : 0u);
    if (len < need) {
        return; /* truncated payload */
    }
    disp = buf + 16;
    inten = has_intensity ? buf + 16 + n : NULL;

    for (r = 0; r < rows; r++) {
        int y_base = y0 + (int)r * y_step;
        bool reverse = serpentine && (r & 1u);
        bool have = false;
        uint16_t px = 0, py = 0;
        uint16_t k;
        for (k = 0; k < cols; k++) {
            uint16_t c = reverse ? (uint16_t)(cols - 1 - k) : k;
            uint32_t idx = (uint32_t)r * cols + c;
            uint8_t z = inten ? inten[idx] : bright;
            uint16_t x, y;
            if (z == 0) {
                have = false; /* blanked: break the scan-line run */
                continue;
            }
            x = clamp_dev_int(x0 + (int)c * x_step);
            y = clamp_dev_int(y_base + (int)(((uint32_t)disp[idx] * y_scale) >> 8));
            if (have) {
                add_segment(parser, px, py, x, y, z);
            }
            px = x;
            py = y;
            have = true;
        }
    }
}

/* POLYLINE: an absolute anchor followed by signed per-point deltas. */
static void dispatch_polyline(vt_parser *parser, const uint8_t *buf, uint32_t len)
{
    uint8_t flags, bright;
    int x0, y0;
    uint16_t count;
    bool has_intensity, closed, wide;
    uint32_t npts, stride, i;
    const uint8_t *p;
    int x, y;
    uint16_t px, py;

    if (len < 8) {
        return;
    }
    flags = buf[0];
    bright = buf[1];
    x0 = rd_u16(buf, 2);
    y0 = rd_u16(buf, 4);
    count = rd_u16(buf, 6);
    has_intensity = (flags & 0x01u) != 0;
    closed = (flags & 0x02u) != 0;
    wide = (flags & 0x04u) != 0;
    if (count == 0) {
        return;
    }
    npts = (uint32_t)count - 1;
    stride = (wide ? 4u : 2u) + (has_intensity ? 1u : 0u);
    if (len < 8u + npts * stride) {
        return; /* truncated payload */
    }

    p = buf + 8;
    x = x0;
    y = y0;
    px = clamp_dev_int(x0);
    py = clamp_dev_int(y0);
    for (i = 0; i < npts; i++) {
        int dx, dy;
        uint8_t z;
        uint16_t cx, cy;
        if (wide) {
            dx = rd_i16(p, 0);
            dy = rd_i16(p, 2);
            p += 4;
        } else {
            dx = (int)(int8_t)p[0];
            dy = (int)(int8_t)p[1];
            p += 2;
        }
        z = has_intensity ? *p++ : bright;
        x += dx;
        y += dy;
        cx = clamp_dev_int(x);
        cy = clamp_dev_int(y);
        if (z > 0) {
            add_segment(parser, px, py, cx, cy, z);
        }
        px = cx;
        py = cy;
    }
    if (closed && npts >= 1 && bright > 0) {
        add_segment(parser, px, py, clamp_dev_int(x0), clamp_dev_int(y0), bright);
    }
}

static void dispatch_ext(vt_parser *parser)
{
    if (parser->ext_overflow) {
        parser->frame.overflow = true; /* payload too large to buffer; skipped */
    } else {
        switch (parser->ext_subtype) {
        case VT_EXT_HEIGHTFIELD:
            dispatch_heightfield(parser, parser->ext_buf, parser->ext_len);
            break;
        case VT_EXT_POLYLINE:
            dispatch_polyline(parser, parser->ext_buf, parser->ext_len);
            break;
        default:
            break; /* unknown subtype: skip (forward-compatible) */
        }
    }
    parser->ext_subtype = 0;
    parser->ext_len = 0;
    parser->ext_overflow = false;
}

void vt_parser_feed_word(vt_parser *parser, uint32_t word)
{
    vt_command cmd;
    if (parser == NULL) {
        return;
    }
    cmd = vt_decode_word(word);
    parser->words_seen++;

    switch (cmd.flag) {
    case VT_FLAG_FRAME:
        /* Frame header: start a fresh frame, remembering the beam-travel hint. */
        vt_parser_reset_frame(parser);
        parser->frame.total_length = cmd.value;
        break;
    case VT_FLAG_RGB:
        parser->brightness = vt_brightness_from_rgb(cmd.r, cmd.g, cmd.b);
        break;
    case VT_FLAG_XY:
        if (cmd.blank) {
            /* Beam-off reposition: move without drawing. */
            parser->cur_x = clamp_dev_coord(cmd.x);
            parser->cur_y = clamp_dev_coord(cmd.y);
            parser->have_pos = true;
        } else {
            emit_lit(parser, clamp_dev_coord(cmd.x), clamp_dev_coord(cmd.y));
        }
        break;
    case VT_FLAG_COMPLETE:
        parser->frame.monochrome = cmd.monochrome;
        parser->frames_done++;
        if (parser->sink.on_frame != NULL) {
            parser->sink.on_frame(parser->sink.ctx, &parser->frame);
        }
        vt_parser_reset_frame(parser);
        break;
    case VT_FLAG_EXIT:
        parser->session_over = true;
        if (parser->sink.on_exit != NULL) {
            parser->sink.on_exit(parser->sink.ctx);
        }
        break;
    case VT_FLAG_EXT: {
        /* Extensions container: gather `length` payload bytes, then dispatch by
         * subtype (vt_parser_feed routes the payload bytes here). */
        uint32_t length = vt_ext_length(word);
        parser->ext_subtype = vt_ext_subtype(word);
        parser->ext_len = 0;
        parser->ext_overflow = length > VT_EXT_MAX;
        parser->ext_remaining = length;
        if (length == 0) {
            dispatch_ext(parser); /* empty payload: nothing to gather */
        }
        break;
    }
    case VT_FLAG_CMD:
        /* Device command channel.  HELLO is answered with the capability
         * descriptor (on_query); keepalive is a null ping that resets the
         * receiver's idle timeout without changing the drawn frame. */
        if (vt_is_hello(word) && parser->sink.on_query != NULL) {
            parser->sink.on_query(parser->sink.ctx);
        } else if (vt_is_keepalive(word) && parser->sink.on_keepalive != NULL) {
            parser->sink.on_keepalive(parser->sink.ctx);
        }
        break;
    case VT_FLAG_QUALITY: /* Render hint: no effect on the decoded geometry. */
    default:
        break;
    }
}

void vt_parser_feed(vt_parser *parser, const uint8_t *data, size_t len)
{
    size_t i;
    if (parser == NULL || data == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        /* Inside an EXT container: route raw payload bytes into ext_buf until the
         * declared length is consumed, then dispatch.  An over-large payload is
         * still drained (ext_overflow set) so word alignment is preserved. */
        if (parser->ext_remaining > 0) {
            if (!parser->ext_overflow && parser->ext_len < VT_EXT_MAX) {
                parser->ext_buf[parser->ext_len++] = data[i];
            }
            parser->ext_remaining--;
            if (parser->ext_remaining == 0) {
                dispatch_ext(parser);
            }
            continue;
        }
        /* Invariant: 0 <= nbytes < VT_WORD_BYTES on entry, so neither the store
         * below nor the dispatch can run past the reassembly buffer.  The bound
         * is reasserted defensively in case the parser state was corrupted. */
        if (parser->nbytes < 0 || parser->nbytes >= VT_WORD_BYTES) {
            parser->nbytes = 0;
        }
        parser->bytes[parser->nbytes++] = data[i];
        if (parser->nbytes == VT_WORD_BYTES) {
            uint32_t word = vt_unpack_be(parser->bytes);
            parser->nbytes = 0;
            vt_parser_feed_word(parser, word);
        }
    }
}
