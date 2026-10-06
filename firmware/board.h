/*
 * Wolfskralle board definitions. See docs/hardware.md.
 *
 * Build for v0 (default) or v1:  make BOARD=v1
 */

#ifndef BOARD_H
#define BOARD_H

#include "wkproto.h"

#if defined(BOARD_V1)
#define BOARD_ID        WK_BOARD_V1
#else
#define BOARD_V0        1
#define BOARD_ID        WK_BOARD_V0
/* the watchdog: feeds it (main loop, every SWIO packet, long waits) */
#define WDT_FEED()      (IWDG->CTLR = 0xAAAA)

#endif

#define PIN_LED         PD0     /* white LED D3, high = on */
#define PIN_TPWRON      PD2     /* target power, high = on (R9 pulls it high) */
#define PIN_TSWO        PC1     /* 100R to TSWIO (Zwölf pin 10) */
#define PIN_TSWI        PC2     /* 100R to TSWIO */

/* SWIO uses both, through TIM2 (partial remap 1), whose channels fix the
 * roles; the schematic's names TSWO/TSWI are the other way round, which
 * does not matter, as the two 100R make the network symmetric */
#define PIN_SWIO_OUT    PC2     /* TIM2 CH2: pulses, open drain */
#define PIN_SWIO_IN     PC1     /* TIM2 CH4: input capture */
#define PIN_ZP1         PC5     /* Zwölf pin 1 (A, Sechs SCL), 10K to TPWR */
#define PIN_ZP2         PC6     /* Zwölf pin 2 (B, Sechs SDA), 10K to TPWR */

/* USB (rv003usb): PD3 D+, PD4 D-, PD5 the 1K5 pull-up on D-; usb_config.h */

#if defined(BOARD_V1)
/* v1 connects TRESET (Zwölf pin 11) to the MCU: set the pin here when the
 * v1 schematic is final (PC0, PC3, PC4 or PC7 are free on v0). */
#ifndef PIN_TRESET
#error "BOARD_V1: define PIN_TRESET in board.h"
#endif
#define HAVE_TRESET     1
#else
#define HAVE_TRESET     0       /* v0: TRESET is not connected (hardware bug) */
#endif

#endif
