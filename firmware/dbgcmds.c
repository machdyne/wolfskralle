/*
 * dbgcmds: the protocol commands for the target's debug module and its
 * flash (CONNECT, RESUME, MEM_READ, CRC32, FLASH_BEGIN, PAGE_LOAD,
 * PAGE_WRITE, FLASH_END). Hardware-independent: the firmware and the host
 * tests (host/test/flash_test.c) both use this file. See docs/protocol.md,
 * docs/debug.md and docs/programming.md.
 */

#include <string.h>
#include "wkproto.h"
#include "ch32dbg.h"
#include "dbgcmds.h"

static uint32_t pagebuf[WK_PAGE_MAX / 4];

static uint8_t cmd_connect(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 1 || (in[0] & 0x7F) > DBG_MODE_RESTART) return WK_ST_BAD_ARG;
	struct dbg_info d;
	dbg_fast_allowed = (in[0] & WK_CN_MODE_SLOW) ? 0 : (DBG_FAST_READ | DBG_FAST_WRITE);
	out[WK_CN_RESULT] = (uint8_t)dbg_connect(in[0] & 0x7F, &d);
	out[WK_CN_HOW] = d.how;
	wk_put32(out + WK_CN_CFGR, d.cfgr);
	wk_put32(out + WK_CN_DMSTATUS, d.dmstatus);
	wk_put32(out + WK_CN_HARTINFO, d.hartinfo);
	wk_put32(out + WK_CN_CHIPID_DM, d.chipid_dm);
	wk_put32(out + WK_CN_CHIPID_MEM, d.chipid_mem);
	wk_put32(out + WK_CN_OBR, d.obr);
	wk_put32(out + WK_CN_FLACAP, d.flacap);
	for (int i = 0; i < 3; i++) wk_put32(out + WK_CN_UID + 4 * i, d.uid[i]);
	out[WK_CN_PROGBUF] = d.progbufsize;
	out[WK_CN_FAST] = d.fast;
	*outlen = WK_CN_LEN;
	return WK_ST_OK;
}

static uint8_t cmd_mem_read(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 5 || !in[4] || in[4] > WK_MEM_READ_MAX) return WK_ST_BAD_ARG;
	uint32_t w[WK_MEM_READ_MAX];
	int n = in[4];
	int r = dbg_mem_read(wk_get32(in), w, n);
	out[0] = (uint8_t)r;
	*outlen = 1;
	if (!r) {
		for (int i = 0; i < n; i++) wk_put32(out + 1 + 4 * i, w[i]);
		*outlen = (uint8_t)(1 + 4 * n);
	}
	return WK_ST_OK;
}

static uint8_t cmd_crc32(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 8) return WK_ST_BAD_ARG;
	uint32_t len = wk_get32(in + 4), crc = 0;
	if ((len & 3) || len > WK_CRC_MAX) return WK_ST_BAD_ARG;
	out[0] = (uint8_t)dbg_crc32(wk_get32(in), len, &crc);
	wk_put32(out + 1, crc);
	*outlen = 5;
	return WK_ST_OK;
}

static uint8_t cmd_flash_begin(uint8_t *out, uint8_t *outlen)
{
	out[0] = (uint8_t)dbg_flash_begin();
	wk_put16(out + 1, (uint16_t)dbg_page);
	wk_put32(out + 3, dbg_flash_size);
	*outlen = 7;
	return WK_ST_OK;
}

static uint8_t cmd_page_load(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 2) return WK_ST_BAD_ARG;
	uint16_t off = wk_get16(in);
	int n = inlen - 2;
	if (off + n > WK_PAGE_MAX) return WK_ST_BAD_ARG;
	memcpy((uint8_t *)pagebuf + off, in + 2, n);
	out[0] = WK_DBG_OK;
	*outlen = 1;
	return WK_ST_OK;
}

static uint8_t cmd_page_write(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 9) return WK_ST_BAD_ARG;
	uint32_t addr = wk_get32(in), crc = wk_get32(in + 4);
	int written = 0, attempts = 0, r;
	*outlen = 3;
	out[1] = out[2] = 0;
	if (!dbg_page) {
		out[0] = (uint8_t)(dbg_halted ? WK_DBG_UNSUPPORTED : WK_DBG_NOT_HALTED);
		return WK_ST_OK;
	}
	if (inlen > 9) {                    /* data for the buffer first */
		if (inlen < 11) return WK_ST_BAD_ARG;
		uint16_t off = wk_get16(in + 9);
		int n = inlen - 11;
		if (off + n > WK_PAGE_MAX) return WK_ST_BAD_ARG;
		memcpy((uint8_t *)pagebuf + off, in + 11, n);
	}
	if (in[8] & WK_PW_BLANK) {
		memset(pagebuf, 0xFF, sizeof(pagebuf));
	} else if (dbg_crc32_update(0, (const uint8_t *)pagebuf, dbg_page) != crc) {
		out[0] = WK_DBG_BAD_BUFFER;     /* a PAGE_LOAD went missing */
		return WK_ST_OK;
	}
	r = dbg_page_write(addr, pagebuf, (in[8] & WK_PW_DIFFERS) != 0, &written, &attempts);
	out[0] = (uint8_t)r;
	out[1] = (uint8_t)written;
	out[2] = (uint8_t)attempts;
	return WK_ST_OK;
}

static uint8_t cmd_verify_pages(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 5) return WK_ST_BAD_ARG;
	uint32_t addr = wk_get32(in), differs = 0;
	int n = in[4];
	if (!n || n > WK_VERIFY_PAGES_MAX || inlen < 5 + 4 * n) return WK_ST_BAD_ARG;
	*outlen = 5;
	wk_put32(out + 1, 0);
	if (!dbg_page) {
		out[0] = (uint8_t)(dbg_halted ? WK_DBG_UNSUPPORTED : WK_DBG_NOT_HALTED);
		return WK_ST_OK;
	}
	for (int i = 0; i < n; i++) {
		uint32_t crc = 0;
		int r = dbg_crc32(addr + (uint32_t)i * dbg_page, dbg_page, &crc);
		if (r) {
			out[0] = (uint8_t)r;
			return WK_ST_OK;
		}
		if (crc != wk_get32(in + 5 + 4 * i)) differs |= 1u << i;
	}
	out[0] = WK_DBG_OK;
	wk_put32(out + 1, differs);
	return WK_ST_OK;
}

static uint8_t cmd_flash_end(uint8_t *out, uint8_t *outlen)
{
	out[0] = (uint8_t)dbg_flash_end();
	*outlen = 1;
	return WK_ST_OK;
}

int dbgcmd_handle(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                  uint8_t *out, uint8_t *outlen, uint8_t *status)
{
	switch (cmd) {
	case WK_CMD_CONNECT: case WK_CMD_RESUME: case WK_CMD_MEM_READ: case WK_CMD_CRC32:
	case WK_CMD_FLASH_BEGIN: case WK_CMD_PAGE_LOAD: case WK_CMD_PAGE_WRITE:
	case WK_CMD_FLASH_END: case WK_CMD_VERIFY_PAGES:
		break;
	default:
		return 0;
	}
	if (!dbgcmd_swio_ready()) {
		*status = WK_ST_NO_POWER;
		return 1;
	}
	switch (cmd) {
	case WK_CMD_CONNECT:     *status = cmd_connect(in, inlen, out, outlen); break;
	case WK_CMD_RESUME:      out[0] = (uint8_t)dbg_resume(); *outlen = 1; *status = WK_ST_OK; break;
	case WK_CMD_MEM_READ:    *status = cmd_mem_read(in, inlen, out, outlen); break;
	case WK_CMD_CRC32:       *status = cmd_crc32(in, inlen, out, outlen); break;
	case WK_CMD_FLASH_BEGIN: *status = cmd_flash_begin(out, outlen); break;
	case WK_CMD_PAGE_LOAD:   *status = cmd_page_load(in, inlen, out, outlen); break;
	case WK_CMD_PAGE_WRITE:  *status = cmd_page_write(in, inlen, out, outlen); break;
	case WK_CMD_FLASH_END:   *status = cmd_flash_end(out, outlen); break;
	case WK_CMD_VERIFY_PAGES: *status = cmd_verify_pages(in, inlen, out, outlen); break;
	}
	return 1;
}
