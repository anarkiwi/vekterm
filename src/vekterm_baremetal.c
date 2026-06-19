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

/* Stringize the configured baud so the splash can show the line settings an
 * operator must match on the sender, without any runtime int formatting. */
#define VT_STR2(x) #x
#define VT_STR(x) VT_STR2(x)
#define VT_SPLASH_BAUD_LINE VT_STR(VT_UART_BAUD) " BAUD 8N1"

/* Per-frame flow control. The receiver has no byte-level resync and the Pi
 * mini-UART has only an 8-byte FIFO, so the link must be lossless: vekterm sends
 * VT_SYNC_BYTE on its TX when it is ready, and the sender (pyvterm with flow
 * control on) transmits exactly one frame per sync. Nothing then arrives while
 * vekterm is drawing. VT_RX_TIMEOUT_US bounds the wait so the splash still draws
 * when no sender is connected. */
#ifndef VT_SYNC_BYTE
#define VT_SYNC_BYTE 0x06 /* ASCII ACK */
#endif
#ifndef VT_RX_TIMEOUT_US
#define VT_RX_TIMEOUT_US 50000u
#endif
/* Hard backstop on the receive wait, independent of the system timer, so the
 * draw loop can never hang (it just falls through to a redraw). */
#ifndef VT_RX_SPINS
#define VT_RX_SPINS 5000000u
#endif

/* Double-buffered active frame: the parser fills it on COMPLETE, the draw loop
 * renders it every refresh. */
static vt_frame g_active;
static int g_have_frame;
static volatile int g_new_frame; /* set by on_frame when a COMPLETE arrives */

static void on_frame(void *ctx, const vt_frame *frame)
{
    (void)ctx;
    g_active = *frame;
    g_have_frame = 1;
    g_new_frame = 1;
}

static void on_exit(void *ctx)
{
    /* Baremetal has no OS to return to; keep holding the last frame. */
    (void)ctx;
}

/* Until the first frame arrives (e.g. nothing is connected to the UART yet),
 * draw a static splash so the operator can see the receiver booted and is
 * waiting — a blank screen is indistinguishable from a dead board. The Vectrex
 * BIOS font is uppercase-only; coords are 8-bit, centred at 0,0 with +y up.
 * v_printString takes (x, y, string, textSize, brightness). */
/* v_printString draws the Vectrex BIOS font at textSize*180-ish units per glyph
 * and positions at x*128; the default textSize 1-2 is ~2% of the screen, far too
 * small. These sizes fill the screen without overflowing the 16-char lines.
 * Tune with -DVT_SPLASH_TITLE_SIZE / -DVT_SPLASH_TEXT_SIZE if your display differs. */
#ifndef VT_SPLASH_TITLE_SIZE
#define VT_SPLASH_TITLE_SIZE 8
#endif
#ifndef VT_SPLASH_TEXT_SIZE
#define VT_SPLASH_TEXT_SIZE 4
#endif

static void draw_idle_splash(void)
{
    v_printString(-24, 30, "VEKTERM", VT_SPLASH_TITLE_SIZE, VT_SPLASH_BRIGHT);
    v_printString(-40, -5, "WAITING FOR DATA", VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);
    v_printString(-40, -40, VT_SPLASH_BAUD_LINE, VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);
}

static void draw_active_frame(void)
{
    int i;
    for (i = 0; i < g_active.count; i++) {
        const vt_segment *s = &g_active.segments[i];
        int32_t x0 = vt_map_coord(s->x0, VT_VECTREX_MIN, VT_VECTREX_MAX);
        int32_t y0 = vt_map_coord(s->y0, VT_VECTREX_MIN, VT_VECTREX_MAX);
        int32_t x1 = vt_map_coord(s->x1, VT_VECTREX_MIN, VT_VECTREX_MAX);
        int32_t y1 = vt_map_coord(s->y1, VT_VECTREX_MIN, VT_VECTREX_MAX);
        uint8_t z = (uint8_t)(s->brightness >> VT_BRIGHT_SHIFT);
        v_directDraw32(x0, y0, x1, y1, z);
    }
}

int main(int argc, char **argv)
{
    vt_parser parser;
    vt_sink sink;

    (void)argc;
    (void)argv;

    /* Bring up the PiTrex/VIA and the Vectrex interface. */
    vectrexinit(1);
    v_setName("vekterm");
    v_init();
    v_setRefresh(VT_REFRESH_HZ);

    /* Last line emitted at the boot-banner baud (921600), before the link
     * switches to the protocol rate. Seeing this on the console confirms init
     * completed and the draw loop is about to run (the splash is being drawn) —
     * useful for a bench test with no Vectrex or sender attached. */
    printf("vekterm: init complete; entering draw loop, link -> %d baud\r\n", VT_UART_BAUD);

    /* kernelMain brought the mini-UART up at 921600 for the banner; switch it
     * to the protocol's line rate for incoming vector data. */
    RPI_AuxMiniUartInit(VT_UART_BAUD, 8, VT_UART_CLOCK);
    uart_rx_init(); /* interrupt-driven, lossless RX into a ring buffer */

    sink.ctx = NULL;
    sink.on_frame = on_frame;
    sink.on_exit = on_exit;
    vt_parser_init(&parser, sink);

    /* Start each frame from a clean, byte-aligned state: the handshake means the
     * sender hasn't transmitted yet, so flushing now drops any line-noise from
     * the 921600->2M switch and re-aligns the word parser. */
    uart_rx_flush();
    vt_parser_resync(&parser);
    RPI_AuxMiniUartWrite(VT_SYNC_BYTE); /* "ready" for the first frame */
    for (;;) {
        /* Receive exactly one frame, then draw. With flow control the sender
         * only transmits after our sync byte, so the draw never competes with
         * the UART. Time out so the idle splash still refreshes with no sender. */
        uint32_t t0 = RPI_GetSystemTimer()->counter_lo;
        uint32_t spins = VT_RX_SPINS;
        g_new_frame = 0;
        while (!g_new_frame && --spins &&
               (uint32_t)(RPI_GetSystemTimer()->counter_lo - t0) < VT_RX_TIMEOUT_US) {
            int b;
            uart_rx_poll(); /* drain the UART FIFO into the ring buffer */
            while (!g_new_frame && (b = uart_rx_get()) >= 0) {
                uint8_t byte = (uint8_t)b;
                vt_parser_feed(&parser, &byte, 1);
            }
        }

        v_WaitRecal();
        if (g_have_frame) {
            draw_active_frame();
        } else {
            draw_idle_splash();
        }

        /* Frame boundary: discard leftovers + re-align before asking for the
         * next frame, so a one-off byte slip can't desync the stream forever. */
        uart_rx_flush();
        vt_parser_resync(&parser);
        RPI_AuxMiniUartWrite(VT_SYNC_BYTE); /* consumed; ready for the next frame */
    }
    return 0;
}
