/*
 * gen_frame.c — emit a USB-DVG/vecterm byte stream for a known test shape, so
 * the emulator can be validated against geometry we control. Writes to stdout.
 *
 *   gen_frame square > square.bin
 *
 * Uses the real protocol.c encoders, so the bytes are exactly what a sender
 * (pyvterm / MAME) would put on the wire.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"

static void emit(uint32_t word)
{
    uint8_t b[VT_WORD_BYTES];
    vt_pack_be(word, b);
    fwrite(b, 1, sizeof b, stdout);
}

int main(int argc, char **argv)
{
    const char *shape = argc > 1 ? argv[1] : "square";

    emit(vt_encode_frame(0));
    emit(vt_encode_rgb(0xF0, 0xF0, 0xF0));      /* white */

    if (!strcmp(shape, "square")) {
        /* centred square, device coords 0..4095 (centre 2048) */
        emit(vt_encode_xy(1024, 1024, true));   /* move  to bottom-left  */
        emit(vt_encode_xy(3072, 1024, false));  /* draw  to bottom-right */
        emit(vt_encode_xy(3072, 3072, false));  /* draw  to top-right    */
        emit(vt_encode_xy(1024, 3072, false));  /* draw  to top-left     */
        emit(vt_encode_xy(1024, 1024, false));  /* draw  back to start   */
    } else { /* "line": the docs/PROTOCOL.md worked example */
        emit(vt_encode_xy(2049, 2050, true));
        emit(vt_encode_xy(2449, 2050, false));
    }

    emit(vt_encode_complete(false));
    return 0;
}
