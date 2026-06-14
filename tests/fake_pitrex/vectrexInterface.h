/*
 * Minimal stand-in for pitrex/vectrex/vectrexInterface.h.
 *
 * This is NOT libpitrex.  It exists only so CI can compile src/backend_pitrex.c
 * (with -DHAVE_PITREX) and catch signature/usage drift without the real library
 * or hardware present.  The prototypes mirror the upstream ones the backend
 * uses; see https://github.com/gtoal/pitrex for the genuine header.
 */
#ifndef VEKTERM_FAKE_VECTREXINTERFACE_H
#define VEKTERM_FAKE_VECTREXINTERFACE_H

#include <stdint.h>

#define MAX_PIPELINE 3000

void v_init(void);
void v_setScale(uint16_t s);
void v_setBrightness(uint8_t brightness);
void v_WaitRecal(void);
void v_directDraw32(int32_t xStart, int32_t yStart, int32_t xEnd, int32_t yEnd, uint8_t brightness);

#endif /* VEKTERM_FAKE_VECTREXINTERFACE_H */
