/*
 * uart_rx.h — interrupt-driven mini-UART receive for vekterm (baremetal only).
 *
 * The Pi mini-UART has only an 8-byte RX FIFO and vekterm spends most of each
 * frame drawing (not reading the UART), so at any real line rate the FIFO
 * overflows and bytes are lost. The streaming protocol has no byte-level resync,
 * so a single lost byte corrupts the whole stream. This module drains the FIFO
 * into a large RAM ring buffer from the AUX interrupt (and a polling fallback,
 * so it still works even if the finicky mini-UART IRQ enable misbehaves), giving
 * the draw loop a lossless byte stream. Paired with the per-frame handshake in
 * vekterm_baremetal.c, nothing arrives while drawing, so there is no jitter.
 */
#ifndef VEKTERM_UART_RX_H
#define VEKTERM_UART_RX_H

#include <stdint.h>

/* Install the RX ISR, enable the mini-UART RX interrupt + AUX IRQ, and turn on
 * interrupts. Call once after RPI_AuxMiniUartInit() at the data line rate. */
void uart_rx_init(void);

/* Move any bytes sitting in the mini-UART FIFO into the ring buffer now. A
 * belt-and-suspenders complement to the ISR (covers the window where the ISR
 * might be masked or mis-enabled); safe to call from the main loop. */
void uart_rx_poll(void);

/* Discard any buffered/FIFO bytes (call at a frame boundary, where the
 * flow-control handshake guarantees nothing valid is in flight, to drop any
 * line-noise/startup garbage so the next frame starts byte-aligned). */
void uart_rx_flush(void);

/* Non-zero if the ring buffer has a byte ready. */
int uart_rx_pending(void);

/* Pop and return the next byte (0..255), or -1 if the ring buffer is empty. */
int uart_rx_get(void);

/* Count of bytes dropped because the ring buffer was full (diagnostics). */
uint32_t uart_rx_overruns(void);

#endif /* VEKTERM_UART_RX_H */
