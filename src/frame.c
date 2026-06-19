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
     * alignment if the stream ever slipped a byte. */
    if (parser != NULL) {
        parser->nbytes = 0;
    }
}

static void emit_lit(vt_parser *parser, uint16_t x, uint16_t y)
{
    int sx = parser->have_pos ? parser->cur_x : VT_DEV_CENTER;
    int sy = parser->have_pos ? parser->cur_y : VT_DEV_CENTER;

    if (parser->frame.count < VT_MAX_PIPELINE) {
        vt_segment *seg = &parser->frame.segments[parser->frame.count++];
        seg->x0 = (uint16_t)sx;
        seg->y0 = (uint16_t)sy;
        seg->x1 = x;
        seg->y1 = y;
        seg->brightness = parser->brightness;
    } else {
        parser->frame.overflow = true;
    }

    parser->cur_x = x;
    parser->cur_y = y;
    parser->have_pos = true;
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
    case VT_FLAG_QUALITY: /* Render hint: no effect on the decoded geometry. */
    case VT_FLAG_CMD:     /* Device command channel: not used by the pitrex variant. */
    case VT_FLAG_EXT:     /* Reserved extensions container: skip unknown words. */
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
