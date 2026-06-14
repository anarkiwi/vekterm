#include "frame.h"

#include <string.h>

void vt_parser_reset_frame(vt_parser *parser)
{
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
    memset(parser, 0, sizeof *parser);
    parser->sink = sink;
    vt_parser_reset_frame(parser);
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
    vt_command cmd = vt_decode_word(word);
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
            parser->cur_x = cmd.x;
            parser->cur_y = cmd.y;
            parser->have_pos = true;
        } else {
            emit_lit(parser, cmd.x, cmd.y);
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
    for (i = 0; i < len; i++) {
        parser->bytes[parser->nbytes++] = data[i];
        if (parser->nbytes == 4) {
            uint32_t word = vt_unpack_be(parser->bytes);
            parser->nbytes = 0;
            vt_parser_feed_word(parser, word);
        }
    }
}
