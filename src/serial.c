/* POSIX termios is hidden under -std=c99 without a feature-test macro. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1

#include "serial.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Map a baud rate to its termios constant.  `*exact` is cleared when the rate
 * has no constant on this platform and a fallback is used. */
static speed_t baud_constant(int baud, int *exact)
{
    *exact = 1;
    switch (baud) {
#ifdef B3000000
    case 3000000:
        return B3000000;
#endif
#ifdef B2000000
    case 2000000:
        return B2000000;
#endif
#ifdef B1500000
    case 1500000:
        return B1500000;
#endif
#ifdef B1000000
    case 1000000:
        return B1000000;
#endif
#ifdef B921600
    case 921600:
        return B921600;
#endif
#ifdef B460800
    case 460800:
        return B460800;
#endif
#ifdef B230400
    case 230400:
        return B230400;
#endif
    case 115200:
        return B115200;
    case 57600:
        return B57600;
    case 38400:
        return B38400;
    case 19200:
        return B19200;
    case 9600:
        return B9600;
    default:
        break;
    }
    *exact = 0;
    return B115200;
}

int vt_serial_open(const char *port, int baud)
{
    struct termios tio;
    speed_t spd;
    int exact;
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "vekterm: open %s: %s\n", port, strerror(errno));
        return -1;
    }

    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "vekterm: tcgetattr %s: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Raw mode (equivalent to cfmakeraw, but using only POSIX flags). */
    tio.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON |
                                IXOFF | IXANY);
    tio.c_oflag &= (tcflag_t)~OPOST;
    tio.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= (tcflag_t) ~(CSIZE | PARENB | CSTOPB);
    tio.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
#ifdef CRTSCTS
    tio.c_cflag &= (tcflag_t)~CRTSCTS; /* no hardware flow control */
#endif
    tio.c_cc[VMIN] = 0; /* fully non-blocking: return whatever is available */
    tio.c_cc[VTIME] = 0;

    spd = baud_constant(baud, &exact);
    if (!exact) {
        fprintf(stderr,
                "vekterm: baud %d has no termios constant here; using 115200 "
                "(USB-CDC ignores the line rate)\n",
                baud);
    }
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "vekterm: tcsetattr %s: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}
