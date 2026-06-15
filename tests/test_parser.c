/*
 * Robustness and adversarial-input tests for the streaming frame parser.
 *
 * test_frame.c pins the happy-path decode; this file hammers the edges a real
 * (or hostile) serial link produces: NULL/empty feeds, NULL sink callbacks,
 * partial words, frames torn down mid-flight, out-of-range coordinates, pipeline
 * overflow and recovery, and a deterministic fuzz of random bytes that asserts
 * the parser's invariants always hold (no out-of-bounds; every stored coordinate
 * stays within the device grid).  Built with -fsanitize=address,undefined under
 * `make test SAN=1`, the fuzz turns any latent overrun into a hard failure.
 */
#include <stddef.h>
#include <string.h>

#include "frame.h"
#include "protocol.h"
#include "vt_test.h"

/* --- recording sink ----------------------------------------------------- */

typedef struct {
    vt_frame frames[4];
    int nframes;
    int exits;
    /* Fuzz invariants: incremented if any emitted frame breaks a guarantee. */
    int bad_count;
    int bad_coord;
} recorder;

static recorder g_rec;

static void rec_on_frame(void *ctx, const vt_frame *frame)
{
    recorder *r = (recorder *)ctx;
    int i;
    if (frame->count < 0 || frame->count > VT_MAX_PIPELINE) {
        r->bad_count++;
    }
    for (i = 0; i < frame->count && i < VT_MAX_PIPELINE; i++) {
        const vt_segment *s = &frame->segments[i];
        if (s->x0 > VT_DVG_RES_MAX || s->y0 > VT_DVG_RES_MAX || s->x1 > VT_DVG_RES_MAX ||
            s->y1 > VT_DVG_RES_MAX) {
            r->bad_coord++;
        }
    }
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
    uint8_t bytes[VT_WORD_BYTES];
    vt_pack_be(word, bytes);
    vt_parser_feed(p, bytes, sizeof bytes);
}

/* --- defensive API contract --------------------------------------------- */

static void test_null_and_empty_feeds_are_safe(void)
{
    vt_parser p;
    vt_sink s = recording_sink();
    uint8_t byte = 0x00;
    vt_parser_init(&p, s);

    /* None of these may dereference NULL, write anything, or dispatch a frame. */
    vt_parser_feed(NULL, &byte, 1);
    vt_parser_feed(&p, NULL, 4);
    vt_parser_feed(&p, &byte, 0);
    vt_parser_feed_word(NULL, 0x12345678u);
    vt_parser_init(NULL, s);
    vt_parser_reset_frame(NULL);

    VT_CHECK_EQ(g_rec.nframes, 0);
    VT_CHECK_EQ(p.words_seen, 0);
    VT_CHECK_EQ(p.nbytes, 0);
}

static void test_null_sink_callbacks_are_safe(void)
{
    /* vt_parser_init documents that either callback may be NULL; a full session
     * must then run without dispatching through a NULL pointer. */
    vt_parser p;
    vt_sink sink;
    sink.ctx = NULL;
    sink.on_frame = NULL;
    sink.on_exit = NULL;
    vt_parser_init(&p, sink);

    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(100, 100, true));
    feed_word(&p, vt_encode_xy(200, 200, false));
    feed_word(&p, vt_encode_complete(false)); /* on_frame NULL: must not crash */
    feed_word(&p, vt_encode_exit());          /* on_exit  NULL: must not crash */

    VT_CHECK_EQ(p.frames_done, 1);
    VT_CHECK(p.session_over);
}

/* --- reassembly --------------------------------------------------------- */

static void test_partial_word_does_not_dispatch(void)
{
    vt_parser p;
    uint8_t three[3] = {0x00, 0x00, 0x00};
    uint8_t last = 0x00;
    vt_parser_init(&p, recording_sink());

    vt_parser_feed(&p, three, sizeof three); /* 3 of a COMPLETE word's 4 bytes */
    VT_CHECK_EQ(p.words_seen, 0);
    VT_CHECK_EQ(g_rec.nframes, 0);
    VT_CHECK_EQ(p.nbytes, 3);

    vt_parser_feed(&p, &last, 1); /* the 4th byte completes the word */
    VT_CHECK_EQ(p.words_seen, 1);
    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(p.nbytes, 0);
}

static const uint8_t WORKED_EXAMPLE[] = {
    0x80, 0x00, 0x01, 0x90, 0x20, 0xF0, 0xF0, 0xF0, 0x52, 0x00, 0x48, 0x02,
    0x42, 0x64, 0x48, 0x02, 0x60, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
};

static void test_every_split_reconstructs_same_frame(void)
{
    /* Split the stream at every byte boundary; reassembly must produce the
     * identical single-vector frame no matter where the chunk break falls. */
    size_t split;
    for (split = 0; split <= sizeof WORKED_EXAMPLE; split++) {
        vt_parser p;
        vt_parser_init(&p, recording_sink());
        vt_parser_feed(&p, WORKED_EXAMPLE, split);
        vt_parser_feed(&p, WORKED_EXAMPLE + split, sizeof WORKED_EXAMPLE - split);
        VT_CHECK_EQ(g_rec.nframes, 1);
        VT_CHECK_EQ(g_rec.frames[0].count, 1);
        VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 2449);
    }
}

/* --- frame lifecycle edges ---------------------------------------------- */

static void test_reset_frame_discards_in_progress(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(123));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(10, 20, true));
    feed_word(&p, vt_encode_xy(30, 40, false)); /* one vector queued */
    VT_CHECK_EQ(p.frame.count, 1);

    vt_parser_reset_frame(&p);
    VT_CHECK_EQ(p.frame.count, 0);
    VT_CHECK_EQ(p.frame.total_length, 0);
    VT_CHECK(!p.have_pos);
    VT_CHECK_EQ(p.brightness, 0);

    /* A COMPLETE now publishes the empty frame, not the discarded vector. */
    feed_word(&p, vt_encode_complete(false));
    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 0);
}

static void test_bare_complete_emits_empty_frame(void)
{
    /* A COMPLETE with no preceding FRAME must still emit a well-formed (empty)
     * frame rather than read uninitialised state. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_complete(false));
    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 0);
    VT_CHECK_EQ(p.frames_done, 1);
}

static void test_reframe_discards_previous_and_resets_beam(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());

    /* First frame body, never COMPLETEd. */
    feed_word(&p, vt_encode_frame(100));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(10, 20, true));
    feed_word(&p, vt_encode_xy(30, 40, false));

    /* A new FRAME header abandons it: fresh length, beam back to center. */
    feed_word(&p, vt_encode_frame(200));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(300, 400, false)); /* lit, no move => from center */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].total_length, 200);
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x0, VT_DEV_CENTER);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y0, VT_DEV_CENTER);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 300);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y1, 400);
}

static void test_brightness_changes_within_frame(void)
{
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0)); /* 240 */
    feed_word(&p, vt_encode_xy(0, 0, true));
    feed_word(&p, vt_encode_xy(100, 0, false));     /* z = 240 */
    feed_word(&p, vt_encode_rgb(0x10, 0x00, 0x00)); /* 16  */
    feed_word(&p, vt_encode_xy(200, 0, false));     /* z = 16  */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.frames[0].count, 2);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].brightness, 240);
    VT_CHECK_EQ(g_rec.frames[0].segments[1].brightness, 16);
}

static void test_exit_does_not_wedge_parser(void)
{
    /* EXIT ends the *session* (the app stops feeding) but must not corrupt the
     * pure parser: a frame fed afterwards still reconstructs correctly. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_exit());
    VT_CHECK(p.session_over);
    VT_CHECK_EQ(g_rec.exits, 1);

    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(0, 0, true));
    feed_word(&p, vt_encode_xy(100, 100, false));
    feed_word(&p, vt_encode_complete(false));
    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
}

/* --- malformed input ---------------------------------------------------- */

static void test_out_of_range_coords_are_clamped(void)
{
    /* 8000/9000/5000 are valid 14-bit fields but past the 0..4095 device grid;
     * the parser must clamp them so no out-of-range point reaches a frame. */
    vt_parser p;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(8000, 9000, true)); /* move, clamped to (4095,4095) */
    feed_word(&p, vt_encode_xy(5000, 100, false)); /* draw, x clamped to 4095     */
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x0, VT_DVG_RES_MAX);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y0, VT_DVG_RES_MAX);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, VT_DVG_RES_MAX);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].y1, 100);
}

static void test_overflow_then_next_frame_recovers(void)
{
    vt_parser p;
    int i;
    vt_parser_init(&p, recording_sink());

    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(0, 0, true));
    for (i = 0; i < VT_MAX_PIPELINE + 100; i++) {
        feed_word(&p, vt_encode_xy(1, 1, false));
    }
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.frames[0].count, VT_MAX_PIPELINE);
    VT_CHECK(g_rec.frames[0].overflow);

    /* The cap and the overflow flag must both clear for the next frame. */
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(0, 0, true));
    feed_word(&p, vt_encode_xy(50, 60, false));
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 2);
    VT_CHECK_EQ(g_rec.frames[1].count, 1);
    VT_CHECK(!g_rec.frames[1].overflow);
}

/* --- fuzz --------------------------------------------------------------- */

/* Deterministic xorshift32 so the fuzz is reproducible in CI (no rand()). */
static uint32_t fuzz_rng(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_fuzz_random_bytes_preserve_invariants(void)
{
    /* Feed millions of random bytes in random-sized chunks.  The sink validates
     * every emitted frame; reaching the end without a crash (ASan/UBSan in CI)
     * and with zero invariant violations is the assertion. */
    vt_parser p;
    uint32_t state = 0x01234567u;
    long iter;
    vt_parser_init(&p, recording_sink());

    for (iter = 0; iter < 100000; iter++) {
        uint8_t chunk[64];
        size_t n = (size_t)(fuzz_rng(&state) % (sizeof chunk + 1)); /* 0..64 */
        size_t k;
        for (k = 0; k < n; k++) {
            chunk[k] = (uint8_t)(fuzz_rng(&state) >> 24);
        }
        vt_parser_feed(&p, chunk, n);

        /* The in-progress frame and the reassembly buffer must stay in bounds
         * at all times, not just at COMPLETE. */
        if (p.frame.count < 0 || p.frame.count > VT_MAX_PIPELINE) {
            g_rec.bad_count++;
        }
        if (p.nbytes < 0 || p.nbytes >= VT_WORD_BYTES) {
            g_rec.bad_count++;
        }
    }

    VT_CHECK_EQ(g_rec.bad_count, 0);
    VT_CHECK_EQ(g_rec.bad_coord, 0);
    VT_CHECK(p.words_seen > 0); /* random data does form whole words */
}

static void run_all(void)
{
    VT_RUN(test_null_and_empty_feeds_are_safe);
    VT_RUN(test_null_sink_callbacks_are_safe);
    VT_RUN(test_partial_word_does_not_dispatch);
    VT_RUN(test_every_split_reconstructs_same_frame);
    VT_RUN(test_reset_frame_discards_in_progress);
    VT_RUN(test_bare_complete_emits_empty_frame);
    VT_RUN(test_reframe_discards_previous_and_resets_beam);
    VT_RUN(test_brightness_changes_within_frame);
    VT_RUN(test_exit_does_not_wedge_parser);
    VT_RUN(test_out_of_range_coords_are_clamped);
    VT_RUN(test_overflow_then_next_frame_recovers);
    VT_RUN(test_fuzz_random_bytes_preserve_invariants);
}

VT_TEST_MAIN()
