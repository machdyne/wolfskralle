/*
 * proto_test: the device protocol (firmware/wkdev.c) against the host
 * client (host/wkclient.c), with faults injected on the simulated USB.
 *
 * The device's main loop runs in its own thread; the "interrupt" (the
 * USB callbacks) runs in the host's thread under a mutex, which is also
 * what wkdev's IRQ_OFF takes. Reports move 8 bytes at a time, as on the
 * wire, so the main loop can run between packets.
 *
 * Faults: SETUP lost, SET aborted halfway, SET corrupted, SET status
 * stage lost (the device has it, the host sees an error), SET sent twice,
 * GET lost, GET truncated, GET corrupted.
 *
 * Checks: every command completes with the right reply; every request
 * runs exactly once; every corruption is detected (none slips through,
 * none is invented); the fixed BUSY and NO_REPLY frames are valid.
 *
 *   proto_test [commands] [fault percent]
 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wkproto.h"
#include "wkdev.h"
#include "wkclient.h"

#define WVI_FEATURE (0x0300 | WK_REPORT_ID)

static pthread_mutex_t irq = PTHREAD_MUTEX_INITIALIZER;
void wk_test_irq_off(void) { pthread_mutex_lock(&irq); }
void wk_test_irq_on(void)  { pthread_mutex_unlock(&irq); }

static volatile int stop;
static unsigned *execs;              /* executions per request id */
static unsigned nreq;
static int failures;

#define CHECK(c, ...) do { if (!(c) && ++failures <= 10) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- the device ---- */

static unsigned dev_rand = 12345;
static unsigned dev_rnd(void) { dev_rand = dev_rand * 1103515245u + 12345u; return dev_rand >> 8; }

static uint8_t handler(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                       uint8_t *out, uint8_t *outlen)
{
	/* while a command runs, a GET must see the fixed BUSY frame */
	int n;
	pthread_mutex_lock(&irq);
	const uint8_t *f = wkdev_get_start(WVI_FEATURE, WK_REPORT_LEN, &n);
	pthread_mutex_unlock(&irq);
	CHECK(f && n == WK_HDR_LEN && wk_frame_ok(f) && f[3] == WK_ST_BUSY,
	      "no valid BUSY frame (8 bytes) while running");

	if (cmd != WK_CMD_PING) return WK_ST_BAD_CMD;
	if (inlen < 4) return WK_ST_BAD_ARG;
	uint32_t id = wk_get32(in);
	if (id < nreq) execs[id]++;
	unsigned r = dev_rnd() % 100;
	if (r < 30) usleep(dev_rnd() % 400);      /* sometimes a long command */
	memcpy(out, in, inlen);
	*outlen = inlen;
	return WK_ST_OK;
}

static void *device(void *arg)
{
	(void)arg;
	while (!stop)
		if (!wkdev_poll(handler)) sched_yield();
	return NULL;
}

/* ---- the simulated USB, with faults ---- */

static unsigned fault_pct = 5;
static unsigned host_rand = 777;
static unsigned rnd(void) { host_rand = host_rand * 1664525u + 1013904223u; return host_rand >> 8; }
static int fault(void) { return rnd() % 1000 < fault_pct * 10; }

static struct {
	unsigned setup_lost, set_abort, set_corrupt, set_status_lost, set_twice;
	unsigned get_lost, get_trunc, get_corrupt;
} inj;

static void set_raw(const uint8_t *buf, int len, int abort_at, int corrupt_at)
{
	pthread_mutex_lock(&irq);
	int maxlen = wkdev_set_start(WVI_FEATURE, len);
	pthread_mutex_unlock(&irq);
	for (int off = 0; off < len; off += 8) {
		if (off == abort_at) return;
		uint8_t pkt[8];
		int n = len - off < 8 ? len - off : 8;
		memcpy(pkt, buf + off, n);
		if (corrupt_at >= off && corrupt_at < off + n)
			pkt[corrupt_at - off] ^= (uint8_t)(1u << (rnd() % 8));
		pthread_mutex_lock(&irq);
		if (off < maxlen) wkdev_set_data(off, pkt, n);
		pthread_mutex_unlock(&irq);
		if (rnd() % 4 == 0) sched_yield();
	}
}

static int sim_set(void *ctx, const uint8_t *buf, int len)
{
	(void)ctx;
	if (fault()) {
		switch (rnd() % 5) {
		case 0: inj.setup_lost++; return -1;
		case 1: inj.set_abort++;
			set_raw(buf, len, 8 * (rnd() % (len / 8)), -1);
			return -1;
		case 2: inj.set_corrupt++;  /* within the bytes the CRC covers */
			set_raw(buf, len, -1, rnd() % (WK_HDR_LEN + buf[4]));
			return 0;
		case 3: inj.set_status_lost++;
			set_raw(buf, len, -1, -1);
			return -1;
		case 4: inj.set_twice++;    /* as if the host retried too soon */
			set_raw(buf, len, -1, -1);
			set_raw(buf, len, -1, -1);
			return 0;
		}
	}
	set_raw(buf, len, -1, -1);
	return 0;
}

static int sim_get(void *ctx, uint8_t *buf, int len)
{
	(void)ctx;
	int n, kind = fault() ? (int)(rnd() % 3) : -1;
	if (kind == 0) { inj.get_lost++; return -1; }
	pthread_mutex_lock(&irq);
	const uint8_t *f = wkdev_get_start(WVI_FEATURE, len, &n);
	pthread_mutex_unlock(&irq);
	if (!f) return -1;
	for (int off = 0; off < n; off += 8) {
		pthread_mutex_lock(&irq);
		memcpy(buf + off, f + off, n - off < 8 ? n - off : 8);
		pthread_mutex_unlock(&irq);
		if (rnd() % 4 == 0) sched_yield();
	}
	if (kind == 1) { inj.get_trunc++; return 8 * (rnd() % (n / 8)); }
	if (kind == 2) {
		inj.get_corrupt++;
		buf[1 + rnd() % (WK_HDR_LEN - 1 + buf[4])] ^= (uint8_t)(1u << (rnd() % 8));
	}
	return n;
}

static void sim_sleep(void *ctx, unsigned us) { (void)ctx; usleep(us); }
static uint64_t sim_now(void *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

/* ---- the test ---- */

int main(int argc, char **argv)
{
	nreq = argc > 1 ? (unsigned)atoi(argv[1]) : 5000;
	fault_pct = argc > 2 ? (unsigned)atoi(argv[2]) : 5;
	execs = calloc(nreq, sizeof(*execs));

	/* before any command: the NO_REPLY frame */
	int n;
	const uint8_t *f = wkdev_get_start(WVI_FEATURE, WK_REPORT_LEN, &n);
	CHECK(f && n == WK_HDR_LEN && wk_frame_ok(f) && f[3] == WK_ST_NO_REPLY,
	      "no valid NO_REPLY frame (8 bytes) at start");
	/* reports other than ours are ignored */
	CHECK(wkdev_set_start(0x03fd, WK_REPORT_LEN) == 0, "accepted report 0xFD");
	CHECK(wkdev_set_start(0x0201, WK_REPORT_LEN) == 0, "accepted an output report");

	pthread_t th;
	pthread_create(&th, NULL, device, NULL);

	struct wk_transport t = { sim_set, sim_get, sim_sleep, sim_now, NULL };
	struct wk_client c;
	wk_client_init(&c, &t);
	c.poll_min_us = 20;
	c.poll_max_us = 200;

	uint8_t in[WK_PAYLOAD_MAX], out[WK_PAYLOAD_MAX];
	for (unsigned i = 0; i < nreq; i++) {
		int len = 4 + rnd() % (WK_PAYLOAD_MAX - 3);
		wk_put32(in, i);
		for (int k = 4; k < len; k++) in[k] = (uint8_t)rnd();
		int outlen = -1;
		int st = wk_cmd(&c, WK_CMD_PING, in, len, out, sizeof(out), &outlen, 5000);
		CHECK(st == WK_ST_OK, "request %u: %s", i, wk_status_name(st));
		CHECK(outlen == len && !memcmp(in, out, len), "request %u: wrong echo", i);
	}

	/* an unknown command is answered, not ignored */
	int st = wk_cmd(&c, 0x7e, NULL, 0, NULL, 0, NULL, 5000);
	CHECK(st == WK_ST_BAD_CMD, "unknown command: %s", wk_status_name(st));

	stop = 1;
	pthread_join(th, NULL);

	unsigned once = 0;
	for (unsigned i = 0; i < nreq; i++) {
		if (execs[i] == 1) once++;
		else CHECK(0, "request %u ran %u times", i, execs[i]);
	}

	CHECK(wkdev_count.bad == inj.set_corrupt,
	      "device saw %u bad requests, %u were corrupted", wkdev_count.bad, inj.set_corrupt);
	CHECK(c.st.bad_replies == inj.get_trunc + inj.get_corrupt,
	      "host saw %u bad replies, %u were damaged", c.st.bad_replies,
	      inj.get_trunc + inj.get_corrupt);
	CHECK(wkdev_count.cmds == nreq + 1, "device ran %u commands", wkdev_count.cmds);

	printf("%u commands, faults %u%%: each ran once: %u\n", nreq, fault_pct, once);
	printf("  injected: setup lost %u, set aborted %u, set corrupted %u, "
	       "set status lost %u, set twice %u, get lost %u, get truncated %u, "
	       "get corrupted %u\n",
	       inj.setup_lost, inj.set_abort, inj.set_corrupt, inj.set_status_lost,
	       inj.set_twice, inj.get_lost, inj.get_trunc, inj.get_corrupt);
	printf("  host: sets %u, gets %u, busy %u, resends %u, bad replies %u, timeouts %u\n",
	       c.st.sets, c.st.gets, c.st.busy, c.st.resends, c.st.bad_replies, c.st.timeouts);
	printf("  device: commands %u, repeats answered %u, bad %u, rejected while busy %u\n",
	       wkdev_count.cmds, wkdev_count.dups, wkdev_count.bad, wkdev_count.rejected);
	printf(failures ? "FAILED (%d)\n" : "passed\n", failures);
	return failures ? 1 : 0;
}
