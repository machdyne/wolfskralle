/*
 * i2ccmds: the protocol commands for the Sechs bus. See i2ccmds.h and
 * docs/protocol.md.
 */

#include <string.h>
#include "wkproto.h"
#include "i2cm.h"
#include "i2ccmds.h"

static uint8_t cmd_i2c(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 2 || in[0] > 0x7F || in[1] > WK_I2C_MAX) return WK_ST_BAD_ARG;
	int rlen = in[1];
	int r = i2cm_xfer(in[0], in + 2, inlen - 2, out + 1, rlen);
	out[0] = (uint8_t)r;
	*outlen = (uint8_t)(1 + (r == I2CM_OK ? rlen : 0));
	return WK_ST_OK;
}

static uint8_t cmd_i2c_scan(uint8_t *out, uint8_t *outlen)
{
	memset(out, 0, 17);
	*outlen = 17;
	for (int a = 0x08; a <= 0x77; a++) {
		int r = i2cm_xfer((uint8_t)a, 0, 0, 0, 0);
		if (r == I2CM_OK) out[1 + a / 8] |= (uint8_t)(1u << (a % 8));
		else if (r != I2CM_NAK_ADDR) {
			out[0] = (uint8_t)r;    /* the bus itself fails: stop here */
			return WK_ST_OK;
		}
	}
	out[0] = WK_I2C_OK;
	return WK_ST_OK;
}

int i2ccmd_handle(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                  uint8_t *out, uint8_t *outlen, uint8_t *status)
{
	if (cmd != WK_CMD_I2C && cmd != WK_CMD_I2C_SCAN) return 0;
	if (!i2ccmd_ready()) {
		*status = WK_ST_NO_POWER;
		return 1;
	}
	*status = cmd == WK_CMD_I2C ? cmd_i2c(in, inlen, out, outlen) : cmd_i2c_scan(out, outlen);
	return 1;
}
