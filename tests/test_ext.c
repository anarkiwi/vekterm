/*
 * v2 extension tests — decode EXT (HEIGHTFIELD/POLYLINE) and the HELLO probe.
 *
 * The golden HEIGHTFIELD/POLYLINE byte sequences are the cross-language
 * contract: pyvterm's tests/test_ext.py emits these exact bytes and asserts the
 * same reconstructed segments, so encoder (Python) and decoder (C) cannot drift.
 */
#include <string.h>

#include "frame.h"
#include "protocol.h"
#include "vt_test.h"

/* --- recording sink ----------------------------------------------------- */

typedef struct {
    vt_frame frames[4];
    int nframes;
    int queries;
    int keepalives;
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

static void rec_on_query(void *ctx)
{
    ((recorder *)ctx)->queries++;
}

static void rec_on_keepalive(void *ctx)
{
    ((recorder *)ctx)->keepalives++;
}

static vt_sink recording_sink(void)
{
    vt_sink sink;
    memset(&g_rec, 0, sizeof g_rec);
    sink.ctx = &g_rec;
    sink.on_frame = rec_on_frame;
    sink.on_exit = NULL;
    sink.on_query = rec_on_query;
    sink.on_keepalive = rec_on_keepalive;
    return sink;
}

static void feed_word(vt_parser *p, uint32_t word)
{
    uint8_t bytes[VT_WORD_BYTES];
    vt_pack_be(word, bytes);
    vt_parser_feed(p, bytes, sizeof bytes);
}

/* The canonical 3x2 HEIGHTFIELD from tests/test_ext.py, EXT command bytes. */
static const uint8_t HF_GOLDEN[] = {
    0xc1, 0x00, 0x00, 0x16,             /* EXT subtype=1 (HEIGHTFIELD) length=22 */
    0x00,                               /* flags: none                          */
    0x00, 0x03,                         /* cols=3                               */
    0x00, 0x02,                         /* rows=2                               */
    0x03, 0xe8,                         /* x0=1000                              */
    0x01, 0xf4,                         /* x_step=500                           */
    0x07, 0xd0,                         /* y0=2000                              */
    0xfe, 0x70,                         /* y_step=-400                          */
    0x01, 0x00,                         /* y_scale=256                          */
    0xf0,                               /* brightness=240                       */
    0x00, 0x80, 0xff, 0xff, 0x00, 0x40, /* displacement rows 0 and 1            */
};

/* The canonical POLYLINE from tests/test_ext.py, EXT command bytes. */
static const uint8_t PL_GOLDEN[] = {
    0xc2, 0x00, 0x00, 0x0e, /* EXT subtype=2 (POLYLINE) length=14 */
    0x00,                   /* flags: none                       */
    0xc8,                   /* brightness=200                    */
    0x08, 0x00,             /* x0=2048                           */
    0x08, 0x00,             /* y0=2048                           */
    0x00, 0x04,             /* count=4                           */
    0x0a, 0x00,             /* (10, 0)                           */
    0x00, 0x14,             /* (0, 20)                           */
    0xfb, 0xfb,             /* (-5, -5)                          */
};

static void check_segment(const vt_segment *s, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                          uint8_t z)
{
    VT_CHECK_EQ(s->x0, x0);
    VT_CHECK_EQ(s->y0, y0);
    VT_CHECK_EQ(s->x1, x1);
    VT_CHECK_EQ(s->y1, y1);
    VT_CHECK_EQ(s->brightness, z);
}

/* Wrap an EXT command in FRAME ... COMPLETE and feed it. */
static void feed_ext_frame(vt_parser *p, const uint8_t *ext, size_t len)
{
    feed_word(p, vt_encode_frame(0));
    vt_parser_feed(p, ext, len);
    feed_word(p, vt_encode_complete(false));
}

/* --- tests -------------------------------------------------------------- */

static void test_heightfield_reconstructs_segments(void)
{
    vt_parser p;
    const vt_frame *f;
    vt_parser_init(&p, recording_sink());
    feed_ext_frame(&p, HF_GOLDEN, sizeof HF_GOLDEN);

    VT_CHECK_EQ(g_rec.nframes, 1);
    f = &g_rec.frames[0];
    VT_CHECK_EQ(f->count, 4);
    VT_CHECK(!f->overflow);
    check_segment(&f->segments[0], 1000, 2000, 1500, 2128, 240);
    check_segment(&f->segments[1], 1500, 2128, 2000, 2255, 240);
    check_segment(&f->segments[2], 1000, 1855, 1500, 1600, 240);
    check_segment(&f->segments[3], 1500, 1600, 2000, 1664, 240);
}

static void test_heightfield_split_across_reads(void)
{
    /* A real link delivers arbitrary chunk boundaries; the EXT payload must
     * reassemble identically when fed one byte at a time. */
    vt_parser p;
    size_t i;
    uint8_t b;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    for (i = 0; i < sizeof HF_GOLDEN; i++) {
        b = HF_GOLDEN[i];
        vt_parser_feed(&p, &b, 1);
    }
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 4);
    check_segment(&g_rec.frames[0].segments[3], 1500, 1600, 2000, 1664, 240);
}

static void test_heightfield_serpentine(void)
{
    vt_parser p;
    uint8_t bytes[sizeof HF_GOLDEN];
    memcpy(bytes, HF_GOLDEN, sizeof HF_GOLDEN);
    bytes[4] = 0x02; /* flags: serpentine */
    vt_parser_init(&p, recording_sink());
    feed_ext_frame(&p, bytes, sizeof bytes);

    /* Row 1 is now walked right-to-left, reversing its segment endpoints. */
    VT_CHECK_EQ(g_rec.frames[0].count, 4);
    check_segment(&g_rec.frames[0].segments[2], 2000, 1664, 1500, 1600, 240);
    check_segment(&g_rec.frames[0].segments[3], 1500, 1600, 1000, 1855, 240);
}

static void test_heightfield_intensity_plane_blanks_gaps(void)
{
    /* Append an intensity plane that darkens row 0 column 1, splitting its run;
     * row 1 stays a single run. flags bit0 = intensity present. */
    vt_parser p;
    uint8_t bytes[sizeof HF_GOLDEN + 6];
    static const uint8_t plane[6] = {240, 0, 240, 240, 240, 240};
    memcpy(bytes, HF_GOLDEN, sizeof HF_GOLDEN);
    bytes[0] = 0xc1; /* still HEIGHTFIELD */
    bytes[3] = 0x1c; /* length now 16 + 6 + 6 = 28 */
    bytes[4] = 0x01; /* flags: intensity plane present */
    memcpy(bytes + sizeof HF_GOLDEN, plane, sizeof plane);
    vt_parser_init(&p, recording_sink());
    feed_ext_frame(&p, bytes, sizeof bytes);

    /* Row 0: c0 starts, c1 blanks (run reset), c2 starts -> no lit pair.
     * Row 1: two segments. */
    VT_CHECK_EQ(g_rec.frames[0].count, 2);
    check_segment(&g_rec.frames[0].segments[0], 1000, 1855, 1500, 1600, 240);
    check_segment(&g_rec.frames[0].segments[1], 1500, 1600, 2000, 1664, 240);
}

static void test_polyline_reconstructs_segments(void)
{
    vt_parser p;
    const vt_frame *f;
    vt_parser_init(&p, recording_sink());
    feed_ext_frame(&p, PL_GOLDEN, sizeof PL_GOLDEN);

    VT_CHECK_EQ(g_rec.nframes, 1);
    f = &g_rec.frames[0];
    VT_CHECK_EQ(f->count, 3);
    check_segment(&f->segments[0], 2048, 2048, 2058, 2048, 200);
    check_segment(&f->segments[1], 2058, 2048, 2058, 2068, 200);
    check_segment(&f->segments[2], 2058, 2068, 2053, 2063, 200);
}

static void test_unknown_subtype_is_skipped_in_alignment(void)
{
    /* An unrecognised EXT subtype must be drained by its length so the words
     * after it still parse — here a normal XY draw and COMPLETE. */
    vt_parser p;
    const uint8_t junk[4] = {0xde, 0xad, 0xbe, 0xef};
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_ext(0x1F, 4)); /* unknown subtype, 4-byte payload */
    vt_parser_feed(&p, junk, sizeof junk);
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(100, 100, true));
    feed_word(&p, vt_encode_xy(300, 300, false));
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 300);
}

static void test_oversized_payload_is_drained_and_flagged(void)
{
    /* A payload larger than VT_EXT_MAX is not buffered, but its bytes are still
     * consumed so the stream stays word-aligned; the frame is flagged overflow. */
    vt_parser p;
    uint32_t big = VT_EXT_MAX + 8u;
    uint32_t i;
    uint8_t zero = 0;
    vt_parser_init(&p, recording_sink());
    feed_word(&p, vt_encode_frame(0));
    feed_word(&p, vt_encode_ext(VT_EXT_HEIGHTFIELD, big));
    for (i = 0; i < big; i++) {
        vt_parser_feed(&p, &zero, 1);
    }
    feed_word(&p, vt_encode_rgb(0xF0, 0xF0, 0xF0));
    feed_word(&p, vt_encode_xy(0, 0, true));
    feed_word(&p, vt_encode_xy(123, 45, false));
    feed_word(&p, vt_encode_complete(false));

    VT_CHECK_EQ(g_rec.nframes, 1);
    VT_CHECK(g_rec.frames[0].overflow);
    /* Alignment preserved: the trailing XY draw still landed. */
    VT_CHECK_EQ(g_rec.frames[0].count, 1);
    VT_CHECK_EQ(g_rec.frames[0].segments[0].x1, 123);
}

static void test_hello_probe_triggers_query(void)
{
    vt_parser p;
    uint32_t hello = ((uint32_t)VT_FLAG_CMD << VT_FLAG_SHIFT) | VT_CMD_HELLO;
    vt_parser_init(&p, recording_sink());
    VT_CHECK(vt_is_hello(hello));
    feed_word(&p, hello);
    VT_CHECK_EQ(g_rec.queries, 1);
    /* A different CMD subcommand must not be treated as HELLO. */
    feed_word(&p, ((uint32_t)VT_FLAG_CMD << VT_FLAG_SHIFT) | 0x01u);
    VT_CHECK_EQ(g_rec.queries, 1);
}

static void test_keepalive_triggers_callback(void)
{
    vt_parser p;
    uint32_t ka = vt_encode_keepalive();
    vt_parser_init(&p, recording_sink());
    VT_CHECK(vt_is_keepalive(ka));
    VT_CHECK(!vt_is_hello(ka)); /* keepalive and HELLO are distinct CMDs */
    feed_word(&p, ka);
    VT_CHECK_EQ(g_rec.keepalives, 1);
    VT_CHECK_EQ(g_rec.queries, 0);
    /* A keepalive carries no geometry and starts no frame. */
    VT_CHECK_EQ(g_rec.nframes, 0);
    /* HELLO must not be mistaken for a keepalive. */
    feed_word(&p, ((uint32_t)VT_FLAG_CMD << VT_FLAG_SHIFT) | VT_CMD_HELLO);
    VT_CHECK_EQ(g_rec.keepalives, 1);
}

static void test_hello_descriptor_bytes(void)
{
    uint8_t d[VT_HELLO_LEN];
    static const uint8_t want[VT_HELLO_LEN] = {
        0x56, 0x4b, /* 'V' 'K'                 */
        0x02,       /* version 2               */
        0x07,       /* caps HF|POLYLINE|INT    */
        0x0c,       /* coord bits 12           */
        0x08,       /* brightness bits 8       */
        0x0b, 0xb8, /* max_pipeline 3000       */
        0x20, 0x00, /* max_payload 8192        */
        0x32,       /* refresh 50 Hz           */
        0x00,       /* reserved                */
    };
    vt_encode_hello_descriptor(d);
    VT_CHECK(memcmp(d, want, sizeof want) == 0);
}

static void run_all(void)
{
    VT_RUN(test_heightfield_reconstructs_segments);
    VT_RUN(test_heightfield_split_across_reads);
    VT_RUN(test_heightfield_serpentine);
    VT_RUN(test_heightfield_intensity_plane_blanks_gaps);
    VT_RUN(test_polyline_reconstructs_segments);
    VT_RUN(test_unknown_subtype_is_skipped_in_alignment);
    VT_RUN(test_oversized_payload_is_drained_and_flagged);
    VT_RUN(test_hello_probe_triggers_query);
    VT_RUN(test_keepalive_triggers_callback);
    VT_RUN(test_hello_descriptor_bytes);
}

VT_TEST_MAIN()
