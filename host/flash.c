/*
 * flash: programming a module's flash. See flash.h and
 * docs/programming.md (the failsafe rules are numbered as there).
 */

#include <stdio.h>
#include <string.h>
#include "flash.h"

static void say(struct flash_job *j, const char *msg)
{
	if (j->say) j->say(j->ctx, msg);
}

static int contains(const uint8_t *img, uint32_t len, const char *t)
{
	uint32_t n = (uint32_t)strlen(t);
	for (uint32_t i = 0; i + n <= len; i++)
		if (!memcmp(img + i, t, n)) return 1;
	return 0;
}

int flash_image_chip(const uint8_t *img, uint32_t len)
{
	if (!contains(img, len, "fw=Machdyne BASIC")) return 0;
	if (contains(img, len, "mod=LS10A")) return 3;
	if (contains(img, len, "mod=LS11A")) return 5;
	return 0;
}

const char *flash_message(int r)
{
	switch (r) {
	case FL_OK:           return "written and verified";
	case FL_BAD_IMAGE:    return "not a Machdyne BASIC firmware for LS10A or LS11A, or empty or "
	                             "larger than 32 KB (--force writes other firmware): nothing written";
	case FL_NO_CHIP:      return "no chip answers on SWIO (is a module seated?): nothing written";
	case FL_NO_HALT:      return "the chip would not stop: nothing written";
	case FL_UNSUPPORTED:  return "not a CH32V003 or CH32V005: nothing written";
	case FL_PROTECTED:    return "the chip is read-protected: nothing written";
	case FL_TOO_BIG:      return "the image is larger than this chip's flash: nothing written";
	case FL_WRONG_MODULE: return "the firmware is for the other module (LS10A: CH32V003, "
	                             "LS11A: CH32V005): nothing written";
	case FL_LOCKED:       return "the flash would not unlock: nothing written";
	case FL_VERIFY:       return "the flash did not verify: the module is left stopped; "
	                             "flash it again";
	case FL_FORBIDDEN:    return "the Wolfskralle refused a write outside the main flash "
	                             "(a bug: please report)";
	case FL_USB:          return "the Wolfskralle stopped answering: flash again "
	                             "(the module may be left stopped)";
	default:              return "?";
	}
}

/* the page as it must end up: the image, then 0xFF */
static void page_bytes(const struct flash_job *j, uint32_t off, uint32_t page, uint8_t *b)
{
	memset(b, 0xFF, page);
	if (off < j->len) {
		uint32_t n = j->len - off;
		memcpy(b, j->img + off, n < page ? n : page);
	}
}

/* one page: its data (the last part inside PAGE_WRITE itself, so a
 * CH32V003 page takes one request), then write; the debug result, or -1
 * if USB failed */
static int one_page(struct wk_client *cl, uint32_t addr, const uint8_t *b, uint32_t page,
                    int differs, int *written, int *attempts)
{
	uint8_t in[WK_PAYLOAD_MAX], out[8];
	int n, st, blank = 1;
	uint32_t off = 0;
	for (uint32_t i = 0; i < page; i++)
		if (b[i] != 0xFF) blank = 0;

	if (!blank) {
		while (page - off > WK_PW_DATA_MAX) {
			wk_put16(in, (uint16_t)off);
			memcpy(in + 2, b + off, WK_PAGE_LOAD_MAX);
			st = wk_cmd(cl, WK_CMD_PAGE_LOAD, in, 2 + WK_PAGE_LOAD_MAX, out, sizeof(out), &n, 3000);
			if (st == WK_E_TIMEOUT) return -1;
			if (st || out[0]) return WK_DBG_BAD_BUFFER;
			off += WK_PAGE_LOAD_MAX;
		}
	}
	wk_put32(in, addr);
	wk_put32(in + 4, chip_crc32(0, b, page));
	in[8] = (uint8_t)((blank ? WK_PW_BLANK : 0) | (differs ? WK_PW_DIFFERS : 0));
	int inlen = 9;
	if (!blank) {
		wk_put16(in + 9, (uint16_t)off);
		memcpy(in + 11, b + off, page - off);
		inlen = (int)(11 + page - off);
	}
	st = wk_cmd(cl, WK_CMD_PAGE_WRITE, in, inlen, out, sizeof(out), &n, 10000);
	if (st == WK_E_TIMEOUT) return -1;
	if (st || n < 3) return WK_DBG_READ_ERR;
	*written = out[1];
	*attempts = out[2];
	return out[0];
}

/* which pages differ from what they must hold: CRCs compared on the
 * Wolfskralle, 28 pages a request; -1 if USB failed, else the result */
static int compare(struct wk_client *cl, struct flash_job *j, const uint32_t *crcs,
                   uint32_t pages, uint8_t *differs)
{
	uint8_t in[WK_PAYLOAD_MAX], out[8];
	int n;
	for (uint32_t p = 0; p < pages; p += WK_VERIFY_PAGES_MAX) {
		uint32_t k = pages - p < WK_VERIFY_PAGES_MAX ? pages - p : WK_VERIFY_PAGES_MAX;
		wk_put32(in, CH32_FLASH_BASE + p * j->chip.page);
		in[4] = (uint8_t)k;
		for (uint32_t i = 0; i < k; i++) wk_put32(in + 5 + 4 * i, crcs[p + i]);
		int st = wk_cmd(cl, WK_CMD_VERIFY_PAGES, in, (int)(5 + 4 * k), out, sizeof(out), &n,
		                5000 + k * j->chip.page / 2);
		if (st == WK_E_TIMEOUT) return -1;
		if (st || n < 5) return WK_DBG_READ_ERR;
		if (out[0]) return out[0];
		uint32_t bits = wk_get32(out + 1);
		for (uint32_t i = 0; i < k; i++) differs[p + i] = (bits >> i) & 1;
	}
	return WK_DBG_OK;
}

static int flash_begin(struct wk_client *cl, struct flash_job *j)
{
	uint8_t out[8];
	int n = 0;
	int st = wk_cmd(cl, WK_CMD_FLASH_BEGIN, NULL, 0, out, sizeof(out), &n, 5000);
	if (st == WK_E_TIMEOUT) return -1;
	if (st || n < 7) return WK_DBG_READ_ERR;
	if (out[0]) return out[0];
	/* the Wolfskralle's gate must agree with this chip */
	if (wk_get16(out + 1) != j->chip.page || wk_get32(out + 3) != j->chip.flash)
		return WK_DBG_UNSUPPORTED;
	return WK_DBG_OK;
}

static void flash_end(struct wk_client *cl)
{
	uint8_t out[4];
	int n;
	wk_cmd(cl, WK_CMD_FLASH_END, NULL, 0, out, sizeof(out), &n, 5000);
}

/* gives up: the flash locked again, the chip left stopped (rule 6) */
static int fail(struct wk_client *cl, struct flash_job *j, int r)
{
	if (r != FL_USB) flash_end(cl);
	(void)j;
	return r;
}

/* refuses before anything is written: the chip runs on as before */
static int refuse(struct wk_client *cl, struct flash_job *j, int r)
{
	int res;
	say(j, "refused: restarting the chip as it was");
	chip_resume(cl, &res);
	return r;
}

static int flash_run2(struct wk_client *cl, struct flash_job *j);

/* the job asks a busy Wolfskralle more often than usual: its requests are
 * short (a page takes a few ms), and waiting adds up over hundreds */
int flash_run(struct wk_client *cl, struct flash_job *j)
{
	unsigned keep = cl->poll_max_us;
	if (cl->poll_max_us > 4000) cl->poll_max_us = 4000;
	int r = flash_run2(cl, j);
	cl->poll_max_us = keep;
	return r;
}

static int flash_run2(struct wk_client *cl, struct flash_job *j)
{
	char msg[128];
	int r, res;
	memset(&j->chip, 0, sizeof(j->chip));
	j->pages = j->written = j->unchanged = j->retries = j->passes = 0;
	j->crc = 0;

	/* rule 4: the image, before the module is touched */
	say(j, "checking the image");
	if (!j->len || j->len > FL_IMAGE_MAX) return FL_BAD_IMAGE;
	int want = flash_image_chip(j->img, j->len);
	if (!j->force && !want) return FL_BAD_IMAGE;

	/* rules 5 and 3: stopped and identified before anything is written */
	say(j, "connecting");
	int st = chip_connect(cl, j->mode, &j->chip, &res);
	j->dbg = res;
	if (st) return FL_USB;
	if (res == WK_DBG_NO_CHIP) return FL_NO_CHIP;
	if (res == WK_DBG_NO_HALT) return FL_NO_HALT;
	if (res) return refuse(cl, j, FL_NO_CHIP);
	snprintf(msg, sizeof(msg), "%s, stopped", j->chip.name);
	say(j, msg);
	if (!j->chip.kind) return refuse(cl, j, FL_UNSUPPORTED);
	if (j->chip.protected) return refuse(cl, j, FL_PROTECTED);
	/* rule 4, the parts that depend on the chip */
	if (j->len > j->chip.flash) return refuse(cl, j, FL_TOO_BIG);
	if (!j->force && want != j->chip.kind) return refuse(cl, j, FL_WRONG_MODULE);

	say(j, "unlocking the flash");
	r = flash_begin(cl, j);
	for (int t = 0; t < 2 && (r == WK_DBG_LOCKED || r == WK_DBG_READ_ERR); t++) {
		/* a key that may have gone astray leaves the flash locked until
		 * the chip restarts: stop it again (a core reset) and retry */
		say(j, "the flash stayed locked: stopping the chip again");
		st = chip_connect(cl, (j->mode & WK_CN_MODE_SLOW) | 1, &j->chip, &res);
		if (st) return FL_USB;
		if (res) return refuse(cl, j, FL_LOCKED);
		r = flash_begin(cl, j);
	}
	j->dbg = r;
	if (r < 0) return FL_USB;
	if (r == WK_DBG_UNSUPPORTED) return refuse(cl, j, FL_UNSUPPORTED);
	if (r == WK_DBG_PROTECTED) return refuse(cl, j, FL_PROTECTED);
	if (r) return refuse(cl, j, FL_LOCKED);

	/* every page of the main flash: the image, then 0xFF. The Wolfskralle
	 * first compares every page with its CRC; only those that differ are
	 * sent and written (and verified as they are written: rule 6) */
	uint32_t page = j->chip.page, pages = j->chip.flash / page;
	static uint32_t crcs[FL_IMAGE_MAX / 64];
	static uint8_t differs[FL_IMAGE_MAX / 64];
	uint8_t b[WK_PAGE_MAX];
	j->pages = pages;
	uint32_t want_crc = 0;
	for (uint32_t p = 0; p < pages; p++) {
		page_bytes(j, p * page, page, b);
		crcs[p] = chip_crc32(0, b, page);
		want_crc = chip_crc32(want_crc, b, page);
	}

	for (int pass = 1; pass <= 2; pass++) {
		j->passes = (uint32_t)pass;
		say(j, pass == 1 ? "comparing" : "the read-back differed: comparing again");
		r = compare(cl, j, crcs, pages, differs);
		if (r < 0) return fail(cl, j, FL_USB);
		if (r) {
			j->dbg = r;
			snprintf(msg, sizeof(msg), "comparing: %s", chip_result(r));
			say(j, msg);
			return fail(cl, j, FL_VERIFY);
		}
		uint32_t todo = 0, done = 0;
		for (uint32_t p = 0; p < pages; p++) todo += differs[p];
		if (pass == 1) j->unchanged = pages - todo;
		if (todo) say(j, "writing");
		for (uint32_t p = 0; p < pages; p++) {
			if (!differs[p]) continue;
			uint32_t addr = CH32_FLASH_BASE + p * page;
			int written = 0, attempts = 0, tries;
			page_bytes(j, p * page, page, b);
			for (tries = 0; tries < 3; tries++) {
				/* the compare found it different: no need to read it
				 * again first (unless retrying after lost data) */
				r = one_page(cl, addr, b, page, tries == 0, &written, &attempts);
				if (r < 0) return fail(cl, j, FL_USB);
				if (r != WK_DBG_BAD_BUFFER) break;
				j->retries++;           /* data lost on the way: send it again */
			}
			j->dbg = r;
			if (r == WK_DBG_FORBIDDEN) return fail(cl, j, FL_FORBIDDEN);
			if (r) {
				snprintf(msg, sizeof(msg), "page %u at %08x: %s", p, addr, chip_result(r));
				say(j, msg);
				return fail(cl, j, FL_VERIFY);
			}
			if (attempts > 1) j->retries += (uint32_t)(attempts - 1);
			if (pass == 1 && written) j->written++;
			if (j->progress) j->progress(j->ctx, ++done, todo);
		}

		/* rule 6: everything read back once more, as a CRC on the device */
		say(j, "verifying everything");
		uint8_t in[8], out[8];
		int n = 0;
		wk_put32(in, CH32_FLASH_BASE);
		wk_put32(in + 4, j->chip.flash);
		st = wk_cmd(cl, WK_CMD_CRC32, in, 8, out, sizeof(out), &n, 5000 + j->chip.flash / 2);
		if (st == WK_E_TIMEOUT) return fail(cl, j, FL_USB);
		if (!st && n >= 5 && !out[0] && wk_get32(out + 1) == want_crc) {
			j->crc = want_crc;
			snprintf(msg, sizeof(msg), "verified %u bytes, CRC-32 %08x", j->chip.flash, want_crc);
			say(j, msg);
			say(j, "restarting the chip");
			flash_end(cl);
			chip_resume(cl, &res);
			return FL_OK;
		}
	}
	return fail(cl, j, FL_VERIFY);
}
