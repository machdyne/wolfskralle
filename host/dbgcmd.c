/*
 * wolfctl: the debug commands (phase 3): id, read, crc, resume.
 * See docs/wolfctl.md and docs/debug.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wkproto.h"
#include "wkclient.h"
#include "swiocmd.h"
#include "dbgcmd.h"
#include "chip.h"

#define FLASH_BASE CH32_FLASH_BASE

static uint32_t cn32(const struct chip *c, int off) { return chip_raw32(c, off); }
static const char *dbg_result(int r) { return chip_result(r); }

static int connect(int mode, struct chip *c)
{
	int r;
	int st = chip_connect(&cl, mode, c, &r);
	if (st) die_status("connect", st);
	if (r != WK_DBG_OK) {
		fprintf(stderr, "wolfctl: connect: %s", dbg_result(r));
		uint32_t cfgr = cn32(c, WK_CN_CFGR);
		if (r == WK_DBG_NO_CHIP)
			fprintf(stderr, " (configuration register %08x%s)", cfgr,
			        cfgr == 0xffffffffu ? ": nothing pulls the line low" : "");
		fprintf(stderr, "\n");
		if (r == WK_DBG_NO_CHIP)
			fprintf(stderr, "  is a module seated, and is target power on? "
			        "(wolfctl swio-test 10 checks the link alone)\n");
		exit(1);
	}
	return 0;
}

static void resume(void)
{
	int r;
	int st = chip_resume(&cl, &r);
	if (st) die_status("resume", st);
}

static void print_chip(const struct chip *c)
{
	static const char *how[] = { "attached to the running chip",
	                             "power-cycled and caught at start",
	                             "held in reset and caught at start" };
	uint8_t h = c->raw[WK_CN_HOW];
	printf("chip:        %s%s\n", c->name, c->kind ? "" : " (not a Zwölf module chip)");
	printf("flash:       %u KB%s\n", c->flash / 1024,
	       c->protected ? ", READ-PROTECTED (reads of the flash are meaningless)" : "");
	printf("unique id:   %08x%08x%08x\n", cn32(c, WK_CN_UID), cn32(c, WK_CN_UID + 4),
	       cn32(c, WK_CN_UID + 8));
	printf("connected:   %s\n", h <= 2 ? how[h] : "?");
	uint8_t fast = c->raw[WK_CN_FAST];
	printf("fast paths:  %s (program buffer: %u words)\n",
	       fast == 3 ? "reads and writes" : fast == 1 ? "reads" : fast == 2 ? "writes" : "none",
	       c->raw[WK_CN_PROGBUF]);
	printf("debug:       chip id %08x, hartinfo %08x, id word %08x, dmstatus %08x, "
	       "obr %08x\n", cn32(c, WK_CN_CHIPID_DM), cn32(c, WK_CN_HARTINFO),
	       cn32(c, WK_CN_CHIPID_MEM), cn32(c, WK_CN_DMSTATUS), cn32(c, WK_CN_OBR));
}

int do_id(int mode, int keep_halted)
{
	struct chip c;
	connect(mode, &c);
	print_chip(&c);
	if (keep_halted) printf("left stopped (wolfctl resume restarts it)\n");
	else resume();
	return 0;
}

int do_resume(void)
{
	resume();
	printf("restarted\n");
	return 0;
}

#define crc32_update chip_crc32

static int device_crc(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint8_t in[8], out[8];
	int n = 0;
	wk_put32(in, addr);
	wk_put32(in + 4, len);
	int st = wk_cmd(&cl, WK_CMD_CRC32, in, 8, out, sizeof(out), &n, 5000 + len / 4);
	if (st) die_status("crc", st);
	if (out[0]) {
		fprintf(stderr, "wolfctl: crc: %s\n", dbg_result(out[0]));
		return -1;
	}
	*crc = wk_get32(out + 1);
	return 0;
}

/* addr and len from the arguments, or the whole flash */
static int range(int argc, char **argv, const struct chip *c, uint32_t *addr, uint32_t *len)
{
	*addr = FLASH_BASE;
	*len = c->flash;
	if (argc >= 1) *addr = (uint32_t)strtoul(argv[0], NULL, 0);
	if (argc >= 2) *len = (uint32_t)strtoul(argv[1], NULL, 0);
	if ((*addr & 3) || (*len & 3) || !*len) {
		fprintf(stderr, "wolfctl: address and length must be multiples of 4\n");
		return -1;
	}
	if (*len > WK_CRC_MAX) {
		fprintf(stderr, "wolfctl: at most %u bytes at a time\n", WK_CRC_MAX);
		return -1;
	}
	return 0;
}

static void hexdump(uint32_t addr, const uint8_t *b, uint32_t len)
{
	for (uint32_t i = 0; i < len; i += 16) {
		printf("%08x ", addr + i);
		for (uint32_t k = 0; k < 16; k++)
			if (i + k < len) printf(" %02x", b[i + k]);
			else printf("   ");
		printf("  ");
		for (uint32_t k = 0; k < 16 && i + k < len; k++)
			putchar(b[i + k] >= 32 && b[i + k] < 127 ? b[i + k] : '.');
		printf("\n");
	}
}

int do_read(int argc, char **argv, int mode, const char *outfile, int keep_halted)
{
	struct chip c;
	uint32_t addr, len;
	connect(mode, &c);
	if (range(argc, argv, &c, &addr, &len)) { resume(); return 2; }
	if (c.protected)
		fprintf(stderr, "warning: the chip is read-protected: the flash reads as noise\n");

	uint8_t *buf = malloc(len);
	if (!buf) { resume(); return 1; }
	for (uint32_t off = 0; off < len; ) {
		uint8_t in[5], out[WK_PAYLOAD_MAX];
		uint32_t words = (len - off) / 4;
		if (words > WK_MEM_READ_MAX) words = WK_MEM_READ_MAX;
		wk_put32(in, addr + off);
		in[4] = (uint8_t)words;
		int n = 0;
		int st = wk_cmd(&cl, WK_CMD_MEM_READ, in, 5, out, sizeof(out), &n, 5000);
		if (st) die_status("read", st);
		if (out[0] || n != (int)(1 + 4 * words)) {
			fprintf(stderr, "wolfctl: read at %08x: %s\n", addr + off,
			        out[0] ? dbg_result(out[0]) : "short reply");
			resume();
			return 1;
		}
		memcpy(buf + off, out + 1, 4 * words);
		off += 4 * words;
		if (outfile && len >= 4096 && (off % 4096 == 0 || off == len))
			fprintf(stderr, "\r  %u/%u bytes", off, len);
	}
	if (outfile && len >= 4096) fprintf(stderr, "\n");

	/* read it all a second time on the device: the CRCs must agree */
	uint32_t want = crc32_update(0, buf, len), got = 0;
	int crc_ok = !device_crc(addr, len, &got) && got == want;

	if (outfile) {
		FILE *f = fopen(outfile, "wb");
		if (!f || fwrite(buf, 1, len, f) != len || fclose(f)) {
			perror(outfile);
			free(buf);
			resume();
			return 1;
		}
		printf("%u bytes from %08x to %s, CRC-32 %08x%s\n", len, addr, outfile, want,
		       crc_ok ? ", read twice and equal" : "");
	} else {
		hexdump(addr, buf, len);
	}
	if (!crc_ok)
		fprintf(stderr, "wolfctl: the second read differs (CRC-32 %08x on the "
		        "device, %08x here): the link is unreliable\n", got, want);
	free(buf);
	if (!keep_halted) resume();
	return crc_ok ? 0 : 1;
}

int do_crc(int argc, char **argv, int mode, int keep_halted)
{
	struct chip c;
	uint32_t addr, len, crc;
	connect(mode, &c);
	if (range(argc, argv, &c, &addr, &len)) { resume(); return 2; }
	int r = device_crc(addr, len, &crc);
	if (!r) printf("CRC-32 of %u bytes from %08x: %08x\n", len, addr, crc);
	if (!keep_halted) resume();
	return r ? 1 : 0;
}

/* ---- the profile: where the Wolfskralle's time went ---- */

static int profile_get(int reset, uint8_t *p)
{
	uint8_t r = (uint8_t)reset;
	int n = 0;
	int st = wk_cmd(&cl, WK_CMD_PROFILE, &r, 1, p, WK_PAYLOAD_MAX, &n, 2000);
	if (!st && n < WK_PF_LEN) st = WK_ST_BAD_FRAME;
	return st;
}

static void profile_print(const uint8_t *p, double wall)
{
	double h = wk_get32(p + WK_PF_HANDLER_US) / 1e6;
	uint32_t pk = wk_get32(p + WK_PF_SWIO_PACKETS);
	printf("Wolfskralle:  %u commands, %.2f s running them", wk_get32(p + WK_PF_COMMANDS), h);
	if (wall > 0) printf(" (of %.2f s)", wall);
	printf("\n");
	printf("  reading before writing  %6.2f s\n", wk_get32(p + WK_PF_PRECHECK_US) / 1e6);
	printf("  erasing                 %6.2f s\n", wk_get32(p + WK_PF_ERASE_US) / 1e6);
	printf("  programming             %6.2f s\n", wk_get32(p + WK_PF_PROGRAM_US) / 1e6);
	printf("  reading back            %6.2f s\n", wk_get32(p + WK_PF_VERIFY_US) / 1e6);
	printf("  CRCs (compare, verify)  %6.2f s\n", wk_get32(p + WK_PF_READ_US) / 1e6);
	printf("  pages written %u, flash controller busy polls %u, fast writes given up %u\n",
	       wk_get32(p + WK_PF_PAGES), wk_get32(p + WK_PF_WAITS), wk_get32(p + WK_PF_FALLBACKS));
	printf("  SWIO packets %u (%u incomplete)%s", pk, wk_get32(p + WK_PF_SWIO_BAD),
	       pk && h > 0 ? "" : "\n");
	if (pk && h > 0) printf(", %.0f us each while running commands\n", h * 1e6 / pk);
	printf("  USB: %u GETs answered, %u of them BUSY\n", wk_get32(p + WK_PF_GETS),
	       wk_get32(p + WK_PF_BUSY_GETS));
}

int do_profile(int reset)
{
	uint8_t p[WK_PAYLOAD_MAX];
	int st = profile_get(reset, p);
	if (st) die_status("profile", st);
	profile_print(p, 0);
	if (reset) printf("(counters reset)\n");
	return 0;
}

/* ---- programming (phase 4) ---- */

#include "flash.h"

static int read_file(const char *path, uint8_t *buf, uint32_t max, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return -1; }
	size_t n = fread(buf, 1, max + 1, f);
	fclose(f);
	if (n > max) {
		fprintf(stderr, "wolfctl: %s: larger than %u bytes\n", path, max);
		return -1;
	}
	if (!n) {
		fprintf(stderr, "wolfctl: %s: empty\n", path);
		return -1;
	}
	*len = (uint32_t)n;
	return 0;
}

static void flash_say(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "\r%-60s\n", msg);
}

static void flash_progress(void *ctx, uint32_t done, uint32_t total)
{
	(void)ctx;
	int w = 40, k = (int)(done * w / total);
	fprintf(stderr, "\r  [%.*s%*s] %3u%%", k, "########################################",
	        w - k, "", done * 100 / total);
	if (done == total) fprintf(stderr, "\r%-60s\r", "");
}

int do_flash(int argc, char **argv, int mode, int force)
{
	static uint8_t img[FL_IMAGE_MAX];
	uint32_t len;
	if (argc < 1) {
		fprintf(stderr, "usage: wolfctl flash IMAGE.bin [--force] [--attach|--restart]\n");
		return 2;
	}
	if (read_file(argv[0], img, FL_IMAGE_MAX, &len)) return 1;

	struct flash_job j;
	memset(&j, 0, sizeof(j));
	j.img = img;
	j.len = len;
	j.force = force;
	j.mode = mode;
	j.say = flash_say;
	j.progress = flash_progress;

	uint8_t prof[WK_PAYLOAD_MAX];
	int have_prof = verbose && !profile_get(1, prof);  /* (zeroes the counters) */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int r = flash_run(&cl, &j);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	if (r == FL_OK) {
		printf("%s: %u bytes written and verified on a %s in %.1f s "
		       "(%u pages written, %u already right%s)\n", argv[0], len, j.chip.name, secs,
		       j.written, j.unchanged, j.retries ? ", some retried" : "");
		uint8_t fast = j.chip.raw[WK_CN_FAST];
		if (verbose)
			printf("fast paths: %s\n", fast == 3 ? "reads and writes" : fast == 1 ? "reads" :
			       fast == 2 ? "writes" : "none");
		if (verbose) printf("retries %u, passes %u\n", j.retries, j.passes);
		if (have_prof && !profile_get(0, prof)) profile_print(prof, secs);
		if (verbose) print_client_stats();
		return 0;
	}
	fprintf(stderr, "wolfctl: flash: %s\n", flash_message(r));
	if (r == FL_USB && hid.gone) fprintf(stderr, "  (the Wolfskralle was unplugged)\n");
	if (verbose) print_client_stats();
	return 1;
}

int do_verify(int argc, char **argv, int mode)
{
	static uint8_t img[FL_IMAGE_MAX];
	uint32_t len;
	if (argc < 1) {
		fprintf(stderr, "usage: wolfctl verify IMAGE.bin\n");
		return 2;
	}
	if (read_file(argv[0], img, FL_IMAGE_MAX, &len)) return 1;
	struct chip c;
	connect(mode, &c);
	if (!c.kind || len > c.flash) {
		fprintf(stderr, "wolfctl: verify: %s\n",
		        !c.kind ? "not a CH32V003 or CH32V005" : "the image is larger than this flash");
		resume();
		return 1;
	}
	/* the whole flash: the image, then 0xFF, as flash writes it */
	static uint8_t whole[FL_IMAGE_MAX];
	memset(whole, 0xFF, c.flash);
	memcpy(whole, img, len);
	uint32_t want = chip_crc32(0, whole, c.flash), got = 0;
	int r = device_crc(FLASH_BASE, c.flash, &got);
	resume();
	if (r) return 1;
	if (got == want) {
		printf("%s: the %s holds it (CRC-32 %08x of %u bytes)\n", argv[0], c.name, got, c.flash);
		return 0;
	}
	printf("%s: the %s holds something else (CRC-32 %08x, the image gives %08x)\n",
	       argv[0], c.name, got, want);
	return 1;
}
