/*
 * Wire-level decoding for the USB-DVG / pitrex *vecterm* serial protocol.
 *
 * This translation unit is **pure**: no I/O, no hardware, no libpitrex.  It can
 * be compiled and unit-tested on any host.  It is the receiving counterpart to
 * pyvterm's `protocol.py` and mirrors the command encoding in gtoal/pitrex's
 * VMMenu/Win32/dvg/zvgFrame.c (USB-DVG drivers by Mario Montminy, 2020),
 * cross-checked against AdvanceMAME's advance/osd/dvg.c.
 *
 * Every command is a single 32-bit word transmitted **big-endian** (most
 * significant byte first).  The top three bits [31:29] select the command:
 *
 *     bit  31      29 28                                              0
 *          +---------+------------------------------------------------+
 *          |  flag   |  payload (command-specific)                    |
 *          +---------+------------------------------------------------+
 *
 * See docs/PROTOCOL.md for the full specification.
 */
#ifndef VEKTERM_PROTOCOL_H
#define VEKTERM_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

/* Command selector occupying the top three bits of every word. */
typedef enum {
    VT_FLAG_COMPLETE = 0x0, /* End-of-frame marker (payload 0 or the mono bit). */
    VT_FLAG_RGB = 0x1,      /* Set the colour/intensity of following vectors.   */
    VT_FLAG_XY = 0x2,       /* Move (beam off) or draw (beam on) to a coord.    */
    VT_FLAG_QUALITY = 0x3,  /* Render-quality hint (pitrex/zvgFrame variant).   */
    VT_FLAG_FRAME = 0x4,    /* Frame header carrying total beam-travel length.  */
    VT_FLAG_CMD = 0x5,      /* Device command channel (AdvanceMAME).            */
    VT_FLAG_EXT = 0x6,      /* Reserved for the extensions proposal (ignored).  */
    VT_FLAG_EXIT = 0x7      /* Tell the device the session is over.             */
} vt_flag;

/* Bit layout. */
#define VT_WORD_BYTES 4                   /* Every command is one 32-bit (4-byte) word. */
#define VT_FLAG_SHIFT 29                  /* The flag occupies bits [31:29].          */
#define VT_BLANK_SHIFT 28                 /* The XY blank (beam-off) bit.             */
#define VT_COORD_BITS 14                  /* Each XY coordinate is 14 bits wide.      */
#define VT_COORD_MASK 0x3FFFu             /* (1 << 14) - 1.                           */
#define VT_PAYLOAD_MASK 0x1FFFFFFFu       /* Bits available below the flag.          */
#define VT_COMPLETE_MONOCHROME (1u << 28) /* OR'd into COMPLETE for B&W games. */

/* Device resolution: the 12-bit DAC grid that XY coordinates address. */
#define VT_DVG_RES_MIN 0
#define VT_DVG_RES_MAX 4095
#define VT_DVG_RENDER_QUALITY 5

/* A single decoded command word. */
typedef struct {
    vt_flag flag;
    /* XY */
    bool blank;
    uint16_t x;
    uint16_t y;
    /* RGB */
    uint8_t r;
    uint8_t g;
    uint8_t b;
    /* FRAME / QUALITY payload */
    uint32_t value;
    /* COMPLETE */
    bool monochrome;
} vt_command;

/* Return just the flag of a raw word. */
vt_flag vt_word_flag(uint32_t word);

/* Decode a 32-bit word into its command fields. */
vt_command vt_decode_word(uint32_t word);

/* Big-endian word <-> 4 bytes. */
void vt_pack_be(uint32_t word, uint8_t out[VT_WORD_BYTES]);
uint32_t vt_unpack_be(const uint8_t in[VT_WORD_BYTES]);

/*
 * Word encoders.  The receiver only strictly needs the decoders above, but the
 * encoders pin the wire format in tests and back the optional device->host
 * capability channel (GET_DVG_INFO).
 */
uint32_t vt_encode_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t vt_encode_xy(uint16_t x, uint16_t y, bool blank);
uint32_t vt_encode_frame(uint32_t vector_length);
uint32_t vt_encode_quality(uint32_t value);
uint32_t vt_encode_complete(bool monochrome);
uint32_t vt_encode_exit(void);

/*
 * Scale a ~4-bit colour channel to 8 bits, clamped to 255 (zvgFrameSetRGB15:
 * value << 4, so 15 -> 240 and >= 16 saturates).  Senders pre-scale colours,
 * so the receiver normally never calls this; it exists for symmetry/tests.
 */
uint8_t vt_scale_color(int value);

/* Monochrome beam intensity from an RGB triple (the brightest channel). */
uint8_t vt_brightness_from_rgb(uint8_t r, uint8_t g, uint8_t b);

/*
 * Map a device coordinate (0..4095) linearly onto [out_lo, out_hi] (the Vectrex
 * integrator range), rounded to nearest.  Out-of-range inputs are clamped.
 */
int vt_map_coord(int dev, int out_lo, int out_hi);

#endif /* VEKTERM_PROTOCOL_H */
