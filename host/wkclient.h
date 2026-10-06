/*
 * wkclient: the host side of the Wolfskralle protocol (docs/protocol.md).
 *
 * Transport-independent: wkhid.c provides Linux hidraw; the tests provide
 * a simulated device.
 */

#ifndef WKCLIENT_H
#define WKCLIENT_H

#include <stdint.h>

struct wk_transport {
	/* send one feature report (len bytes, report ID first): 0, or <0 */
	int (*set)(void *ctx, const uint8_t *buf, int len);
	/* fetch one feature report: bytes received, or <0 */
	int (*get)(void *ctx, uint8_t *buf, int len);
	void (*sleep_us)(void *ctx, unsigned us);
	uint64_t (*now_us)(void *ctx);
	void *ctx;
};

struct wk_stats {
	uint32_t cmds;          /* commands completed */
	uint32_t sets, gets;    /* reports sent and fetched */
	uint32_t set_errors;    /* SETs the transport reported as failed */
	uint32_t get_errors;    /* GETs the transport reported as failed */
	uint32_t bad_replies;   /* replies with a wrong length or CRC */
	uint32_t busy;          /* BUSY answers */
	uint32_t resends;       /* requests sent again */
	uint32_t timeouts;      /* commands given up */
};

struct wk_client {
	struct wk_transport t;
	uint8_t seq;
	unsigned poll_min_us;   /* the first wait after BUSY */
	unsigned poll_max_us;   /* the longest wait between GETs */
	struct wk_stats st;
	int verbose;
	int timeout_why;        /* after WK_E_TIMEOUT: WK_TO_* */
};

/* why a command timed out */
#define WK_TO_BUSY      1   /* the device answered BUSY to the end */
#define WK_TO_IO        2   /* the USB transfers failed */
#define WK_TO_NO_MATCH  3   /* it answered, but never to this request */

/* errors (negative); statuses from the device are WK_ST_* (>= 0) */
#define WK_E_TIMEOUT    (-1)    /* no reply within the time allowed */
#define WK_E_ARG        (-2)    /* request payload too long */

void wk_client_init(struct wk_client *c, const struct wk_transport *t);

/* Runs one command: sends the request until the device has it, waits
 * while it runs, and returns its reply. out may be NULL. Returns the
 * device's WK_ST_* status, or a WK_E_* error. timeout_ms covers the
 * whole exchange, including the time the command runs. */
int wk_cmd(struct wk_client *c, uint8_t cmd, const void *in, int inlen,
           void *out, int outmax, int *outlen, unsigned timeout_ms);

const char *wk_status_name(int st);
const char *wk_timeout_why(const struct wk_client *c);

#endif
