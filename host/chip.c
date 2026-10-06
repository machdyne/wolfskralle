/*
 * chip: connecting to, identifying and restarting the target chip.
 * See chip.h and docs/debug.md.
 */

#include <string.h>
#include "chip.h"

const char *chip_result(int r)
{
	switch (r) {
	case WK_DBG_OK:          return "ok";
	case WK_DBG_NO_CHIP:     return "no chip answers on SWIO";
	case WK_DBG_NO_HALT:     return "the chip answers, but its core would not stop";
	case WK_DBG_READ_ERR:    return "a debug access failed";
	case WK_DBG_NOT_HALTED:  return "not connected (the chip runs)";
	case WK_DBG_UNSUPPORTED: return "not a CH32V003 or CH32V005";
	case WK_DBG_PROTECTED:   return "the chip is read-protected";
	case WK_DBG_LOCKED:      return "the flash would not unlock";
	case WK_DBG_NOT_STARTED: return "the flash was not unlocked";
	case WK_DBG_FORBIDDEN:   return "the Wolfskralle refused a write outside the main flash (a bug)";
	case WK_DBG_VERIFY:      return "a page did not verify after three attempts";
	case WK_DBG_BAD_BUFFER:  return "page data went missing on the way";
	default:                 return "unknown result";
	}
}

/* names the chip from what CONNECT read; the programmable ones by the
 * rules machdyne/basic proved (the Wolfskralle's write gate uses the same) */
void chip_identify(struct chip *c)
{
	uint32_t id = chip_raw32(c, WK_CN_CHIPID_DM), mem = chip_raw32(c, WK_CN_CHIPID_MEM);
	uint32_t hi = chip_raw32(c, WK_CN_HARTINFO), kb = chip_raw32(c, WK_CN_FLACAP) & 0xffff;
	c->kind = 0;
	c->flash = c->page = 0;
	if ((id >> 20) == 0x005) {
		c->name = "CH32V005";
		c->kind = 5;
		c->flash = 32768;
		c->page = 256;
	} else if (!id || (id & 0xFFF00F00u) == 0x00300500u) {
		if ((mem >> 20) == 0x003 && (hi & 0x7ff) == 0x0f4) {
			c->name = "CH32V003";
			c->kind = 3;
			c->flash = 16384;
			c->page = 64;
		} else {
			c->name = "unknown (looks like a CH32V003, but its IDs do not match)";
		}
	} else if ((id >> 20) == 0x002) c->name = "CH32V002";
	else if ((id >> 20) == 0x004) c->name = "CH32V004";
	else if ((id >> 20) == 0x006) c->name = "CH32V006";
	else if ((id >> 20) == 0x007) c->name = "CH32V007";
	else c->name = "unknown";
	if (!c->kind && kb >= 2 && kb <= 1024) c->flash = kb * 1024;   /* for reading */
	c->protected = (chip_raw32(c, WK_CN_OBR) & 2) != 0;
}

int chip_connect(struct wk_client *cl, int mode, struct chip *c, int *res)
{
	uint8_t m = (uint8_t)mode;
	int n = 0;
	memset(c, 0, sizeof(*c));
	c->name = "?";
	*res = WK_DBG_NO_CHIP;
	int st = wk_cmd(cl, WK_CMD_CONNECT, &m, 1, c->raw, sizeof(c->raw), &n, 5000);
	if (st) return st;
	if (n < WK_CN_LEN) return WK_ST_BAD_FRAME;
	*res = c->raw[WK_CN_RESULT];
	if (*res == WK_DBG_OK) chip_identify(c);
	return 0;
}

int chip_resume(struct wk_client *cl, int *res)
{
	uint8_t r = 0;
	int n = 0;
	int st = wk_cmd(cl, WK_CMD_RESUME, NULL, 0, &r, 1, &n, 5000);
	*res = r;
	return st;
}

uint32_t chip_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}
