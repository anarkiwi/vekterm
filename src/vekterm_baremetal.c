/*
 * vekterm — baremetal entry point for the PiTrex.
 *
 * This is the deployable form of vekterm: a freestanding PiTrex program that the
 * pitrex baremetal framework boots into directly (no Linux). The framework's
 * kernelMain() sets up clocks + the mini-UART and then calls main() in a loop.
 *
 * main() reads the USB-DVG / vecterm protocol off the mini-UART, feeds it to the
 * shared pure parser (frame.c / protocol.c), and re-draws the active frame on
 * the Vectrex every refresh through vectrexInterface (v_WaitRecal +
 * v_directDraw32) — a vector display has no persistence, so each frame is held
 * by re-drawing until the next one arrives.
 *
 * The protocol decode is identical to the host build; only the I/O (mini-UART in,
 * VIA out) is baremetal. Built by `make baremetal` with arm-none-eabi-gcc; see
 * docs/DEPLOY.md.
 */
#include <stdint.h>
#include <stdio.h> /* printf (baremetal: routed to the mini-UART via cstubs) */

#include <pitrex/pitrexio-gpio.h> /* vectrexinit() */
#include <rpi-aux.h>              /* RPI_AuxMiniUart* (mini-UART) */
#include <rpi-systimer.h>         /* RPI_GetSystemTimer (1 MHz, for the RX timeout) */
#include <vectrex/vectrexInterface.h>

#include "frame.h"
#include "protocol.h"
#include "uart_rx.h" /* interrupt-driven RX ring buffer */

/* Serial line rate of the incoming protocol. Must match the sender. The
 * mini-UART baud is core_clock / (8 * (divisor+1)) with the divisor TRUNCATED to
 * an integer, so VT_UART_CLOCK MUST equal the actual VPU/core clock AND the rate
 * must land the divisor on an integer or the real baud drifts off the target.
 *
 * Default 1,280,000 baud: exact from the config.txt-pinned core_freq=256 MHz
 * (256e6 / (8 * 25) = 1,280,000, divisor 24, 0% error) and reliable in practice
 * over a 3.3 V USB-TTL adapter. (2 Mbaud is also exact but proved marginal on
 * real wiring — bytes corrupt; the flow-control handshake keeps the link
 * lossless but can't fix raw bit errors, so a slightly slower rate is the fix.)
 * If you change the baud, pick a core_freq that keeps core/(8*baud) integral
 * (e.g. at 256 MHz: 2.0M, 1.6M, 1.28M, 1.0M, 800k, 640k, 500k are all exact);
 * -DVT_UART_BAUD=115200 is ~0.1% off and rock-solid. Keep VT_UART_CLOCK ==
 * config.txt's core_freq. (The boot banner is emitted separately at 921600.) */
#ifndef VT_UART_BAUD
#define VT_UART_BAUD 1280000
#endif
#ifndef VT_UART_CLOCK
#define VT_UART_CLOCK 256000000
#endif

/* Device coords 0..4095 map onto this Vectrex integrator range. The default
 * roughly fills the screen (the pitrex "hello" example draws to +/-10000);
 * calibrate per display with -DVT_VECTREX_MIN/MAX. */
#ifndef VT_VECTREX_MIN
#define VT_VECTREX_MIN (-10000)
#endif
#ifndef VT_VECTREX_MAX
#define VT_VECTREX_MAX 10000
#endif

/* Beam intensity (0..255) is shifted onto the Vectrex z-axis (0..127). */
#ifndef VT_BRIGHT_SHIFT
#define VT_BRIGHT_SHIFT 1
#endif

/* Target refresh rate handed to the pitrex pacing (Vectrex is ~50 Hz). */
#ifndef VT_REFRESH_HZ
#define VT_REFRESH_HZ 50
#endif

/* Idle splash brightness (Vectrex z-axis, 0..127). */
#ifndef VT_SPLASH_BRIGHT
#define VT_SPLASH_BRIGHT 0x50
#endif

/* Build identity shown on the splash. The Makefile injects the git tag/describe
 * and short commit; off-target builds (the emulator) fall back to placeholders. */
#ifndef VT_GIT_VERSION
#define VT_GIT_VERSION "dev"
#endif
#ifndef VT_GIT_COMMIT
#define VT_GIT_COMMIT "unknown"
#endif

/* Global inactivity timeout: with no new frame *or* keepalive for this long, the
 * held frame is dropped and the receiver returns to the splash, so a stale image
 * never lingers after a sender disconnects. A sender holding a static image keeps
 * it alive cheaply with the CMD keepalive ping (see PROTOCOL-EXTENSIONS.md §11).
 * Microseconds, against the 1 MHz system timer. */
#ifndef VT_IDLE_TIMEOUT_US
#define VT_IDLE_TIMEOUT_US 30000000u /* 30 s */
#endif

/* Baud rates the operator can cycle through from the splash with a Vectrex
 * button, so a freshly flashed board can be matched to a sender without
 * re-flashing. All are exact (0% divisor error) at core_freq=256 MHz; see the
 * VT_UART_BAUD note above. The active rate is shown on the splash. */
#ifndef VT_BAUD_OPTIONS
#define VT_BAUD_OPTIONS {1280000u, 2000000u, 1000000u, 921600u, 500000u, 115200u}
#endif

/* Per-frame flow control. The receiver has no byte-level resync and the Pi
 * mini-UART has only an 8-byte FIFO, so the link must be lossless: vekterm sends
 * VT_SYNC_BYTE on its TX when it is ready, and the sender (pyvterm with flow
 * control on) transmits exactly one frame per sync. Nothing then arrives while
 * vekterm is drawing. VT_RX_TIMEOUT_US bounds the wait so the splash still draws
 * when no sender is connected. */
#ifndef VT_SYNC_BYTE
#define VT_SYNC_BYTE 0x06 /* ASCII ACK */
#endif

/* Once a sender has negotiated v2 (it sent the HELLO probe, so we know it is not
 * a plain USB-DVG), the per-frame "ready" reply is replaced *wholesale* by a
 * compact, fixed 5-byte timing record — no sync byte, no marker, no framing — so
 * the sender can adapt its frame rate to how long the last frame took to draw.
 * The record's arrival IS the readiness signal. This is NOT backward compatible
 * within v2 by design; cross-version safety comes from negotiation, exactly like
 * the EXT subtypes: a v1 (un-negotiated) peer keeps getting the plain
 * VT_SYNC_BYTE, so the base DVG flow control is unchanged. Layout (big-endian):
 *   [0..1] draw_us  u16   microseconds spent drawing the last frame
 *   [2..3] vectors  u16   vectors in the last frame
 *   [4]    flags    u8    bit0 overflow, bit1 idle (splash drawn) */
#define VT_SYNC_FLAG_OVERFLOW 0x01u
#define VT_SYNC_FLAG_IDLE 0x02u
#define VT_SYNC_V2_LEN 5
#ifndef VT_RX_TIMEOUT_US
#define VT_RX_TIMEOUT_US 50000u
#endif
/* Hard backstop on the receive wait, independent of the system timer, so the
 * draw loop can never hang (it just falls through to a redraw). */
#ifndef VT_RX_SPINS
#define VT_RX_SPINS 5000000u
#endif

/* One vector with coordinates already mapped to the Vectrex integrator range.
 * Mapping at COMPLETE (once) rather than every refresh keeps the per-vector
 * 64-bit multiply/divide out of the 50 Hz draw loop — see PROTOCOL-EXTENSIONS.md
 * §8. */
typedef struct {
    int16_t x0, y0, x1, y1;
    uint8_t z;
} draw_seg;

/* Active draw list: the parser fills it on COMPLETE, the draw loop renders it
 * every refresh. */
static draw_seg g_draw[VT_MAX_PIPELINE];
static int g_draw_count;
static int g_have_frame;
static int g_overflow;          /* last frame exceeded MAX_PIPELINE */
static volatile int g_activity; /* set by on_frame/on_keepalive: sender is alive */
static volatile int g_peer_v2;  /* set by on_query: the peer negotiated v2 */

static void on_frame(void *ctx, const vt_frame *frame)
{
    int i;
    (void)ctx;
    /* Map device coords (0..4095) onto the Vectrex range once, here, so the
     * refresh loop just feeds v_directDraw32 with no arithmetic per vector. */
    for (i = 0; i < frame->count; i++) {
        const vt_segment *s = &frame->segments[i];
        draw_seg *d = &g_draw[i];
        d->x0 = (int16_t)vt_map_coord(s->x0, VT_VECTREX_MIN, VT_VECTREX_MAX);
        d->y0 = (int16_t)vt_map_coord(s->y0, VT_VECTREX_MIN, VT_VECTREX_MAX);
        d->x1 = (int16_t)vt_map_coord(s->x1, VT_VECTREX_MIN, VT_VECTREX_MAX);
        d->y1 = (int16_t)vt_map_coord(s->y1, VT_VECTREX_MIN, VT_VECTREX_MAX);
        d->z = (uint8_t)(s->brightness >> VT_BRIGHT_SHIFT);
    }
    g_draw_count = frame->count;
    g_overflow = frame->overflow;
    g_have_frame = 1;
    g_activity = 1;
}

static void on_exit(void *ctx)
{
    /* Baremetal has no OS to return to; keep holding the last frame. */
    (void)ctx;
}

/* A keepalive ping carries no geometry: it just proves the sender is still there
 * so the idle timeout doesn't drop a (legitimately static) held frame. It breaks
 * the receive wait like a frame would, so the next sync goes out promptly. */
static void on_keepalive(void *ctx)
{
    (void)ctx;
    g_activity = 1;
}

/* Answer the HELLO capability probe on the mini-UART TX so a sender can detect a
 * v2 vekterm and switch to the compact EXT subtypes. */
static void on_query(void *ctx)
{
    uint8_t descriptor[VT_HELLO_LEN];
    int i;
    (void)ctx;
    /* A HELLO probe means the peer is a v2 sender: from now the per-frame reply
     * carries the compact timing record instead of the bare sync byte. */
    g_peer_v2 = 1;
    vt_encode_hello_descriptor(descriptor);
    for (i = 0; i < VT_HELLO_LEN; i++) {
        RPI_AuxMiniUartWrite(descriptor[i]);
    }
}

/* --- runtime text helpers (the splash now shows the live baud + git build) -- */

/* Append v's decimal digits to buf at *pos (caller guarantees room, then NUL). */
static void append_u32(char *buf, int *pos, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) {
        buf[(*pos)++] = '0';
        return;
    }
    while (v > 0 && n < (int)sizeof tmp) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        buf[(*pos)++] = tmp[--n];
    }
}

/* Append s to buf at *pos, upper-casing ASCII and stopping before cap-1 (the
 * Vectrex BIOS font is uppercase-only; git output carries lowercase hex). */
static void append_upper(char *buf, int *pos, const char *s, int cap)
{
    while (*s != '\0' && *pos < cap - 1) {
        char c = *s++;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        buf[(*pos)++] = c;
    }
}

/* Cycleable baud table and the active rate (shown on the splash, changed by a
 * button press while idle — see handle_splash_buttons). */
static const uint32_t g_baud_options[] = VT_BAUD_OPTIONS;
#define VT_BAUD_COUNT ((int)(sizeof g_baud_options / sizeof g_baud_options[0]))
static int g_baud_idx;
static uint32_t g_cur_baud = VT_UART_BAUD;

/* Until the first frame arrives (e.g. nothing is connected to the UART yet),
 * draw a splash so the operator can see the receiver booted, which build is
 * running, the active line rate, and that a button changes it — a blank screen
 * is indistinguishable from a dead board. The Vectrex BIOS font is
 * uppercase-only; coords are 8-bit, centred at 0,0 with +y up.
 * v_printString takes (x, y, string, textSize, brightness); it draws ~180 units
 * per glyph at textSize and positions at x*128. Tune the sizes with
 * -DVT_SPLASH_*_SIZE if your display differs. */
#ifndef VT_SPLASH_TITLE_SIZE
#define VT_SPLASH_TITLE_SIZE 6
#endif
#ifndef VT_SPLASH_TEXT_SIZE
#define VT_SPLASH_TEXT_SIZE 3
#endif
#ifndef VT_SPLASH_VERSION_SIZE
#define VT_SPLASH_VERSION_SIZE 2
#endif
#ifndef VT_SPLASH_HINT_SIZE
#define VT_SPLASH_HINT_SIZE 2
#endif

static void draw_idle_splash(void)
{
    char line[40];
    int pos;

    v_printString(-30, 56, "VEKTERM", VT_SPLASH_TITLE_SIZE, VT_SPLASH_BRIGHT);

    pos = 0;
    append_upper(line, &pos, VT_GIT_VERSION " ", (int)sizeof line);
    append_upper(line, &pos, VT_GIT_COMMIT, (int)sizeof line);
    line[pos] = '\0';
    v_printString(-50, 28, line, VT_SPLASH_VERSION_SIZE, VT_SPLASH_BRIGHT);

    v_printString(-50, 2, "WAITING FOR DATA", VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);

    pos = 0;
    append_u32(line, &pos, g_cur_baud);
    append_upper(line, &pos, " BAUD 8N1", (int)sizeof line);
    line[pos] = '\0';
    v_printString(-50, -24, line, VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);

    v_printString(-50, -52, "BTN: CYCLE BAUD", VT_SPLASH_HINT_SIZE, VT_SPLASH_BRIGHT);
}

static void draw_active_frame(void)
{
    int i;
    for (i = 0; i < g_draw_count; i++) {
        const draw_seg *d = &g_draw[i];
        v_directDraw32(d->x0, d->y0, d->x1, d->y1, d->z);
    }
}

/* Switch the mini-UART (and the lossless RX ring) to a new line rate and start
 * the word parser clean, so the splash baud cycler can match a sender's rate
 * without re-flashing. */
static void set_baud(vt_parser *parser, uint32_t baud)
{
    g_cur_baud = baud;
    RPI_AuxMiniUartInit((int)baud, 8, VT_UART_CLOCK);
    uart_rx_init();
    uart_rx_flush();
    vt_parser_resync(parser);
}

/* On the splash, any Vectrex button press cycles to the next baud in the table.
 * Edge-detected (rising edges only) so one press steps once. No-op while a frame
 * is held, so a button never disturbs an active stream. */
static void handle_splash_buttons(vt_parser *parser)
{
    static uint8_t prev;
    uint8_t now = v_readButtons();
    uint8_t pressed = (uint8_t)(now & ~prev);
    prev = now;
    if (pressed != 0) {
        g_baud_idx = (g_baud_idx + 1) % VT_BAUD_COUNT;
        set_baud(parser, g_baud_options[g_baud_idx]);
    }
}

/* Signal "ready for the next frame". A v1 (un-negotiated) peer gets the plain
 * base-protocol sync byte. A negotiated v2 peer gets the compact fixed timing
 * record instead (its arrival is the readiness signal), so it can adapt its
 * frame rate to how long the receiver took to draw — see VT_SYNC_V2_LEN. */
static void send_sync_reply(uint32_t draw_us, int vectors, uint8_t flags)
{
    uint8_t rec[VT_SYNC_V2_LEN];
    uint32_t us, vc;
    int i;

    if (!g_peer_v2) {
        RPI_AuxMiniUartWrite((char)VT_SYNC_BYTE); /* base DVG flow control */
        return;
    }
    us = draw_us > 0xFFFFu ? 0xFFFFu : draw_us;
    vc = vectors < 0 ? 0u : ((uint32_t)vectors > 0xFFFFu ? 0xFFFFu : (uint32_t)vectors);
    rec[0] = (uint8_t)(us >> 8);
    rec[1] = (uint8_t)(us & 0xFFu);
    rec[2] = (uint8_t)(vc >> 8);
    rec[3] = (uint8_t)(vc & 0xFFu);
    rec[4] = flags;
    for (i = 0; i < VT_SYNC_V2_LEN; i++) {
        RPI_AuxMiniUartWrite((char)rec[i]);
    }
}

int main(int argc, char **argv)
{
    vt_parser parser;
    vt_sink sink;
    uint32_t last_activity;
    int i;

    (void)argc;
    (void)argv;

    /* Bring up the PiTrex/VIA and the Vectrex interface. */
    vectrexinit(1);
    v_setName("vekterm");
    v_init();
    v_setRefresh(VT_REFRESH_HZ);

    /* Start the baud cycler at the compile-time default if it is in the table. */
    g_cur_baud = VT_UART_BAUD;
    for (i = 0; i < VT_BAUD_COUNT; i++) {
        if (g_baud_options[i] == VT_UART_BAUD) {
            g_baud_idx = i;
            break;
        }
    }

    /* Last line emitted at the boot-banner baud (921600), before the link
     * switches to the protocol rate. Seeing this on the console confirms init
     * completed and the draw loop is about to run (the splash is being drawn) —
     * useful for a bench test with no Vectrex or sender attached. */
    printf("vekterm: init complete; entering draw loop, link -> %u baud\r\n", g_cur_baud);

    /* kernelMain brought the mini-UART up at 921600 for the banner; switch it
     * to the protocol's line rate for incoming vector data. */
    RPI_AuxMiniUartInit((int)g_cur_baud, 8, VT_UART_CLOCK);
    uart_rx_init(); /* interrupt-driven, lossless RX into a ring buffer */

    sink.ctx = NULL;
    sink.on_frame = on_frame;
    sink.on_exit = on_exit;
    sink.on_query = on_query;
    sink.on_keepalive = on_keepalive;
    vt_parser_init(&parser, sink);

    /* Start each frame from a clean, byte-aligned state: the handshake means the
     * sender hasn't transmitted yet, so flushing now drops any line-noise from
     * the 921600->2M switch and re-aligns the word parser. */
    uart_rx_flush();
    vt_parser_resync(&parser);
    last_activity = RPI_GetSystemTimer()->counter_lo;
    send_sync_reply(0, 0, VT_SYNC_FLAG_IDLE); /* "ready" for the first frame */
    for (;;) {
        uint32_t now, draw_t0, draw_us;
        uint8_t flags;

        /* Receive exactly one frame (or a keepalive ping), then draw. With flow
         * control the sender only transmits after our sync byte, so the draw
         * never competes with the UART. Time out so the idle splash still
         * refreshes, and so the inactivity timeout below is evaluated, with no
         * sender connected. */
        uint32_t t0 = RPI_GetSystemTimer()->counter_lo;
        uint32_t spins = VT_RX_SPINS;
        g_activity = 0;
        while (!g_activity && --spins &&
               (uint32_t)(RPI_GetSystemTimer()->counter_lo - t0) < VT_RX_TIMEOUT_US) {
            int b;
            uart_rx_poll(); /* drain the UART FIFO into the ring buffer */
            while (!g_activity && (b = uart_rx_get()) >= 0) {
                uint8_t byte = (uint8_t)b;
                vt_parser_feed(&parser, &byte, 1);
            }
        }

        /* A new frame or a keepalive both count as the sender being alive. With
         * neither for VT_IDLE_TIMEOUT_US, drop the held frame and fall back to
         * the splash so a stale image never lingers after a disconnect. */
        now = RPI_GetSystemTimer()->counter_lo;
        if (g_activity) {
            last_activity = now;
        } else if (g_have_frame && (uint32_t)(now - last_activity) >= VT_IDLE_TIMEOUT_US) {
            g_have_frame = 0;
            g_draw_count = 0;
            g_overflow = 0;
        }

        v_WaitRecal();
        if (g_have_frame) {
            draw_t0 = RPI_GetSystemTimer()->counter_lo;
            draw_active_frame();
            draw_us = (uint32_t)(RPI_GetSystemTimer()->counter_lo - draw_t0);
        } else {
            /* Idle: the splash is interactive — a button cycles the baud. */
            handle_splash_buttons(&parser);
            draw_t0 = RPI_GetSystemTimer()->counter_lo;
            draw_idle_splash();
            draw_us = (uint32_t)(RPI_GetSystemTimer()->counter_lo - draw_t0);
        }

        /* Frame boundary: discard leftovers + re-align before asking for the
         * next frame, so a one-off byte slip can't desync the stream forever. */
        uart_rx_flush();
        vt_parser_resync(&parser);
        flags = (uint8_t)((g_overflow ? VT_SYNC_FLAG_OVERFLOW : 0u) |
                          (g_have_frame ? 0u : VT_SYNC_FLAG_IDLE));
        send_sync_reply(draw_us, g_draw_count, flags); /* ready for the next frame */
    }
    return 0;
}
