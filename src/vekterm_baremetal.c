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

#include <pitrex/pitrexio-gpio.h> /* vectrexinit() */
#include <rpi-aux.h>              /* RPI_AuxMiniUart* (mini-UART) */
#include <vectrex/vectrexInterface.h>

#include "frame.h"
#include "protocol.h"

/* Serial line rate of the incoming protocol. Must match the sender. The
 * mini-UART baud is the core clock / (8 * (divisor+1)); VT_UART_CLOCK MUST equal
 * the actual VPU/core clock (deploy/config.txt pins core_freq=250 MHz) or the
 * baud is wrong. NB: 2 Mbaud does NOT divide cleanly from 250 MHz
 * (250e6/(8*2e6) = 15.6 -> nearest divisor gives ~2.08 Mbaud, ~4% off), which is
 * marginal for a reliable data link. For streaming data prefer a rate that
 * divides cleanly, e.g. -DVT_UART_BAUD=1250000 (250e6/(8*25), exact) with the
 * sender set to match, or -DVT_UART_BAUD=115200 (250e6/(8*271), 0.1% off) for a
 * slow, rock-solid link. The boot banner at 115200 is well within tolerance. */
#ifndef VT_UART_BAUD
#define VT_UART_BAUD 2000000
#endif
#ifndef VT_UART_CLOCK
#define VT_UART_CLOCK 250000000
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

/* Cap bytes drained per refresh so a flood of input can't starve the display. */
#ifndef VT_DRAIN_BUDGET
#define VT_DRAIN_BUDGET 16384
#endif

/* Double-buffered active frame: the parser fills it on COMPLETE, the draw loop
 * renders it every refresh. */
static vt_frame g_active;
static int g_have_frame;

static void on_frame(void *ctx, const vt_frame *frame)
{
    (void)ctx;
    g_active = *frame;
    g_have_frame = 1;
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
static void draw_idle_splash(void)
{
    v_printString(-40, 40, "VEKTERM", 2, VT_SPLASH_BRIGHT);
    v_printString(-60, 5, "WAITING FOR DATA", 1, VT_SPLASH_BRIGHT);
    v_printString(-60, -25, VT_SPLASH_BAUD_LINE, 1, VT_SPLASH_BRIGHT);
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

    /* The framework brought the mini-UART up at 115200; switch it to the
     * protocol's line rate. */
    RPI_AuxMiniUartInit(VT_UART_BAUD, 8, VT_UART_CLOCK);

    sink.ctx = NULL;
    sink.on_frame = on_frame;
    sink.on_exit = on_exit;
    vt_parser_init(&parser, sink);

    for (;;) {
        int budget = VT_DRAIN_BUDGET;
        while (budget-- > 0 && RPI_AuxMiniUartReadPending()) {
            uint8_t byte = (uint8_t)RPI_AuxMiniUartRead();
            vt_parser_feed(&parser, &byte, 1);
        }
        v_WaitRecal();
        if (g_have_frame) {
            draw_active_frame();
        } else {
            draw_idle_splash();
        }
    }
    return 0;
}
