/*
 * wkclient: the host side of the Wolfskralle protocol. See wkclient.h and
 * docs/protocol.md.
 *
 * One command:
 * 1. SET the request (a new seq for every command; the same frame for
 *    every resend of it).
 * 2. GET until the reply arrives:
 *    - a failed GET, or a reply with a wrong length or CRC: GET again;
 *    - BUSY: the device is running a command (ours, or an older one):
 *      wait a little, GET again;
 *    - a reply with our seq, cmd and check byte: done;
 *    - anything else (another reply, NO_REPLY): the device is idle and
 *      does not have our request, so SET it again (step 1).
 * A request the device did get but whose reply was lost is never run
 * twice: the device answers the repeat from the reply it kept.
 */

#include <string.h>
#include "wkproto.h"
#include "wkclient.h"

void wk_client_init(struct wk_client *c, const struct wk_transport *t)
{
	memset(c, 0, sizeof(*c));
	c->t = *t;
	c->poll_min_us = 200;
	c->poll_max_us = 20000;     /* gentle on a busy device: each GET costs it */
	/* a starting seq unlikely to equal the device's last one */
	c->seq = (uint8_t)(c->t.now_us(c->t.ctx) >> 10);
}

static unsigned grow(struct wk_client *c, unsigned wait)
{
	if (!wait) return c->poll_min_us;
	wait *= 2;
	return wait > c->poll_max_us ? c->poll_max_us : wait;
}

int wk_cmd(struct wk_client *c, uint8_t cmd, const void *in, int inlen,
           void *out, int outmax, int *outlen, unsigned timeout_ms)
{
	uint8_t req[WK_REPORT_LEN], rep[WK_REPORT_LEN];

	if (inlen < 0 || inlen > WK_PAYLOAD_MAX) return WK_E_ARG;
	memset(req, 0, sizeof(req));
	req[0] = WK_REPORT_ID;
	req[1] = ++c->seq;
	req[2] = cmd;
	req[4] = (uint8_t)inlen;
	if (inlen) memcpy(req + WK_HDR_LEN, in, inlen);
	wk_frame_seal(req);

	uint64_t deadline = c->t.now_us(c->t.ctx) + (uint64_t)timeout_ms * 1000;
	unsigned wait = 0;
	int last = 0;               /* what the last GET brought: WK_TO_* */

	for (;;) {
		c->st.sets++;
		/* only the bytes in use (the device takes short requests) */
		if (c->t.set(c->t.ctx, req, WK_HDR_LEN + inlen) < 0)
			c->st.set_errors++;   /* it may have arrived all the same */

		for (;;) {
			if (c->t.now_us(c->t.ctx) > deadline) {
				c->st.timeouts++;
				c->timeout_why = last ? last : WK_TO_IO;
				return WK_E_TIMEOUT;
			}
			if (wait) c->t.sleep_us(c->t.ctx, wait);

			c->st.gets++;
			memset(rep, 0, sizeof(rep));
			rep[0] = WK_REPORT_ID;
			int n = c->t.get(c->t.ctx, rep, WK_REPORT_LEN);
			if (n < 0) {
				c->st.get_errors++;
				last = WK_TO_IO;
				wait = grow(c, wait);
				continue;
			}
			if (n < WK_HDR_LEN || n < WK_HDR_LEN + rep[4] || n > WK_REPORT_LEN ||
			    !wk_frame_ok(rep)) {
				c->st.bad_replies++;
				last = WK_TO_IO;
				wait = grow(c, wait);
				continue;
			}
			if (rep[3] == WK_ST_BUSY) {
				c->st.busy++;
				last = WK_TO_BUSY;
				wait = grow(c, wait);
				continue;
			}
			if (rep[1] == req[1] && rep[2] == cmd && rep[5] == req[6] &&
			    rep[3] != WK_ST_NO_REPLY) {
				int n = rep[4];
				if (out) {
					if (n > outmax) n = outmax;
					memcpy(out, rep + WK_HDR_LEN, n);
				}
				if (outlen) *outlen = n;
				c->st.cmds++;
				return rep[3];
			}
			last = WK_TO_NO_MATCH;
			break;      /* the device is idle and lacks our request */
		}
		c->st.resends++;
		wait = 0;
	}
}

const char *wk_timeout_why(const struct wk_client *c)
{
	switch (c->timeout_why) {
	case WK_TO_BUSY:
		return "the Wolfskralle was still busy (with this command, or one "
		       "an earlier wolfctl left running): wait, then try again";
	case WK_TO_IO:
		return "USB transfers to the Wolfskralle failed (see dmesg)";
	case WK_TO_NO_MATCH:
		return "the Wolfskralle answered, but never to this request";
	default:
		return "";
	}
}

const char *wk_status_name(int st)
{
	switch (st) {
	case WK_ST_OK:        return "ok";
	case WK_ST_BUSY:      return "busy";
	case WK_ST_NO_REPLY:  return "no reply";
	case WK_ST_BAD_FRAME: return "bad frame";
	case WK_ST_BAD_CMD:   return "unknown command";
	case WK_ST_BAD_ARG:   return "bad argument";
	case WK_E_TIMEOUT:    return "timeout";
	case WK_E_ARG:        return "request too long";
	default:              return "unknown status";
	}
}
