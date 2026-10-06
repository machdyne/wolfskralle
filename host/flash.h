/*
 * flash: programming a module's flash through the Wolfskralle, with the
 * failsafe rules of docs/programming.md. Transport-independent (a
 * wk_client): wolfctl uses it over USB, host/test/flash_test.c against a
 * simulated Wolfskralle and chip.
 */

#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include "wkclient.h"
#include "chip.h"

#define FL_IMAGE_MAX    32768

enum {
	FL_OK = 0,
	FL_BAD_IMAGE,       /* empty, too large, or not Machdyne BASIC (and not forced) */
	FL_NO_CHIP,         /* nothing answers on SWIO */
	FL_NO_HALT,         /* the core would not stop */
	FL_UNSUPPORTED,     /* not a CH32V003 or CH32V005 */
	FL_PROTECTED,       /* read-protected */
	FL_TOO_BIG,         /* the image does not fit this chip's flash */
	FL_WRONG_MODULE,    /* built for the other module (and not forced) */
	FL_LOCKED,          /* the flash would not unlock */
	FL_VERIFY,          /* did not verify: the module is left stopped */
	FL_FORBIDDEN,       /* the Wolfskralle's write gate refused (a bug) */
	FL_USB,             /* the Wolfskralle stopped answering */
};

struct flash_job {
	const uint8_t *img;
	uint32_t len;
	int force;          /* skip the image identity checks */
	int mode;           /* CONNECT mode: 0 auto, 1 attach, 2 restart */
	/* optional callbacks */
	void (*say)(void *ctx, const char *msg);
	void (*progress)(void *ctx, uint32_t done, uint32_t total);
	void *ctx;
	/* results */
	struct chip chip;
	uint32_t pages, written, unchanged, retries, passes;
	uint32_t crc;       /* of the whole flash as written */
	int dbg;            /* the last debug result, for messages */
};

/* 3 (LS10A), 5 (LS11A): the chip a Machdyne BASIC image is for; 0 if
 * it is not one (or names no module) */
int flash_image_chip(const uint8_t *img, uint32_t len);

int flash_run(struct wk_client *cl, struct flash_job *job);
const char *flash_message(int r);

#endif
