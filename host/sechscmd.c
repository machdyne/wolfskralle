/*
 * wolfctl: the Sechs commands: scan, info, check. See docs/sechs.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wkproto.h"
#include "swiocmd.h"
#include "sechs.h"
#include "sechscmd.h"
#include "flash.h"

static void fail_xfer(const char *what, int r)
{
	fprintf(stderr, "wolfctl: sechs %s: %s\n", what,
	        r <= -10 ? sx_i2c_result(-10 - r) : sx_i2c_result(r));
	exit(1);
}

static void print_info(const struct sx_info *i)
{
	char b[160];
	printf("module:      Sechs %u.%u at 0x%02X\n", i->ver >> 4, i->ver & 15, i->addr);
	printf("status:      %s\n", sx_status_text(i->status, b, sizeof(b)));
	printf("ok:          %02x (alive %s, no fault %s, not degraded %s)\n", i->ok,
	       i->ok & 1 ? "yes" : "NO", i->ok & 2 ? "yes" : "NO", i->ok & 4 ? "yes" : "NO");
	if (i->fault) printf("fault:       %u: %s\n", i->fault, sx_fault_text(i->fault));
	printf("caps:        %02x%s%s\n", i->caps,
	       i->caps & SX_CAP_I2C_CON ? " (I2C console)" : "",
	       i->caps & SX_CAP_PROGRAM ? " (programming mode)" : "");
	const char *p = i->text;
	while (*p) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		printf("info:        %.*s\n", n, p);
		if (!e) break;
		p = e + 1;
	}
}

static int do_scan(void)
{
	uint8_t found[112];
	int n = sx_scan(&cl, found, 112);
	if (n < 0) fail_xfer("scan", n);
	if (!n) printf("nothing answers on the Sechs bus\n");
	for (int i = 0; i < n; i++) {
		struct sx_info inf;
		char fw[48] = "", mod[24] = "";
		if (!sx_read_info(&cl, found[i], &inf)) {
			sx_info_get(inf.text, "fw", fw, sizeof(fw));
			sx_info_get(inf.text, "mod", mod, sizeof(mod));
			printf("0x%02X  Sechs %u.%u  %s  %s\n", found[i], inf.ver >> 4, inf.ver & 15,
			       mod, fw);
		} else {
			printf("0x%02X  (answers, but is not a Sechs module)\n", found[i]);
		}
	}
	return 0;
}

static int do_info(uint8_t addr)
{
	struct sx_info i;
	if (!addr) {
		uint8_t found[4];
		int n = sx_scan(&cl, found, 4);
		if (n < 0) fail_xfer("info", n);
		addr = SX_DEFAULT_ADDR;
		for (int k = 0; k < n; k++)
			if (sx_is_module(&cl, found[k])) { addr = found[k]; break; }
	}
	int r = sx_read_info(&cl, addr, &i);
	if (r == WK_I2C_NAK_DATA) {
		fprintf(stderr, "wolfctl: sechs info: 0x%02X is not a Sechs module\n", addr);
		return 1;
	}
	if (r) fail_xfer("info", r);
	print_info(&i);
	return 0;
}

static void check_say(void *ctx, const char *msg)
{
	(void)ctx;
	if (verbose) fprintf(stderr, "  %s\n", msg);
}

/* used by `sechs check` and `flash --check`; module and img may be NULL */
int sechs_check(uint8_t addr, int restart, int console, const char *module,
                const uint8_t *img, uint32_t img_len)
{
	struct sx_check c;
	memset(&c, 0, sizeof(c));
	c.addr = addr;
	c.restart = restart;
	c.console = console;
	c.module = module;
	c.img = img;
	c.img_len = img_len;
	c.wait_ms = 12000;      /* an LS11 in programming mode waits 10 s */
	c.say = check_say;
	if (cl.poll_max_us > 4000) cl.poll_max_us = 4000;

	int r = sx_check_run(&cl, &c);
	if (c.found) {
		char fw[48] = "", mod[24] = "", b[120];
		sx_info_get(c.info.text, "fw", fw, sizeof(fw));
		sx_info_get(c.info.text, "mod", mod, sizeof(mod));
		printf("module:      %s at 0x%02X, answered %.1f s after %s\n",
		       mod[0] ? mod : "?", c.info.addr, c.answered_ms / 1000.0,
		       restart ? "power-on" : "the check began");
		printf("firmware:    %s\n", fw[0] ? fw : "(none named)");
		printf("status:      %s", sx_status_text(c.info.status, b, sizeof(b)));
		if (c.info.fault) printf("; fault %u: %s", c.info.fault, sx_fault_text(c.info.fault));
		printf("\n");
		if (img && r != SX_NOT_BASIC && r != SX_WRONG_MODULE)
			printf("image:       %s\n", r == SX_OTHER_IMAGE ? "its INFO is NOT in the image"
			                                                : "its INFO is the image's");
		if (c.console_ran) {
			printf("console:     PRINT 1234+4321 -> %s\n",
			       strstr(c.console_out, "5555") ? "5555" : "(wrong answer)");
			if (verbose || r == SX_CONSOLE_BAD) printf("%s\n", c.console_out);
		} else if (r == SX_GOOD || r == SX_CONSOLE_BAD) {
			printf("console:     not tested (%s)\n",
			       !console ? "--no-console" :
			       !(c.info.caps & SX_CAP_I2C_CON) ? "no I2C console" :
			       "a program is running");
		}
	}
	if (r == SX_GOOD) {
		printf("result:      %s\n", sx_check_message(r));
		return 0;
	}
	fprintf(stderr, "wolfctl: sechs check: %s\n", sx_check_message(r));
	if (r == SX_NONE)
		fprintf(stderr, "  (an LS10A or LS11A with BASIC answers at 0x0C within about a "
		        "second of power-on)\n");
	return 1;
}

static int read_image(const char *path, uint8_t *buf, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return -1; }
	*len = (uint32_t)fread(buf, 1, FL_IMAGE_MAX, f);
	fclose(f);
	return 0;
}

int do_sechs(int argc, char **argv, uint8_t addr, int restart, int console)
{
	if (argc < 1) {
		fprintf(stderr, "usage: wolfctl sechs scan | info [-a ADDR] | "
		        "check [IMAGE] [-a ADDR] [--no-restart] [--no-console]\n");
		return 2;
	}
	if (!strcmp(argv[0], "scan")) return do_scan();
	if (!strcmp(argv[0], "info")) return do_info(argc > 1 ? (uint8_t)strtoul(argv[1], NULL, 0) : addr);
	if (!strcmp(argv[0], "check")) {
		static uint8_t img[FL_IMAGE_MAX];
		uint32_t len = 0;
		const char *module = NULL;
		if (argc > 1) {
			if (read_image(argv[1], img, &len)) return 1;
			int k = flash_image_chip(img, len);
			module = k == 3 ? "LS10A" : k == 5 ? "LS11A" : NULL;
		}
		return sechs_check(addr, restart, console, module, argc > 1 ? img : NULL, len);
	}
	fprintf(stderr, "wolfctl: sechs: scan, info or check\n");
	return 2;
}
