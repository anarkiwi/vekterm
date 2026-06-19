/*
 * via_vectrex.h — interface to the software 6522 VIA + Vectrex vector-generator
 * model. The model implements libpitrex's vectrexwrite()/vectrexread()/
 * vectrexinit() so the real vectrexInterface.c drawing code can run off-target;
 * every reconstructed beam line is handed to a VxSink.
 */
#ifndef VIA_VECTREX_H
#define VIA_VECTREX_H

/* A reconstructed lit beam segment, in Vectrex integrator coordinates
 * (0,0 = screen centre; the same space v_directDraw32 takes), z = 1..127. */
typedef struct {
    void (*line)(void *ctx, double x0, double y0, double x1, double y1, int z);
} VxSink;

/* (Re)initialise the model and install the sink that receives lit segments. */
void vx_reset(VxSink sink, void *ctx);

/* Flush any ramp still in flight (call at end of frame). */
void vx_flush(void);

/* Current integrator (beam) position, for diagnostics. */
void vx_beam_pos(double *x, double *y);

/* libpitrex symbols implemented by the model (declared for the driver). */
void vectrexwrite(unsigned int address, unsigned char data);
void vectrexwrite_short(unsigned int address, unsigned char data);
unsigned char vectrexread(unsigned int address);
unsigned char vectrexread_short(unsigned int address);
int vectrexinit(char viaconfig);
int vectrexclose(void);

#endif /* VIA_VECTREX_H */
