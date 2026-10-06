/*
 * dbgcmds: the debug and flash commands (dbgcmds.c)
 */

#ifndef DBGCMDS_H
#define DBGCMDS_H

#include <stdint.h>

/* provided by the caller: 1 if SWIO is (now) on, 0 if target power is off */
int dbgcmd_swio_ready(void);

/* 1 if cmd is one of these commands (and ran: *status is set), 0 if not */
int dbgcmd_handle(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                  uint8_t *out, uint8_t *outlen, uint8_t *status);

#endif
