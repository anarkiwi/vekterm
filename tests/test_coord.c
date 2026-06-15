/* Coordinate mapping, intensity, and colour-scaling helpers. */
#include "protocol.h"
#include "vt_test.h"

/* The server's default Vectrex integrator range (see src/vekterm.c). */
#define LO (-2048)
#define HI 2047

static void test_map_coord_endpoints(void)
{
    VT_CHECK_EQ(vt_map_coord(0, LO, HI), LO);
    VT_CHECK_EQ(vt_map_coord(VT_DVG_RES_MAX, LO, HI), HI);
    /* Device center (~2048) maps to the screen center (~0). */
    VT_CHECK_EQ(vt_map_coord(2048, LO, HI), 0);
    /* The protocol's origin device coords (2049, 2050) sit just past center. */
    VT_CHECK_EQ(vt_map_coord(2049, LO, HI), 1);
    VT_CHECK_EQ(vt_map_coord(2050, LO, HI), 2);
}

static void test_map_coord_clamps(void)
{
    VT_CHECK_EQ(vt_map_coord(-100, LO, HI), LO);
    VT_CHECK_EQ(vt_map_coord(99999, LO, HI), HI);
}

static void test_map_coord_arbitrary_range(void)
{
    VT_CHECK_EQ(vt_map_coord(0, 0, 4095), 0);
    VT_CHECK_EQ(vt_map_coord(4095, 0, 4095), 4095);
    VT_CHECK_EQ(vt_map_coord(2048, 0, 1000), 500);
}

static void test_map_coord_inverted_range(void)
{
    /* A descending range (axis flip): device 0 -> the high end, device max ->
     * the low end, both *exactly* (the old rounding was off by one here). */
    VT_CHECK_EQ(vt_map_coord(0, HI, LO), HI);
    VT_CHECK_EQ(vt_map_coord(VT_DVG_RES_MAX, HI, LO), LO);
    {
        int mid = vt_map_coord(2048, HI, LO);
        VT_CHECK(mid >= -1 && mid <= 1); /* center still lands ~0 */
    }
}

static void test_map_coord_degenerate_range(void)
{
    /* out_lo == out_hi: every device value collapses onto the single point. */
    VT_CHECK_EQ(vt_map_coord(0, 7, 7), 7);
    VT_CHECK_EQ(vt_map_coord(2048, 7, 7), 7);
    VT_CHECK_EQ(vt_map_coord(VT_DVG_RES_MAX, 7, 7), 7);
}

static void test_map_coord_wide_range_no_overflow(void)
{
    /* span * dev exceeds 2^31 here, so a 32-bit intermediate (the baremetal
     * target's `long`) would overflow; the endpoints must still be exact. */
    VT_CHECK_EQ(vt_map_coord(0, 0, 2000000), 0);
    VT_CHECK_EQ(vt_map_coord(VT_DVG_RES_MAX, 0, 2000000), 2000000);
}

static void test_brightness_from_rgb(void)
{
    VT_CHECK_EQ(vt_brightness_from_rgb(240, 240, 240), 240);
    VT_CHECK_EQ(vt_brightness_from_rgb(0, 0, 0), 0);
    VT_CHECK_EQ(vt_brightness_from_rgb(16, 0, 0), 16);
    VT_CHECK_EQ(vt_brightness_from_rgb(10, 200, 30), 200);
}

static void test_scale_color(void)
{
    VT_CHECK_EQ(vt_scale_color(0), 0);
    VT_CHECK_EQ(vt_scale_color(8), 128);
    VT_CHECK_EQ(vt_scale_color(15), 240);
    VT_CHECK_EQ(vt_scale_color(16), 255); /* saturates */
    VT_CHECK_EQ(vt_scale_color(255), 255);
    VT_CHECK_EQ(vt_scale_color(-4), 0);
}

static void run_all(void)
{
    VT_RUN(test_map_coord_endpoints);
    VT_RUN(test_map_coord_clamps);
    VT_RUN(test_map_coord_arbitrary_range);
    VT_RUN(test_map_coord_inverted_range);
    VT_RUN(test_map_coord_degenerate_range);
    VT_RUN(test_map_coord_wide_range_no_overflow);
    VT_RUN(test_brightness_from_rgb);
    VT_RUN(test_scale_color);
}

VT_TEST_MAIN()
