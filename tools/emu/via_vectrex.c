/*
 * via_vectrex.c — a software model of the PiTrex's 6522 VIA + the Vectrex
 * analog vector generator, sitting exactly where the real hardware does.
 *
 * On a real PiTrex the Vectrex's 6522 VIA is genuine silicon: libpitrex's
 * vectrexInterface.c drives it through vectrexwrite()/vectrexread(), which on
 * the Pi bit-bang the cartridge bus and busy-wait on the RDY line. This file
 * provides those same vectrexwrite()/vectrexread()/vectrexinit() symbols, but
 * instead of touching GPIO it feeds an in-process model of the VIA registers
 * and the Vectrex integrators, reconstructing the beam's line segments. The
 * *exact* drawing code (vectrexInterface.c, the SET_YSH16/START_T1/beam macros,
 * the vector font) runs unmodified on top of it.
 *
 * This lets us "boot" vekterm's draw path off-target and intercept what would
 * be drawn on the Vectrex CRT — e.g. the idle splash — with no Pi, no QEMU and
 * no Vectrex. See tools/emu/emu_main.c for the driver and docs/EMULATOR.md for
 * how the model maps onto the hardware.
 */
#include "via_vectrex.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- VIA register file (addressed by the low nibble of 0xD00x) ---------- */
enum {
    R_PORTB = 0x00, R_PORTA = 0x01, R_DDRB = 0x02, R_DDRA = 0x03,
    R_T1CL = 0x04, R_T1CH = 0x05, R_T1LL = 0x06, R_T1LH = 0x07,
    R_T2L = 0x08, R_T2H = 0x09, R_SR = 0x0A, R_ACR = 0x0B,
    R_PCR = 0x0C, R_IFR = 0x0D, R_IER = 0x0E, R_PANH = 0x0F
};

/* libpitrex's empirical strength<->scale fudge: it programs the per-axis DAC
 * "strength" as delta/(scale + SCALE_STRENGTH_DIF) because the integrator runs
 * a little longer than the raw T1 count. To invert that faithfully we integrate
 * over (scale + SCALE_STRENGTH_DIF). It's a libpitrex global (set in v_init). */
extern uint16_t SCALE_STRENGTH_DIF;

/* ---- model state -------------------------------------------------------- */
static uint8_t via[16];

static double beam_x, beam_y;   /* integrator outputs (Vectrex coord units) */
static int    dac;              /* signed Port-A DAC value                   */
static int    mux_en, mux_sel;  /* Port-B bit0 (0=enabled) and SEL1:SEL0     */
static int    x_rate, y_rate;   /* DAC strength latched onto X / Y integrators */
static int    intensity;        /* Z sample/hold (bit7 set => blanked)       */
static int    beam_on;          /* latched /BLANK state (CB2 in PCR)         */

/* A ramp is "armed" when the high byte of T1 is written (START_T1_TIMER) and
 * committed (integrated) at the next state change, so we know the beam-on state
 * that applied while it ran. */
static int    armed;
static int    a_xrate, a_yrate, a_visible;
static double a_ticks;

static VxSink g_sink;
static void  *g_sink_ctx;

void vx_reset(VxSink sink, void *ctx)
{
    memset(via, 0, sizeof via);
    beam_x = beam_y = 0;
    dac = 0;
    mux_en = 0; mux_sel = 0;
    x_rate = y_rate = 0;
    intensity = 0;
    beam_on = 0;
    armed = 0; a_xrate = a_yrate = a_visible = 0; a_ticks = 0;
    g_sink = sink;
    g_sink_ctx = ctx;
}

static void commit_ramp(void)
{
    if (!armed)
        return;
    double dx = (double)a_xrate * a_ticks;
    double dy = (double)a_yrate * a_ticks;
    double nx = beam_x + dx;
    double ny = beam_y + dy;
    int z = intensity & 0x7F;
    if (a_visible && (intensity & 0x80) == 0 && z > 0 && g_sink.line)
        g_sink.line(g_sink_ctx, beam_x, beam_y, nx, ny, z);
    beam_x = nx;
    beam_y = ny;
    armed = 0;
}

static void arm_ramp(void)
{
    commit_ramp();              /* flush any previous ramp first */
    int scale = via[R_T1CL] | (via[R_T1CH] << 8);
    a_ticks = (double)(scale + SCALE_STRENGTH_DIF);
    a_xrate = x_rate;
    a_yrate = y_rate;
    a_visible = beam_on;        /* may be raised to 1 when the beam is switched on */
    armed = 1;
}

/* Drive the integrators from a single VIA register write. */
void vectrexwrite(unsigned int address, unsigned char data)
{
    int a = address & 0x0F;
    via[a] = data;

    switch (a) {
    case R_PORTA:
        dac = (int8_t)data;
        if (mux_en && mux_sel == 0)        /* MUX -> Y integrator rate */
            y_rate = dac;
        else if (mux_en && mux_sel == 2)   /* MUX -> Z (brightness) sample */
            intensity = data;
        if (!mux_en)                       /* mux off: DAC drives X directly */
            x_rate = dac;
        break;

    case R_PORTB:
        mux_en = ((data & 0x01) == 0);
        mux_sel = (data >> 1) & 0x03;
        if (mux_en && mux_sel == 0)
            y_rate = dac;
        else if (mux_en && mux_sel == 2)
            intensity = (unsigned char)dac;
        if (!mux_en)
            x_rate = dac;
        break;

    case R_T1CH:                /* writing the high byte starts T1 -> RAMP */
        arm_ramp();
        break;

    case R_PCR: {               /* VIA_cntl: CA2=/ZERO, CB2=/BLANK */
        int cb2 = (data >> 5) & 0x07;
        int ca2 = (data >> 1) & 0x07;
        int new_beam = (cb2 == 0x07);       /* 111 => /BLANK high => beam on */
        int new_zero = (ca2 == 0x06);       /* 110 => /ZERO low  => zeroing  */
        if (new_beam && armed)
            a_visible = 1;                  /* this armed ramp is a lit draw */
        beam_on = new_beam;
        if (!new_beam)
            commit_ramp();                  /* end of a lit draw / move      */
        if (new_zero) {
            commit_ramp();
            beam_x = beam_y = 0;            /* /ZERO snaps the beam to centre */
        }
        break;
    }

    case R_SR:                  /* shift-register beam control (alt blank path) */
        /* libpitrex defaults to BEAM_LIGHT_BY_CNTL, so the SR isn't the blank
         * source; handle it anyway for completeness. 0xff=on, 0x00=off. */
        if (data == 0xFF || data == 0x00) {
            int new_beam = (data == 0xFF);
            if (new_beam && armed)
                a_visible = 1;
            beam_on = new_beam;
            if (!new_beam)
                commit_ramp();
        }
        break;

    default:
        break;
    }
}

void vectrexwrite_short(unsigned int address, unsigned char data)
{
    vectrexwrite(address, data);
}

/* Reads must let libpitrex's wait-loops fall through and must never look like
 * a button press or a bus reset (see v_readButtons / v_resetDetection). */
unsigned char vectrexread(unsigned int address)
{
    int a = address & 0x0F;
    if (a == R_IFR)
        return 0xFF;            /* all interrupt flags set: T1/T2 waits exit */
    if (a == R_PORTA || a == R_PANH)
        return 0xFF;            /* buttons are active-low: ~0xFF == 0 (none) */
    return via[a];              /* DDRs etc. read back what was written      */
}

unsigned char vectrexread_short(unsigned int address)
{
    return vectrexread(address);
}

int vectrexinit(char viaconfig)
{
    (void)viaconfig;
    return 1;                   /* pretend the VIA came up and #HALT is asserted */
}

int vectrexclose(void)
{
    commit_ramp();
    return 1;
}

void vx_flush(void)
{
    commit_ramp();
}

void vx_beam_pos(double *x, double *y)
{
    if (x) *x = beam_x;
    if (y) *y = beam_y;
}
