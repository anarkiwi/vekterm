#include "protocol.h"

#include <string.h>

void vt_pack_be(uint32_t word, uint8_t out[VT_WORD_BYTES])
{
    out[0] = (uint8_t)(word >> 24);
    out[1] = (uint8_t)(word >> 16);
    out[2] = (uint8_t)(word >> 8);
    out[3] = (uint8_t)(word >> 0);
}

uint32_t vt_unpack_be(const uint8_t in[VT_WORD_BYTES])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

vt_flag vt_word_flag(uint32_t word)
{
    return (vt_flag)((word >> VT_FLAG_SHIFT) & 0x7u);
}

vt_command vt_decode_word(uint32_t word)
{
    vt_command cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.flag = vt_word_flag(word);
    switch (cmd.flag) {
    case VT_FLAG_RGB:
        cmd.r = (uint8_t)((word >> 16) & 0xFFu);
        cmd.g = (uint8_t)((word >> 8) & 0xFFu);
        cmd.b = (uint8_t)(word & 0xFFu);
        break;
    case VT_FLAG_XY:
        cmd.blank = ((word >> VT_BLANK_SHIFT) & 0x1u) != 0;
        cmd.x = (uint16_t)((word >> VT_COORD_BITS) & VT_COORD_MASK);
        cmd.y = (uint16_t)(word & VT_COORD_MASK);
        break;
    case VT_FLAG_FRAME:
    case VT_FLAG_QUALITY:
        cmd.value = word & VT_PAYLOAD_MASK;
        break;
    case VT_FLAG_COMPLETE:
        cmd.monochrome = (word & VT_COMPLETE_MONOCHROME) != 0;
        break;
    case VT_FLAG_CMD:
    case VT_FLAG_EXT:
    case VT_FLAG_EXIT:
    default:
        break;
    }
    return cmd;
}

uint32_t vt_encode_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)VT_FLAG_RGB << VT_FLAG_SHIFT) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
           (uint32_t)b;
}

uint32_t vt_encode_xy(uint16_t x, uint16_t y, bool blank)
{
    return ((uint32_t)VT_FLAG_XY << VT_FLAG_SHIFT) | ((blank ? 1u : 0u) << VT_BLANK_SHIFT) |
           (((uint32_t)x & VT_COORD_MASK) << VT_COORD_BITS) | ((uint32_t)y & VT_COORD_MASK);
}

uint32_t vt_encode_frame(uint32_t vector_length)
{
    return ((uint32_t)VT_FLAG_FRAME << VT_FLAG_SHIFT) | (vector_length & VT_PAYLOAD_MASK);
}

uint32_t vt_encode_quality(uint32_t value)
{
    return ((uint32_t)VT_FLAG_QUALITY << VT_FLAG_SHIFT) | (value & VT_PAYLOAD_MASK);
}

uint32_t vt_encode_complete(bool monochrome)
{
    return ((uint32_t)VT_FLAG_COMPLETE << VT_FLAG_SHIFT) |
           (monochrome ? VT_COMPLETE_MONOCHROME : 0u);
}

uint32_t vt_encode_exit(void)
{
    return (uint32_t)VT_FLAG_EXIT << VT_FLAG_SHIFT;
}

uint8_t vt_scale_color(int value)
{
    int scaled;
    if (value < 0) {
        return 0;
    }
    scaled = value << 4;
    return scaled > 255 ? 255 : (uint8_t)scaled;
}

uint8_t vt_brightness_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t m = r;
    if (g > m) {
        m = g;
    }
    if (b > m) {
        m = b;
    }
    return m;
}

int vt_map_coord(int dev, int out_lo, int out_hi)
{
    int64_t span;
    int64_t num;
    int64_t q;

    if (dev < VT_DVG_RES_MIN) {
        dev = VT_DVG_RES_MIN;
    }
    if (dev > VT_DVG_RES_MAX) {
        dev = VT_DVG_RES_MAX;
    }

    /* Linear interpolation across the device grid, rounded to nearest.  The
     * 64-bit intermediate keeps `span * dev` exact for any int endpoints (the
     * baremetal target's `long` is only 32 bits, so the old 32-bit product could
     * overflow), and rounding half away from zero keeps both endpoints exact
     * whether the output range ascends (-2048..2047) or descends (2047..-2048). */
    span = (int64_t)out_hi - (int64_t)out_lo;
    num = span * (int64_t)dev;
    if (num >= 0) {
        q = (num + VT_DVG_RES_MAX / 2) / VT_DVG_RES_MAX;
    } else {
        q = (num - VT_DVG_RES_MAX / 2) / VT_DVG_RES_MAX;
    }
    return out_lo + (int)q;
}
