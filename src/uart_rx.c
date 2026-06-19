/*
 * uart_rx.c — interrupt-driven mini-UART receive ring buffer. See uart_rx.h.
 * Baremetal only (built into the PiTrex kernel, not the host tool or emulator).
 */
#include "uart_rx.h"

#include <rpi-aux.h>
#include <rpi-interrupts.h>

/* MU_LSR bit 0 = receive FIFO has at least one symbol (errata-corrected). */
#define MU_LSR_DATA_READY 0x01u
/* MU_IER bit 0 = enable receive interrupt (per the BCM2835 errata; the original
 * PDF had the bits the other way round). */
#define MU_IER_RX_ENABLE 0x01u
/* The AUX block (mini-UART/SPI) is GPU IRQ 29, in Enable_IRQs_1 / IRQ_pending_1. */
#define AUX_IRQ_BIT (1u << 29)

/* Power-of-two ring buffer: single producer (ISR or uart_rx_poll, mutually
 * excluded by masking IRQs in poll), single consumer (the draw loop). 16 KiB is
 * far more than a frame, so the handshake keeps it from ever filling. */
#define RXBUF_SIZE 16384u
#define RXBUF_MASK (RXBUF_SIZE - 1u)
static volatile uint8_t rxbuf[RXBUF_SIZE];
static volatile uint32_t rx_head; /* produced */
static volatile uint32_t rx_tail; /* consumed */
static volatile uint32_t rx_drops;

/* Drain the hardware FIFO into the ring buffer. Runs in ISR context and (with
 * IRQs masked) from uart_rx_poll(). */
static void drain_fifo(void)
{
    aux_t *aux = RPI_GetAux();
    while (aux->MU_LSR & MU_LSR_DATA_READY) {
        uint8_t c = (uint8_t)aux->MU_IO;
        uint32_t next = (rx_head + 1u) & RXBUF_MASK;
        if (next != rx_tail) {
            rxbuf[rx_head] = c;
            rx_head = next;
        } else {
            rx_drops++; /* full: drop (the handshake should prevent this) */
        }
    }
}

void uart_rx_init(void)
{
    rx_head = rx_tail = 0;
    rx_drops = 0;

#ifdef VT_UART_IRQ
    /* Optional (off by default): drive RX from the AUX interrupt as well as the
     * poll. The mini-UART IRQ-enable bit is errata-prone and untested here, and
     * the handshake already keeps the link lossless via uart_rx_poll(), so this
     * is opt-in. Build with -DVT_UART_IRQ to try it. */
    aux_t *aux = RPI_GetAux();
    aux->MU_IER |= MU_IER_RX_ENABLE;                     /* mini-UART: interrupt on RX   */
    RPI_aux_irq_handler = drain_fifo;                    /* dispatch target in the vector */
    RPI_GetIrqController()->Enable_IRQs_1 = AUX_IRQ_BIT; /* route AUX to the ARM */
    _enable_interrupts();                                /* clear CPSR I (global enable)  */
#endif
}

void uart_rx_poll(void)
{
#ifdef VT_UART_IRQ
    /* Mask IRQs so the poll and the ISR don't both push concurrently. (Only when
     * the ISR is enabled — otherwise this must not touch the global IRQ bit.) */
    __asm__ __volatile__("cpsid i" ::: "memory");
    drain_fifo();
    __asm__ __volatile__("cpsie i" ::: "memory");
#else
    drain_fifo();
#endif
}

void uart_rx_flush(void)
{
    uart_rx_poll();    /* pull any FIFO bytes into the ring... */
    rx_tail = rx_head; /* ...then discard the ring contents */
}

int uart_rx_pending(void)
{
    return rx_head != rx_tail;
}

int uart_rx_get(void)
{
    if (rx_head == rx_tail)
        return -1;
    uint8_t c = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1u) & RXBUF_MASK;
    return (int)c;
}

uint32_t uart_rx_overruns(void)
{
    return rx_drops;
}
