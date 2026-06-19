#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "vectors.h"
#include "rpi-gpio.h"
#include "rpi-aux.h"
#include "bcm2835_vc.h"

extern int __bss_start__;
extern int __bss_end__;
extern void main();


/** Main function - we'll never return from here */
void  kernelMain( unsigned int r0, unsigned int r1, unsigned int atags )
{
    tweakVectors();
    int32_t arm_clock = lib_bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_ARM);
#ifdef MHZ1000
    if (arm_clock != 1000000000)
    {
      lib_bcm2835_vc_set_clock_rate(BCM2835_VC_CLOCK_ID_ARM, 1000000000);
    }
    /* vekterm-vendor: ALWAYS bring up the mini-UART, regardless of the ARM clock.
     * Upstream only initialised it inside the `arm_clock != target` branch — but
     * deploy/config.txt sets force_turbo=1, so the firmware brings the Pi Zero 2 W
     * up at exactly its 1 GHz turbo frequency *before* the kernel runs. That made
     * the condition false, so RPI_AuxMiniUartInit was skipped, the mini-UART was
     * never configured (AUX disabled, GPIO14/15 not in ALT5), and there was NO
     * boot output at all — the "doesn't boot, no messages" symptom. Initialising
     * unconditionally is correct: UART bring-up is independent of the CPU clock.
     *
     * The mini-UART is clocked by the VPU/core clock, which deploy/config.txt
     * pins at core_freq=256 MHz (so vekterm's 2 Mbaud data link is exact — see
     * src/vekterm_baremetal.c). The banner uses 921600 baud to match the official
     * PiTrex distribution; RPI_AuxMiniUartInit truncates the divisor
     * (mhz/(8*baud)-1), and 256 MHz / (8 * 35) = 914286 is within 0.8% of 921600,
     * so pass an effective clock that lands div+1 on 35. (Upstream assumed a
     * 400 MHz core and 115200 here.) */
    RPI_AuxMiniUartInit( 921600, 8, 258048000);
#else
    if (arm_clock != 700000000)
    {
       lib_bcm2835_vc_set_clock_rate(BCM2835_VC_CLOCK_ID_ARM, 700000000);
    }
    RPI_AuxMiniUartInit( 115200, 8, 250000000);
#endif
    /* Print to the UART using the standard libc functions */
    printf("PiTrex starting...\r\n" );
    printf("BSS start: %X, end: %X\r\n", &__bss_start__, &__bss_end__);
    
    arm_clock = lib_bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_ARM);
    printf("ARM CLOCK  : %d MHz\r\n", (int) arm_clock);
    int32_t vc_clock = lib_bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_CORE);
    printf("GPU CLOCK  : %d MHz\r\n", (int) vc_clock);
    int32_t uart_clock = lib_bcm2835_vc_get_clock_rate(BCM2835_VC_CLOCK_ID_UART);
    printf("UART CLOCK : %d MHz\r\n", (int) uart_clock);
    
    /* Never exit as there is no OS to exit to! */
    while(1)
    {
      main(); // starting the "normal" main
    }
}
