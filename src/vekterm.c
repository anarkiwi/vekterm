/*
 * vekterm — the USB-DVG / vecterm receiver for the PiTrex.
 *
 * It reads the wire protocol a sender (pyvterm or a custom MAME build) writes
 * over a serial link, decodes it into frames, and re-draws the active frame on
 * the Vectrex ~50 times a second through the selected backend.  With --dry-run
 * (or any build without libpitrex) it uses the stub backend, so the whole
 * pipeline runs and reports on an ordinary computer.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "frame.h"
#include "protocol.h"
#include "serial.h"

#define VEKTERM_VERSION "0.1.0"

#define DEFAULT_PORT "/dev/ttyGS0"
#define DEFAULT_BAUD 2000000
#define DEFAULT_VECTREX_MIN (-2048)
#define DEFAULT_VECTREX_MAX 2047
#define DEFAULT_BRIGHT_SHIFT 1

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    const vt_backend *backend;
    void *backend_ctx;
    unsigned long frames;
    bool exit_seen;
    int out_fd; /* where to write the HELLO reply, or -1 if the source is read-only */
} app_state;

static void app_on_frame(void *ctx, const vt_frame *frame)
{
    app_state *app = (app_state *)ctx;
    app->backend->set_frame(app->backend_ctx, frame);
    app->frames++;
}

static void app_on_exit(void *ctx)
{
    ((app_state *)ctx)->exit_seen = true;
}

/* Answer the HELLO capability probe by writing the descriptor back to the
 * source (a serial port).  Best-effort: a read-only file source has no return
 * channel, so out_fd is -1 and we skip it. */
static void app_on_query(void *ctx)
{
    app_state *app = (app_state *)ctx;
    uint8_t descriptor[VT_HELLO_LEN];
    if (app->out_fd < 0) {
        return;
    }
    vt_encode_hello_descriptor(descriptor);
    if (write(app->out_fd, descriptor, sizeof descriptor) < 0) {
        /* The far end may not be reading; detection is best-effort. */
    }
}

static void usage(const char *argv0)
{
    printf("vekterm %s — USB-DVG / vecterm receiver for the PiTrex\n\n", VEKTERM_VERSION);
    printf("Usage: %s [options]\n\n", argv0);
    printf("  -p, --port PORT      serial device to read (default %s)\n", DEFAULT_PORT);
    printf("  -b, --baud N         nominal baud rate (default %d)\n", DEFAULT_BAUD);
    printf("  -i, --input FILE     read frames from FILE ('-' = stdin) instead of serial\n");
    printf("  -n, --dry-run        decode and report only; never touch hardware\n");
    printf("  -1, --once           exit after the first completed frame\n");
    printf("  -s, --scale N        Vectrex scale register (pitrex backend)\n");
    printf("      --min N          device 0 maps to this Vectrex coord (default %d)\n",
           DEFAULT_VECTREX_MIN);
    printf("      --max N          device 4095 maps to this Vectrex coord (default %d)\n",
           DEFAULT_VECTREX_MAX);
    printf("      --bright-shift N beam intensity = brightness >> N (default %d)\n",
           DEFAULT_BRIGHT_SHIFT);
    printf("  -v, --verbose        print every decoded vector\n");
    printf("  -t, --selftest       run a built-in frame through the pipeline and exit\n");
    printf("  -h, --help           show this help\n");
    printf("      --version        print version and exit\n");
}

/* Parse an integer option value, exiting on a malformed argument. */
static int parse_int(const char *name, const char *value)
{
    char *end = NULL;
    long n;
    if (value == NULL) {
        fprintf(stderr, "vekterm: %s requires a value\n", name);
        exit(2);
    }
    errno = 0;
    n = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "vekterm: invalid integer for %s: %s\n", name, value);
        exit(2);
    }
    return (int)n;
}

static int run_selftest(const vt_options *opt)
{
    const vt_backend *be = &vt_backend_stub;
    void *ctx = be->init(opt);
    app_state app;
    vt_sink sink;
    vt_parser parser;
    size_t i;
    int ok;
    const uint32_t words[] = {
        vt_encode_frame(400),           vt_encode_rgb(0xF0, 0xF0, 0xF0),
        vt_encode_xy(2049, 2050, true), vt_encode_xy(2449, 2050, false),
        vt_encode_quality(5),           vt_encode_complete(false),
    };

    if (ctx == NULL) {
        fprintf(stderr, "vekterm: out of memory\n");
        return 1;
    }

    app.backend = be;
    app.backend_ctx = ctx;
    app.frames = 0;
    app.exit_seen = false;
    app.out_fd = -1;
    sink.ctx = &app;
    sink.on_frame = app_on_frame;
    sink.on_exit = app_on_exit;
    sink.on_query = app_on_query;
    vt_parser_init(&parser, sink);

    for (i = 0; i < sizeof words / sizeof words[0]; i++) {
        uint8_t bytes[4];
        vt_pack_be(words[i], bytes);
        vt_parser_feed(&parser, bytes, sizeof bytes);
    }
    be->shutdown(ctx);

    ok = (app.frames == 1 && parser.frames_done == 1);
    printf("selftest: %s (frames=%lu)\n", ok ? "PASS" : "FAIL", app.frames);
    return ok ? 0 : 1;
}

/* True for sources that signal EOF (regular files, pipes), false for a tty. */
static bool source_is_eofable(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode);
}

int main(int argc, char **argv)
{
    vt_options opt;
    const char *port = DEFAULT_PORT;
    const char *input = NULL;
    int baud = DEFAULT_BAUD;
    bool dry_run = false;
    bool once = false;
    bool selftest = false;
    const vt_backend *be;
    void *backend_ctx;
    app_state app;
    vt_sink sink;
    vt_parser parser;
    int fd;
    bool eofable;
    bool idle_sleep;
    uint8_t buf[4096];
    int i;

    opt.vectrex_min = DEFAULT_VECTREX_MIN;
    opt.vectrex_max = DEFAULT_VECTREX_MAX;
    opt.scale = 0;
    opt.bright_shift = DEFAULT_BRIGHT_SHIFT;
    opt.verbose = false;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-p") == 0 || strcmp(a, "--port") == 0) {
            port = (++i < argc) ? argv[i] : NULL;
            if (port == NULL) {
                fprintf(stderr, "vekterm: %s requires a value\n", a);
                return 2;
            }
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--baud") == 0) {
            baud = parse_int(a, (++i < argc) ? argv[i] : NULL);
        } else if (strcmp(a, "-i") == 0 || strcmp(a, "--input") == 0) {
            input = (++i < argc) ? argv[i] : NULL;
            if (input == NULL) {
                fprintf(stderr, "vekterm: %s requires a value\n", a);
                return 2;
            }
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(a, "-1") == 0 || strcmp(a, "--once") == 0) {
            once = true;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--scale") == 0) {
            opt.scale = parse_int(a, (++i < argc) ? argv[i] : NULL);
        } else if (strcmp(a, "--min") == 0) {
            opt.vectrex_min = parse_int(a, (++i < argc) ? argv[i] : NULL);
        } else if (strcmp(a, "--max") == 0) {
            opt.vectrex_max = parse_int(a, (++i < argc) ? argv[i] : NULL);
        } else if (strcmp(a, "--bright-shift") == 0) {
            opt.bright_shift = parse_int(a, (++i < argc) ? argv[i] : NULL);
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            opt.verbose = true;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--selftest") == 0) {
            selftest = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("vekterm %s\n", VEKTERM_VERSION);
            return 0;
        } else {
            fprintf(stderr, "vekterm: unknown option '%s' (try --help)\n", a);
            return 2;
        }
    }

    if (opt.bright_shift < 0 || opt.bright_shift > 8) {
        fprintf(stderr, "vekterm: --bright-shift must be 0..8\n");
        return 2;
    }

    if (selftest) {
        return run_selftest(&opt);
    }

    /* The host build only has the stub backend (it decodes and reports; it
     * never drives hardware). --dry-run is accepted for compatibility and is
     * implied here; the deployable receiver is the baremetal build. */
    (void)dry_run;
    be = &vt_backend_stub;

    /* Open the input source. */
    if (input != NULL) {
        if (strcmp(input, "-") == 0) {
            fd = STDIN_FILENO;
        } else {
            fd = open(input, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "vekterm: open %s: %s\n", input, strerror(errno));
                return 1;
            }
        }
    } else {
        fd = vt_serial_open(port, baud);
        if (fd < 0) {
            return 1;
        }
    }
    eofable = source_is_eofable(fd);

    backend_ctx = be->init(&opt);
    if (backend_ctx == NULL) {
        fprintf(stderr, "vekterm: backend init failed\n");
        if (fd > STDERR_FILENO) {
            close(fd);
        }
        return 1;
    }

    app.backend = be;
    app.backend_ctx = backend_ctx;
    app.frames = 0;
    app.exit_seen = false;
    /* Only a serial port (opened RDWR) has a return channel for the HELLO reply;
     * a file/stdin replay (--input) is read-only. */
    app.out_fd = (input == NULL) ? fd : -1;
    sink.ctx = &app;
    sink.on_frame = app_on_frame;
    sink.on_exit = app_on_exit;
    sink.on_query = app_on_query;
    vt_parser_init(&parser, sink);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "vekterm %s: backend=%s source=%s\n", VEKTERM_VERSION, be->name,
            input ? input : port);

    /* The stub does no timing of its own, so pace idle polling on a real port.
     * The pitrex backend blocks in v_WaitRecal(), which paces the loop for us. */
    idle_sleep = (be == &vt_backend_stub) && !eofable;

    while (!g_stop && !app.exit_seen) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            vt_parser_feed(&parser, buf, (size_t)n);
        } else if (n == 0) {
            if (eofable) {
                break; /* end of a file/pipe replay */
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fprintf(stderr, "vekterm: read: %s\n", strerror(errno));
            break;
        }

        be->refresh(backend_ctx);

        if (once && app.frames > 0) {
            break;
        }
        if (n <= 0 && idle_sleep) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L; /* 1 ms */
            nanosleep(&ts, NULL);
        }
    }

    be->shutdown(backend_ctx);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    fprintf(stderr, "vekterm: stopped after %lu frame(s)\n", app.frames);
    return 0;
}
