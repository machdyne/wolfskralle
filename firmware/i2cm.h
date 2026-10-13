/*
 * i2cm: a bit-banged I2C master for the Sechs bus (Zwölf pins 1 and 2).
 * See docs/sechs.md.
 *
 * Hardware-independent: the firmware provides the pin hooks below (open
 * drain: a line is either pulled low or released to its pull-up); the
 * host tests provide a simulated Sechs module (host/test/sechs_test.c).
 *
 * About 40 kHz. Clock stretching is honoured (up to I2CM_STRETCH_US). The
 * USB interrupt may pause the master at any point: I2C has no lowest
 * clock rate, so that only makes a transfer slower.
 */

#ifndef I2CM_H
#define I2CM_H

#include <stdint.h>

/* ---- hooks, provided by the caller ---- */
void i2cm_scl(int release);         /* 0: pull low, 1: release */
void i2cm_sda(int release);
int i2cm_scl_in(void);              /* the line as it is: 0 low, 1 high */
int i2cm_sda_in(void);
void i2cm_delay(void);              /* a quarter of a bit, about 6 us */
void i2cm_delay_us(uint32_t us);

#define I2CM_STRETCH_US 50000       /* longest clock stretch accepted */

/* results */
#define I2CM_OK         0
#define I2CM_NAK_ADDR   1           /* nobody answered at that address */
#define I2CM_NAK_DATA   2           /* a written byte was not acknowledged */
#define I2CM_STUCK      3           /* a line stays low: no free bus */
#define I2CM_STRETCH    4           /* SCL held low for too long */

/* One transaction: write wlen bytes, then (with a repeated start) read
 * rlen bytes; either may be 0. Both 0: an address-only probe. */
int i2cm_xfer(uint8_t addr, const uint8_t *w, int wlen, uint8_t *r, int rlen);

/* lets the lines go (both released) */
void i2cm_release(void);

#endif
