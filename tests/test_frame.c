/*
 * Parser tests — decode wire bytes back into the vectors a sender meant.
 *
 * The worked example mirrors pyvterm's tests/test_frame.py: a single white line
 * from host (0,0) to (100,0) serialises to exactly these words, and the
 * receiver must reconstruct the one vector it represents.
 */
#include <string.h>

#include "frame.h"
#include "protocol.h"
#include "vt_test.h"

/* --- recording sink ----------------------------------------------------- */

typedef struct {
    vt_frame frames[4];
    int nframes;
    int exits;
} recorder;

static recorder g_rec;

static void rec_on_frame(void *ctx, const vt_frame *frame)
{
    recorder *r = (recorder *)ctx;
    if (r->nframes < (int)(sizeof r->frames / sizeof r->frames[0])) {
        r->frames[r->nframes] = *frame;
    }
    r->nframes++;
}

static void rec_on_exit(void *ctx)
{
    ((recorder *)ctx)->exits++;
}

static vt_sink recording_sink(void)
{
    vt_sink sink;
    memset(&g_rec, 0, sizeof g_rec);
    sink.ctx = &g_rec;
    sink.on_frame = rec_on_frame;
    sink.on_exit = rec_on_exit;
    return sink;
}

static void feed_word(vt_parser *p, uint32_t word)
{
    uint8_t bytes[4];
    vt_pack_be(word, bytes);
    vt_parser_feed(p, bytes, sizeof bytes);
}

/* The exact frame pyvterm emits for one white line from host (0,0)->(100,0). */
static const uint8_t WORKED_EXAMPLE[] = {
    0x80, 0x00, 0x01, 0x90, /* FRAME, length 400         */
    0x20, 0xF0, 0xF0, 0xF0, /* RGB white (240,240,240)   */
    0x52, 0x00, 0x48, 0x02, /* XY blank move to (2049,2050) */
    0x42, 0x64, 0x48, 0x02, /* XY lit draw to (2449,2050)   */
    0x60, 0x00, 0x00, 0x05, /* QUALITY 5                 */
    0x00, 0x00, 0x00, 0x00, /* COMPLETE                  */
};

/* --- tests -------------------------------------------------------------- */

static void test_worked_example_single_vector(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    vt_parser_feed(&p, WORKED_EXAMPLE, sizeof WORKED_EXAMPLE);

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].total_length, 400);
    VT_CHECK(!g_rec.frames[0].overflow);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x0, 2049);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y0, 2050);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 2449);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y1, 2050);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].brightness, 240);
}

static void test_words_split_across_reads(void)
{
    /* A real serial link delivers arbitrary chunk boundaries; feeding one byte
     * at a time must reconstruct the identical frame. */
    vt_parser p;
    size_t i;
    vt_parser_init(&p, recording_sink());
    for (i = 0; i < sizeof WORKED_EXAMPLE; i++) {
        vt_parser_feed(&p, &WORKED_EXAMPLE[i], 1);
        /* Nothing completes until the final COMPLETE word's last byte. */
        VT_CHECK_EQ(g_rec.nframes, (i == sizeof WORKED_EXAMPLE - 1) ? 1 : 0);
    }
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 2449);
}

static void test_black_colour_blanks_draws(void)
{
    /* pyvterm sets the XY blank bit while the colour is black, so even "draws"
     * arrive blanked and must not produce a lit vector. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0, 0, 0));
    feed_word(&p, vt_encode_xy(2049, 2050, true)); /* move  */
    feed_word(&p, vt_encode_xy(2449, 2050, true)); /* "draw" but blanked */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 0);
}

static void test_connected_polyline_shares_vertices(void)
{
    /* move A, draw B, draw C with no reposition between B and C => two vectors
     * sharing vertex B, mirroring pyvterm's connected-polyline behaviour. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(100, 100, true));  /* move to A */
    feed_word(&p, vt_encode_xy(200, 100, false)); /* draw to B */
    feed_word(&p, vt_encode_xy(200, 200, false)); /* draw to C */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 2);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x0, 100);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y0, 100);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 200);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y1, 100);
    VT_CHECK_EQ(g_rec.frames[0].segments[1].x0, 200);
    VT_CHECK_EQ(g_rec.frames[0].segments[1].y0, 100);
    VT_CHECK_EQ(g_rec.frames[0].segments[1].x1, 200);
    VT_CHECK_EQ(g_rec.frames[0].segments[1].y1, 200);
}

static void test_lit_without_move_starts_from_center(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(100, 200, false)); /* lit, no prior move */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x0, VT_DEV_CENTER);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y0, VT_DEV_CENTER);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 100);
}

static void test_multiple_frames(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    vt_parser_feed(&p, WORKED_EXAMPLE, sizeof WORKED_EXAMPLE);
    vt_parser_feed(&p, WORKED_EXAMPLE, sizeof WORKED_EXAMPLE);
    VT_CHECK_EQ(g_rec.nframes, 2);
    VT_CHECK_EQ(p.frames_done, 2);
    VT_CHECK_EQ(g_rec.frames[1].count, 1);
}

static void test_exit_ends_session(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    VT_CHECK(!p.session_over);
    feed_word(&p, vt_encode_exit());
    VT_CHECK(p.session_over);
    VT_CHECK_EQ(g_rec.exits, 1);
}

static void test_unknown_words_are_ignored(void)
{
    /* A CMD word and a reserved EXT container must not disturb decoding. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, ((uint32_t)VT_FLAG_CMD << VT_FLAG_SHIFT) | 0x1234u);
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, ((uint32_t)VT_FLAG_EXT << VT_FLAG_SHIFT) | 0x5678u);
    feed_word(&p, vt_encode_xy(100, 100, true));
    feed_word(&p, vt_encode_xy(300, 300, false));
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 300);
}

static void test_overflow_is_capped_and_flagged(void)
{
    vt_parser p;
    int i;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(0, 0, true));
    for (i = 0; i < VT_MAX_PIPELINE + 5; i++) {
        feed_word(&p, vt_encode_xy((uint16_t)(i & VT_COORD_MASK), 1, false));
    }
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, VT_MAX_PIPELINE);
    VT_CHECK(g_rec.frames[0].overflow);
}

static void test_monochrome_bit_is_carried(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_complete(true));
    VT_CHECK(g_rec.frames[0].monochrome);
}

static void run_all(void)
{
    VT_RUN(test_worked_example_single_vector);
    VT_RUN(test_words_split_across_reads);
    VT_RUN(test_black_colour_blanks_draws);
    VT_RUN(test_connected_polyline_shares_vertices);
    VT_RUN(test_lit_without_move_starts_from_center);
    VT_RUN(test_multiple_frames);
    VT_RUN(test_exit_ends_session);
    VT_RUN(test_unknown_words_are_ignored);
    VT_RUN(test_overflow_is_capped_and_flagged);
    VT_RUN(test_monochrome_bit_is_carried);
}

VT_TEST_MAIN()
