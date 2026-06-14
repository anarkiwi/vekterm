/* Wire-format tests — these pin the exact words the protocol uses, decoded. */
#include "protocol.h"
#include "vt_test.h"

static void test_flag_values(void)
{
    VT_CHECK_EQ(VT_FLAG_COMPLETE, 0x0);
    VT_CHECK_EQ(VT_FLAG_RGB, 0x1);
    VT_CHECK_EQ(VT_FLAG_XY, 0x2);
    VT_CHECK_EQ(VT_FLAG_QUALITY, 0x3);
    VT_CHECK_EQ(VT_FLAG_FRAME, 0x4);
    VT_CHECK_EQ(VT_FLAG_CMD, 0x5);
    VT_CHECK_EQ(VT_FLAG_EXT, 0x6);
    VT_CHECK_EQ(VT_FLAG_EXIT, 0x7);
}

static void test_big_endian_roundtrip(void)
{
    uint8_t out[4];
    vt_pack_be(0x80000190u, out);
    VT_CHECK_EQ(out[0], 0x80);
    VT_CHECK_EQ(out[1], 0x00);
    VT_CHECK_EQ(out[2], 0x01);
    VT_CHECK_EQ(out[3], 0x90);
    VT_CHECK_EQ_U32(vt_unpack_be(out), 0x80000190u);

    {
        /* The worked-example "move to (2049, 2050)" word, byte for byte. */
        const uint8_t move[4] = {0x52, 0x00, 0x48, 0x02};
        VT_CHECK_EQ_U32(vt_unpack_be(move), 0x52004802u);
    }
}

static void test_encoders_match_worked_example(void)
{
    /* These are the exact words in docs/PROTOCOL.md's worked example. */
    VT_CHECK_EQ_U32(vt_encode_frame(400), 0x80000190u);
    VT_CHECK_EQ_U32(vt_encode_rgb(0xF0, 0xF0, 0xF0), 0x20F0F0F0u);
    VT_CHECK_EQ_U32(vt_encode_xy(2049, 2050, true), 0x52004802u);
    VT_CHECK_EQ_U32(vt_encode_xy(2449, 2050, false), 0x42644802u);
    VT_CHECK_EQ_U32(vt_encode_quality(5), 0x60000005u);
    VT_CHECK_EQ_U32(vt_encode_complete(false), 0x00000000u);
    VT_CHECK_EQ_U32(vt_encode_complete(true), 0x10000000u);
    VT_CHECK_EQ_U32(vt_encode_exit(), 0xE0000000u);
}

static void test_decode_xy(void)
{
    vt_command move = vt_decode_word(0x52004802u);
    VT_CHECK_EQ(move.flag, VT_FLAG_XY);
    VT_CHECK(move.blank);
    VT_CHECK_EQ(move.x, 2049);
    VT_CHECK_EQ(move.y, 2050);

    {
        vt_command draw = vt_decode_word(0x42644802u);
        VT_CHECK_EQ(draw.flag, VT_FLAG_XY);
        VT_CHECK(!draw.blank);
        VT_CHECK_EQ(draw.x, 2449);
        VT_CHECK_EQ(draw.y, 2050);
    }

    {
        /* Coordinates wider than 14 bits are masked to the field. */
        vt_command big = vt_decode_word(vt_encode_xy(0xFFFF, 0xFFFF, false));
        VT_CHECK_EQ(big.x, VT_COORD_MASK);
        VT_CHECK_EQ(big.y, VT_COORD_MASK);
    }
}

static void test_decode_rgb_frame_quality_complete_exit(void)
{
    vt_command rgb = vt_decode_word(0x20123456u);
    VT_CHECK_EQ(rgb.flag, VT_FLAG_RGB);
    VT_CHECK_EQ(rgb.r, 0x12);
    VT_CHECK_EQ(rgb.g, 0x34);
    VT_CHECK_EQ(rgb.b, 0x56);

    {
        vt_command frame = vt_decode_word(0x80000190u);
        VT_CHECK_EQ(frame.flag, VT_FLAG_FRAME);
        VT_CHECK_EQ(frame.value, 400);
    }
    {
        vt_command quality = vt_decode_word(0x60000005u);
        VT_CHECK_EQ(quality.flag, VT_FLAG_QUALITY);
        VT_CHECK_EQ(quality.value, 5);
    }
    {
        vt_command complete = vt_decode_word(0x00000000u);
        VT_CHECK_EQ(complete.flag, VT_FLAG_COMPLETE);
        VT_CHECK(!complete.monochrome);
    }
    {
        vt_command mono = vt_decode_word(0x10000000u);
        VT_CHECK_EQ(mono.flag, VT_FLAG_COMPLETE);
        VT_CHECK(mono.monochrome);
    }
    {
        vt_command quit = vt_decode_word(0xE0000000u);
        VT_CHECK_EQ(quit.flag, VT_FLAG_EXIT);
    }
}

static void test_word_flag(void)
{
    VT_CHECK_EQ(vt_word_flag(0x80000190u), VT_FLAG_FRAME);
    VT_CHECK_EQ(vt_word_flag(0x20F0F0F0u), VT_FLAG_RGB);
    VT_CHECK_EQ(vt_word_flag(0x52004802u), VT_FLAG_XY);
    VT_CHECK_EQ(vt_word_flag(0xC0000000u), VT_FLAG_EXT);
}

static void run_all(void)
{
    VT_RUN(test_flag_values);
    VT_RUN(test_big_endian_roundtrip);
    VT_RUN(test_encoders_match_worked_example);
    VT_RUN(test_decode_xy);
    VT_RUN(test_decode_rgb_frame_quality_complete_exit);
    VT_RUN(test_word_flag);
}

VT_TEST_MAIN()
