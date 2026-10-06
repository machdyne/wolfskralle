/*
 * chip: the target chip as wolfctl sees it (shared by the debug and the
 * programming commands): connecting, identifying, restarting.
 */

#ifndef CHIP_H
#define CHIP_H

#include <stdint.h>
#include "wkproto.h"
#include "wkclient.h"

#define CH32_FLASH_BASE 0x08000000u

struct chip {
	uint8_t raw[WK_CN_LEN];     /* the CONNECT reply */
	const char *name;
	int kind;                   /* 3: CH32V003, 5: CH32V005 (programmable), 0: other */
	uint32_t flash;             /* bytes: what wolfctl reads and programs */
	uint32_t page;              /* bytes per flash page (programmable chips) */
	int protected;              /* read-protected */
};

static inline uint32_t chip_raw32(const struct chip *c, int off) { return wk_get32(c->raw + off); }

/* CONNECT: the frame status (a WK_ST_ or WK_E_ value), and the debug result in *res */
int chip_connect(struct wk_client *cl, int mode, struct chip *c, int *res);
void chip_identify(struct chip *c);
int chip_resume(struct wk_client *cl, int *res);
const char *chip_result(int r);
uint32_t chip_crc32(uint32_t crc, const uint8_t *p, uint32_t n);

#endif
