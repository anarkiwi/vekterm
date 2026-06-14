/*
 * Serial-port input for the receiver.
 *
 * Opens the device the sender writes to (the Pi's USB-CDC gadget /dev/ttyGS0,
 * or a UART such as /dev/serial0) configured the way the protocol expects:
 * raw, 8 data bits, no parity, 1 stop bit, no flow control, non-blocking reads.
 */
#ifndef VEKTERM_SERIAL_H
#define VEKTERM_SERIAL_H

/*
 * Open `port` at `baud` and return a readable file descriptor (>= 0), or -1 on
 * error (a message is printed to stderr).  USB-CDC devices ignore the line
 * rate; an unsupported `baud` falls back to a safe value with a warning.
 */
int vt_serial_open(const char *port, int baud);

#endif /* VEKTERM_SERIAL_H */
