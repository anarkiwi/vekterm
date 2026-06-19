/*
 * emu_main.c — driver for the off-target vekterm emulator.
 *
 * It boots vekterm's *real* draw path against the software 6522 VIA + Vectrex
 * vector generator in via_vectrex.c, captures every lit beam segment, and
 * renders the reconstructed Vectrex screen to a PPM image. Two modes:
 *
 *   --splash            draw the idle "VEKTERM / WAITING FOR DATA" splash that
 *                       vekterm shows before the first frame arrives.
 *   --frame <file.bin>  decode a USB-DVG/vecterm byte stream (the real
 *                       protocol.c/frame.c parser) and draw the frame.
 *
 * The drawing itself — v_init(), v_printString(), v_directDraw32(), the VIA
 * register macros, the vector font — is libpitrex's unmodified code; only the
 * hardware underneath it is the model. This is the proof that vekterm boots to
 * a splash and draws frames, with no Pi, no QEMU and no Vectrex.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "via_vectrex.h"
#include "frame.h"
#include "protocol.h"

/* libpitrex draw API (declared here to avoid pulling the VIA macro header). */
void v_init(void);
void v_setName(char *name);
void v_setRefresh(int hz);
void v_WaitRecal(void);
void v_printString(int8_t x, int8_t y, char *string, uint8_t textSize, uint8_t brightness);
void v_directDraw32(int32_t xStart, int32_t yStart, int32_t xEnd, int32_t yEnd, uint8_t brightness);
extern int usePipeline;

/* Globals referenced by vectrexInterface's timer helpers. On the Pi these point
 * at the BCM system timer; off-target there is none, so we hand them the
 * MAP_FAILED sentinel ((void*)-1) that setMarkStart()/waitMarkEnd() already
 * check for and skip (their delays are pure timing, irrelevant to geometry). */
volatile uint32_t *bcm2835_peripherals = (volatile uint32_t *)-1;
volatile uint32_t *bcm2835_st = (volatile uint32_t *)-1;

/* ---- vekterm_baremetal.c's draw constants (kept in sync with it) -------- */
#define VT_SPLASH_BRIGHT 0x50
#ifndef VT_UART_BAUD
#define VT_UART_BAUD 2000000
#endif
#define VT_VECTREX_MIN (-10000)
#define VT_VECTREX_MAX 10000
#define VT_BRIGHT_SHIFT 1
#define VT_STR2(x) #x
#define VT_STR(x) VT_STR2(x)
#define VT_SPLASH_BAUD_LINE VT_STR(VT_UART_BAUD) " BAUD 8N1"

/* ---- captured segments -------------------------------------------------- */
typedef struct { double x0, y0, x1, y1; int z; } Seg;
static Seg *segs;
static int nsegs, capsegs;

static void on_line(void *ctx, double x0, double y0, double x1, double y1, int z)
{
    (void)ctx;
    if (nsegs == capsegs) {
        capsegs = capsegs ? capsegs * 2 : 4096;
        segs = realloc(segs, (size_t)capsegs * sizeof *segs);
    }
    segs[nsegs++] = (Seg){ x0, y0, x1, y1, z };
}

/* ---- framebuffer (8-bit phosphor intensity) ----------------------------- */
static uint8_t *fb;
static int W, H;
static double wx0, wy0, wscale; /* world->pixel: px = (x-wx0)*wscale         */

static void fb_init(int w, int h) { W = w; H = h; fb = calloc((size_t)W * H, 1); }

static void px_add(int x, int y, int v)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    int i = y * W + x, s = fb[i] + v;
    fb[i] = s > 255 ? 255 : (uint8_t)s;
}

/* additive line with a tiny glow, y flipped (Vectrex +y is up) */
static void draw_line(double X0, double Y0, double X1, double Y1, int z)
{
    int px0 = (int)((X0 - wx0) * wscale), py0 = (int)(H - 1 - (Y0 - wy0) * wscale);
    int px1 = (int)((X1 - wx0) * wscale), py1 = (int)(H - 1 - (Y1 - wy0) * wscale);
    int dx = abs(px1 - px0), dy = -abs(py1 - py0);
    int sx = px0 < px1 ? 1 : -1, sy = py0 < py1 ? 1 : -1, err = dx + dy;
    int core = 120 + z;             /* brightness of the beam core */
    if (core > 255) core = 255;
    for (;;) {
        px_add(px0, py0, core);
        px_add(px0 + 1, py0, z / 3); px_add(px0 - 1, py0, z / 3);
        px_add(px0, py0 + 1, z / 3); px_add(px0, py0 - 1, z / 3);
        if (px0 == px1 && py0 == py1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; px0 += sx; }
        if (e2 <= dx) { err += dx; py0 += sy; }
    }
}

static void render_ppm(const char *path, int fixed)
{
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (int i = 0; i < nsegs; i++) {
        double xs[2] = { segs[i].x0, segs[i].x1 }, ys[2] = { segs[i].y0, segs[i].y1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k]; if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k]; if (ys[k] > maxy) maxy = ys[k];
        }
    }
    fprintf(stderr, "segments=%d  bbox x[%.0f..%.0f] y[%.0f..%.0f]\n",
            nsegs, minx, maxx, miny, maxy);

    double ex0, ey0, ex1, ey1;
    if (fixed) {                    /* nominal Vectrex visible area (portrait) */
        ex0 = -13000; ex1 = 13000; ey0 = -16500; ey1 = 16500;
    } else {                        /* fit content with a margin */
        double mx = (maxx - minx) * 0.08 + 200, my = (maxy - miny) * 0.08 + 200;
        ex0 = minx - mx; ex1 = maxx + mx; ey0 = miny - my; ey1 = maxy + my;
    }
    double worldw = ex1 - ex0, worldh = ey1 - ey0;
    int maxdim = 900;
    if (worldw >= worldh) { W = maxdim; H = (int)(maxdim * worldh / worldw); }
    else { H = maxdim; W = (int)(maxdim * worldw / worldh); }
    if (W < 1) W = 1; if (H < 1) H = 1;
    wscale = (W - 1) / worldw;
    wx0 = ex0; wy0 = ey0;
    fb_init(W, H);

    for (int i = 0; i < nsegs; i++)
        draw_line(segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1, segs[i].z);

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint8_t g = fb[i];
        /* white phosphor with a faint warm tint */
        uint8_t rgb[3] = { g, g, (uint8_t)(g > 12 ? g - 12 : 0) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%dx%d)\n", path, W, H);
}

/* ---- modes -------------------------------------------------------------- */
#ifndef VT_SPLASH_TITLE_SIZE
#define VT_SPLASH_TITLE_SIZE 8
#endif
#ifndef VT_SPLASH_TEXT_SIZE
#define VT_SPLASH_TEXT_SIZE 4
#endif
static void draw_splash(void)
{
    /* mirrors vekterm_baremetal.c:draw_idle_splash() */
    v_printString(-24, 30, "VEKTERM", VT_SPLASH_TITLE_SIZE, VT_SPLASH_BRIGHT);
    v_printString(-40, -5, "WAITING FOR DATA", VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);
    v_printString(-40, -40, VT_SPLASH_BAUD_LINE, VT_SPLASH_TEXT_SIZE, VT_SPLASH_BRIGHT);
}

static vt_frame g_frame;
static int g_have;
static void on_frame(void *ctx, const vt_frame *fr) { (void)ctx; g_frame = *fr; g_have = 1; }
static void on_frame_exit(void *ctx) { (void)ctx; }

static void draw_frame_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    vt_parser parser;
    vt_sink sink = { NULL, on_frame, on_frame_exit };
    vt_parser_init(&parser, sink);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        vt_parser_feed(&parser, buf, n);
    fclose(f);
    if (!g_have) { fprintf(stderr, "no complete frame in %s\n", path); return; }
    /* mirrors vekterm_baremetal.c:draw_active_frame() */
    for (int i = 0; i < g_frame.count; i++) {
        const vt_segment *s = &g_frame.segments[i];
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
    const char *out = "screen.ppm";
    const char *framefile = NULL;
    int mode_splash = 0, fixed = 0, dump = 0, pipeline = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--splash")) mode_splash = 1;
        else if (!strcmp(argv[i], "--frame") && i + 1 < argc) framefile = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--fixed")) fixed = 1;
        else if (!strcmp(argv[i], "--dump")) dump = 1;
        else if (!strcmp(argv[i], "--pipeline")) pipeline = 1;
        else { fprintf(stderr, "usage: %s [--splash | --frame f.bin] [--out f.ppm] [--fixed] [--dump] [--pipeline]\n", argv[0]); return 2; }
    }
    if (!mode_splash && !framefile) mode_splash = 1;

    VxSink sink = { on_line };
    vx_reset(sink, NULL);

    /* bring up the real libpitrex draw state, then take the direct (non-
     * pipeline) draw path so each vector maps to a deterministic VIA sequence */
    vectrexinit(1);
    v_setName("vekterm");
    v_init();
    /* By default take the direct (non-pipeline) draw path: one vector -> one
     * deterministic VIA sequence. With --pipeline keep libpitrex's default
     * pipeline on and mirror vekterm_baremetal.c's loop (draw, then flush in
     * v_WaitRecal) so we exercise the *exact* code path the Pi runs. */
    if (!pipeline)
        usePipeline = 0;
    v_setRefresh(50);

    if (pipeline) {
        /* Build the frame's pipeline, then flush it through v_WaitRecal — and
         * capture only that flush, which is what reaches the Vectrex. */
        if (mode_splash) draw_splash();
        else draw_frame_file(framefile);
        nsegs = 0;
        v_WaitRecal();
    } else {
        if (mode_splash) draw_splash();
        else draw_frame_file(framefile);
    }

    vx_flush();
    if (dump) {
        for (int i = 0; i < nsegs; i++)
            printf("%.1f %.1f %.1f %.1f %d\n",
                   segs[i].x0, segs[i].y0, segs[i].x1, segs[i].y1, segs[i].z);
    }
    render_ppm(out, fixed);
    return 0;
}
