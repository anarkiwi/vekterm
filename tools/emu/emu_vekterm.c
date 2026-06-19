/*
 * emu_vekterm.c — full end-to-end emulation: REAL pyvterm -> PTY -> REAL vekterm
 * main loop -> software 6522 VIA + Vectrex -> rendered image.
 *
 * This links the actual src/vekterm_baremetal.c main() (renamed vekterm_main)
 * and runs it unmodified: the flow-control handshake, the UART receive, the
 * flush/resync, the real protocol parser, and the real libpitrex draw path — all
 * of it. The only substitutions are the lowest-level host shims: the mini-UART
 * and uart_rx read/write a PTY instead of hardware, the "system timer" reads the
 * host clock, and the VIA writes feed the model in via_vectrex.c.
 *
 * On the other end of the PTY we fork *real* pyvterm (examples/testpattern.py
 * flow control on), so the whole pipeline — pyvterm's encoder, its
 * SerialTransport handshake, the wire, vekterm's receive/parse/draw — is
 * exercised exactly as on hardware, minus the 2 Mbaud electrical layer. If the
 * pattern renders here, the logic is proven correct.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <rpi-systimer.h>
#include "uart_rx.h"
#include "via_vectrex.h"

int vekterm_main(int argc, char **argv); /* src/vekterm_baremetal.c (-Dmain=) */

/* ---- the emulated serial wire (PTY master fd) -------------------------- */
static int g_uart_fd = -1;

/* ---- captured lit beam segments (shared: vekterm thread writes) -------- */
typedef struct { double x0, y0, x1, y1; int z; } Seg;
static Seg *segs;
static int nsegs, capsegs;
static pthread_mutex_t seg_lock = PTHREAD_MUTEX_INITIALIZER;

static void on_line(void *ctx, double x0, double y0, double x1, double y1, int z)
{
    (void)ctx;
    pthread_mutex_lock(&seg_lock);
    if (nsegs == capsegs) {
        capsegs = capsegs ? capsegs * 2 : 8192;
        segs = realloc(segs, (size_t)capsegs * sizeof *segs);
    }
    segs[nsegs++] = (Seg){ x0, y0, x1, y1, z };
    pthread_mutex_unlock(&seg_lock);
}

/* ---- host shims the real vekterm main loop links against --------------- */
volatile uint32_t *bcm2835_peripherals = (volatile uint32_t *)-1;
volatile uint32_t *bcm2835_st = (volatile uint32_t *)-1;

void RPI_AuxMiniUartInit(int baud, int bits, int mhz) { (void)baud; (void)bits; (void)mhz; }
void RPI_AuxMiniUartWrite(char c)
{
    if (g_uart_fd >= 0)
        (void)!write(g_uart_fd, &c, 1); /* the receiver's "ready" sync byte */
}
int RPI_AuxMiniUartReadPending(void)
{
    struct pollfd p = { g_uart_fd, POLLIN, 0 };
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}
char RPI_AuxMiniUartRead(void)
{
    char c = 0;
    if (g_uart_fd >= 0)
        (void)!read(g_uart_fd, &c, 1);
    return c;
}

static rpi_sys_timer_t g_systimer;
rpi_sys_timer_t *RPI_GetSystemTimer(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_systimer.counter_lo = (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
    return &g_systimer;
}

/* host uart_rx: drain the PTY into a ring buffer (the real ISR/FIFO has no
 * meaning here). Same API the real src/uart_rx.c exposes. */
#define RB 65536u
static uint8_t rb[RB];
static volatile unsigned rh, rt;
void uart_rx_init(void) { rh = rt = 0; }
void uart_rx_poll(void)
{
    uint8_t buf[1024];
    int n;
    while ((n = (int)read(g_uart_fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            unsigned nx = (rh + 1u) % RB;
            if (nx != rt) { rb[rh] = buf[i]; rh = nx; }
        }
    }
}
void uart_rx_flush(void) { uart_rx_poll(); rt = rh; }
int uart_rx_pending(void) { return rh != rt; }
int uart_rx_get(void)
{
    if (rh == rt) return -1;
    uint8_t c = rb[rt];
    rt = (rt + 1u) % RB;
    return (int)c;
}
uint32_t uart_rx_overruns(void) { return 0; }

static void *vekterm_thread(void *arg)
{
    (void)arg;
    vekterm_main(0, NULL); /* the real main loop; runs until the process exits */
    return NULL;
}

/* ---- minimal PPM render (white-on-black, fit content) ------------------ */
static void render(const char *path)
{
    pthread_mutex_lock(&seg_lock);
    int n = nsegs;
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (int i = 0; i < n; i++) {
        double xs[2] = { segs[i].x0, segs[i].x1 }, ys[2] = { segs[i].y0, segs[i].y1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k]; if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k]; if (ys[k] > maxy) maxy = ys[k];
        }
    }
    fprintf(stderr, "captured %d lit segments; bbox x[%.0f..%.0f] y[%.0f..%.0f]\n",
            n, minx, maxx, miny, maxy);
    if (n == 0) { pthread_mutex_unlock(&seg_lock); return; }
    double mx = (maxx - minx) * 0.08 + 200, my = (maxy - miny) * 0.08 + 200;
    double ex0 = minx - mx, ey0 = miny - my, ww = (maxx + mx) - ex0, wh = (maxy + my) - ey0;
    int W, H, maxd = 900;
    if (ww >= wh) { W = maxd; H = (int)(maxd * wh / ww); } else { H = maxd; W = (int)(maxd * ww / wh); }
    if (W < 1) W = 1; if (H < 1) H = 1;
    double sc = (W - 1) / ww;
    uint8_t *fb = calloc((size_t)W * H, 1);
    for (int i = 0; i < n; i++) {
        int x0 = (int)((segs[i].x0 - ex0) * sc), y0 = (int)(H - 1 - (segs[i].y0 - ey0) * sc);
        int x1 = (int)((segs[i].x1 - ex0) * sc), y1 = (int)(H - 1 - (segs[i].y1 - ey0) * sc);
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
        for (;;) {
            if (x0 >= 0 && y0 >= 0 && x0 < W && y0 < H) fb[y0 * W + x0] = 255;
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * e;
            if (e2 >= dy) { e += dy; x0 += sx; }
            if (e2 <= dx) { e += dx; y0 += sy; }
        }
    }
    pthread_mutex_unlock(&seg_lock);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { uint8_t g = fb[i]; uint8_t px[3] = { g, g, g }; fwrite(px, 1, 3, f); }
    fclose(f);
    free(fb);
    fprintf(stderr, "wrote %s (%dx%d)\n", path, W, H);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "out-emu/e2e.ppm";
    const char *pyvterm_dir = argc > 2 ? argv[2] : "../pyvterm";

    int master, slave;
    char slavename[256];
    if (openpty(&master, &slave, slavename, NULL, NULL) != 0) { perror("openpty"); return 1; }
    fcntl(master, F_SETFL, O_NONBLOCK);
    g_uart_fd = master;

    VxSink sink = { on_line };
    vx_reset(sink, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        close(master);
        if (chdir(pyvterm_dir) != 0) { perror("chdir pyvterm"); _exit(127); }
        setenv("PYTHONPATH", "src", 1);
        /* pyvterm flow control is on by default; the PTY makes it detect the
         * receiver's ready byte and stay in handshake mode. */
        execlp("python3", "python3", "examples/testpattern.py", "--port", slavename,
               "--frames", "40", "--intensity", "15",
               "--vectors", "4", "--fps", "120", (char *)NULL);
        perror("exec pyvterm");
        _exit(127);
    }
    close(slave);

    pthread_t th;
    pthread_create(&th, NULL, vekterm_thread, NULL);

    int status = 0;
    waitpid(pid, &status, 0); /* pyvterm exits after sending its 40 frames */

    /* Drop everything captured so far (the startup splash + streamed frames),
     * then capture a fresh steady state: vekterm keeps re-drawing the LAST
     * received frame, so what we render now is purely the streamed pattern. */
    pthread_mutex_lock(&seg_lock);
    nsegs = 0;
    pthread_mutex_unlock(&seg_lock);
    usleep(200000);

    render(out);
    fprintf(stderr, "pyvterm exit status: %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    exit(0); /* tears down the vekterm thread */
}
