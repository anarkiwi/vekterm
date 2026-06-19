/*
 * Stateful frame assembly for the receiver.
 *
 * `vt_parser` is the inverse of pyvterm's FrameBuilder: it consumes a byte
 * stream, reassembles big-endian command words (handling words split across
 * reads), tracks the beam position and current intensity, and accumulates the
 * **lit** vectors of a frame into a `vt_frame` buffer.  When a COMPLETE word
 * arrives it hands the finished frame to a `vt_sink`; an EXIT word ends the
 * session.
 *
 * This translation unit is **pure** (no I/O, no hardware), so the exact set of
 * vectors decoded from a wire stream can be asserted in unit tests.
 */
#ifndef VEKTERM_FRAME_H
#define VEKTERM_FRAME_H

#include <stddef.h>

#include "protocol.h"

/*
 * Maximum vectors buffered per frame.  Matches the receiver's MAX_PIPELINE in
 * pitrex/vectrex/vectrexInterface.h; vectors beyond it are dropped and the
 * frame's `overflow` flag is set.
 */
#ifndef VT_MAX_PIPELINE
#define VT_MAX_PIPELINE 3000
#endif

/* Device-space center; the beam rests here after a recal, and an early lit XY
 * with no preceding move starts from here. */
#define VT_DEV_CENTER ((VT_DVG_RES_MAX + 1) / 2)

/* One drawn vector in device coordinates (0..4095) with its beam intensity. */
typedef struct {
    uint16_t x0;
    uint16_t y0;
    uint16_t x1;
    uint16_t y1;
    uint8_t brightness; /* 0..255, the brightest RGB channel in force. */
} vt_segment;

/* A complete frame: the vectors to (re)draw until the next frame arrives. */
typedef struct {
    vt_segment segments[VT_MAX_PIPELINE];
    int count;
    uint32_t total_length; /* Beam-travel length from the FRAME header.      */
    bool monochrome;       /* COMPLETE monochrome bit.                       */
    bool overflow;         /* Set when more than VT_MAX_PIPELINE were offered. */
} vt_frame;

/* Where finished frames and session end are delivered. */
typedef struct {
    void *ctx;
    void (*on_frame)(void *ctx, const vt_frame *frame); /* COMPLETE */
    void (*on_exit)(void *ctx);                         /* EXIT     */
} vt_sink;

/* Streaming decoder state. */
typedef struct {
    vt_sink sink;

    /* Byte reassembly buffer for one big-endian word.  Invariant maintained by
     * vt_parser_feed: 0 <= nbytes < VT_WORD_BYTES between bytes. */
    uint8_t bytes[VT_WORD_BYTES];
    int nbytes;

    /* Beam + colour state. */
    int cur_x;
    int cur_y;
    bool have_pos;
    uint8_t brightness;

    /* Frame under construction. */
    vt_frame frame;

    /* Lifetime stats / status. */
    uint64_t words_seen;
    uint64_t frames_done;
    bool session_over;
} vt_parser;

/* Initialise a parser with the given sink (either callback may be NULL). */
void vt_parser_init(vt_parser *parser, vt_sink sink);

/* Discard the in-progress frame and reset beam/colour state. */
void vt_parser_reset_frame(vt_parser *parser);

/* Drop a partial word so the next byte begins a fresh 4-byte word (call at a
 * known frame boundary to recover from a slipped byte). */
void vt_parser_resync(vt_parser *parser);

/* Feed a single already-assembled word through the state machine. */
void vt_parser_feed_word(vt_parser *parser, uint32_t word);

/* Feed raw bytes; complete words are dispatched as they form. */
void vt_parser_feed(vt_parser *parser, const uint8_t *data, size_t len);

#endif /* VEKTERM_FRAME_H */
