/*
 * sechs: a Sechs master through the Wolfskralle (its I2C commands), and
 * the check that a module runs Machdyne BASIC properly. See docs/sechs.md;
 * the Sechs specification is machdyne/basic's docs/sechs.md.
 *
 * Transport-independent (a wk_client): wolfctl uses it over USB,
 * host/test/sechs_test.c against a simulated module.
 */

#ifndef SECHS_H
#define SECHS_H

#include <stdint.h>
#include "wkclient.h"

#define SX_DEFAULT_ADDR 0x0C

/* core registers (Sechs 11.4) */
#define SX_SIG0     0x00
#define SX_STATUS   0x04
#define SX_OK       0x05
#define SX_FAULT    0x06
#define SX_CONTROL  0x07
#define SX_INFO     0x09
#define SX_CIN      0x18
#define SX_COUT     0x19
#define SX_CDATA    0x1A

#define SX_CAP_I2C_CON  0x04
#define SX_CAP_PROGRAM  0x80
#define SX_ST_BOOT      0x01
#define SX_ST_HALTED    0x02
#define SX_ST_RUNNING   0x04
#define SX_ST_FAULT     0x20
#define SX_ST_DEGRADED  0x40

/* errors from sx_* (negative); I2C results are WK_I2C_* (>= 0) */
#define SX_E_USB    (-1)        /* the Wolfskralle did not answer */
#define SX_E_POWER  (-2)        /* target power is off */

#define SX_INFO_MAX 112

struct sx_info {
	uint8_t addr;
	uint8_t ver, caps, status, ok, fault;
	char text[SX_INFO_MAX + 1];     /* INFO: key=value lines */
};

int sx_xfer(struct wk_client *cl, uint8_t addr, const uint8_t *w, int wlen,
            uint8_t *r, int rlen);
int sx_scan(struct wk_client *cl, uint8_t *found, int max);  /* count, or <0 */
int sx_is_module(struct wk_client *cl, uint8_t addr);
int sx_read_info(struct wk_client *cl, uint8_t addr, struct sx_info *i);

/* the value of key in INFO text, into v (empty if missing); 1 if found */
int sx_info_get(const char *text, const char *key, char *v, int vmax);

const char *sx_i2c_result(int r);
const char *sx_status_text(uint8_t status, char *buf, int n);
const char *sx_fault_text(uint8_t fault);

/* types line and a CR on the module's I2C console; collects its output
 * until it has been quiet for quiet_ms (or max_ms in all). Returns the
 * number of bytes in out (NUL-terminated), or <0. */
int sx_console(struct wk_client *cl, uint8_t addr, const char *line,
               char *out, int outmax, unsigned quiet_ms, unsigned max_ms);

/* ---- the check ---- */

enum {
	SX_GOOD = 0,        /* BASIC runs, as far as every check could tell */
	SX_NONE,            /* no Sechs module answered */
	SX_NOT_BASIC,       /* a Sechs module, but not Machdyne BASIC */
	SX_WRONG_MODULE,    /* BASIC for another module than expected */
	SX_OTHER_IMAGE,     /* its INFO is not the image's */
	SX_FAULTY,          /* it reports a fault, or is degraded */
	SX_CONSOLE_BAD,     /* the console did not compute what it should */
	SX_BUS,             /* the bus itself fails (stuck, stretched) */
	SX_USB,             /* the Wolfskralle stopped answering */
	SX_NO_POWER,
};

struct sx_check {
	uint8_t addr;           /* 0: SX_DEFAULT_ADDR, else the only one */
	int restart;            /* power-cycle the module first (a cold start) */
	int console;            /* the console test, if the module allows it */
	const char *module;     /* expected mod= ("LS10A"), or NULL */
	const uint8_t *img;     /* the image it should run, or NULL */
	uint32_t img_len;
	unsigned wait_ms;       /* how long to wait for the module to answer */
	void (*say)(void *ctx, const char *msg);
	void *ctx;
	/* results */
	struct sx_info info;
	int found;              /* the module answered */
	int console_ran;        /* 1 tested, 0 skipped */
	char console_out[256];
	unsigned answered_ms;   /* after the power-on */
};

int sx_check_run(struct wk_client *cl, struct sx_check *c);
const char *sx_check_message(int r);

#endif
