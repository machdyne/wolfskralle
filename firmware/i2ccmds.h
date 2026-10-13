/*
 * i2ccmds: the protocol commands for the Sechs bus (I2C, I2C_SCAN).
 * Hardware-independent: the firmware and the host tests use it.
 */

#ifndef I2CCMDS_H
#define I2CCMDS_H

#include <stdint.h>

/* provided by the caller: 1 if the bus may be used now (target power on;
 * SWIO switched off), 0 if target power is off */
int i2ccmd_ready(void);

/* 1 if cmd is one of these commands (and ran: *status is set), 0 if not */
int i2ccmd_handle(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                  uint8_t *out, uint8_t *outlen, uint8_t *status);

#endif
