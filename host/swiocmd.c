/*
 * wolfctl: the SWIO commands (phase 2). See docs/wolfctl.md, docs/swio.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wkproto.h"
#include "wkclient.h"
#include "swiocmd.h"

#define NS(t) ((t) * 1000.0 / 48.0)     /* timer ticks to ns */

static const char *tm_names[WK_TM_LEN] = {
	"period", "one", "one-pp", "zero", "read", "thresh", "mode", "stop"
};

static int get_timing(uint8_t *tm)
{
	int n = 0;
	int st = wk_cmd(&cl, WK_CMD_SWIO_TIMING, NULL, 0, tm, WK_TM_LEN, &n, 2000);
	if (st == WK_ST_OK && n != WK_TM_LEN) st = WK_ST_BAD_FRAME;
	return st;
}

static int set_timing(const uint8_t *tm)
{
	uint8_t back[WK_TM_LEN];
	int n = 0;
	return wk_cmd(&cl, WK_CMD_SWIO_TIMING, tm, WK_TM_LEN, back, sizeof(back), &n, 2000);
}

static void print_timing(const uint8_t *tm)
{
	for (int i = 0; i < WK_TM_LEN; i++) {
		printf("  %-7s %3u", tm_names[i], tm[i]);
		if (i == WK_TM_MODE)
			printf("      (%s)\n", tm[i] ? "write packets push-pull" : "open drain");
		else if (i == WK_TM_STOP_US)
			printf(" us\n");
		else
			printf(" ticks %4.0f ns\n", NS(tm[i]));
	}
}

int do_swio(int argc, char **argv)
{
	if (argc < 1) {
		fprintf(stderr, "usage: wolfctl swio on|off\n");
		return 2;
	}
	uint8_t on = !strcmp(argv[0], "on");
	if (!on && strcmp(argv[0], "off")) {
		fprintf(stderr, "usage: wolfctl swio on|off\n");
		return 2;
	}
	uint8_t out[4];
	int n = 0;
	int st = wk_cmd(&cl, WK_CMD_SWIO, &on, 1, out, sizeof(out), &n, 2000);
	if (st) die_status("swio", st);
	printf("swio %s\n", n && out[0] ? "on" : "off");
	return 0;
}

int do_swio_timing(int argc, char **argv)
{
	uint8_t tm[WK_TM_LEN];
	int st = get_timing(tm);
	if (st) die_status("swio-timing", st);
	for (int a = 0; a < argc; a++) {
		char *eq = strchr(argv[a], '=');
		int k = -1;
		if (eq)
			for (int i = 0; i < WK_TM_LEN; i++)
				if ((size_t)(eq - argv[a]) == strlen(tm_names[i]) &&
				    !strncmp(argv[a], tm_names[i], eq - argv[a])) k = i;
		if (k < 0) {
			fprintf(stderr, "wolfctl: swio-timing: %s: use name=ticks, names: "
			        "period one one-pp zero read thresh mode stop\n", argv[a]);
			return 2;
		}
		tm[k] = (uint8_t)strtoul(eq + 1, NULL, 0);
	}
	if (argc) {
		st = set_timing(tm);
		if (st == WK_ST_BAD_ARG) {
			fprintf(stderr, "wolfctl: swio-timing: refused (needs one, one-pp, read < "
			        "zero <= period-2, read < thresh < period, 20 <= period <= 250, "
			        "mode 0 or 1, 1 <= stop <= 200)\n");
			return 1;
		}
		if (st) die_status("swio-timing", st);
		if ((st = get_timing(tm))) die_status("swio-timing", st);
	}
	printf("swio timing (1 tick = 20.8 ns):\n");
	print_timing(tm);
	return 0;
}

/* ---- the link test ---- */

struct lt {
	uint8_t result;
	uint32_t cfgr, done, errors, incomplete, want, got;
	uint8_t min[6], max[6];
};

/* one SWIO_TEST command: keep it short (LT_CHUNK round trips, well under
 * a second), so the host's timeout never decides a test */
#define LT_CHUNK 1000

static int link_test_one(uint32_t n, struct lt *r)
{
	uint8_t in[4], out[WK_PAYLOAD_MAX];
	int len = 0;
	wk_put32(in, n);
	/* a round trip takes about 0.2-0.4 ms; allow 2 ms */
	int st = wk_cmd(&cl, WK_CMD_SWIO_TEST, in, 4, out, sizeof(out), &len, 3000 + 2 * n);
	if (st) return st;
	if (len < WK_LT_LEN) return WK_ST_BAD_FRAME;
	r->result = out[WK_LT_RESULT];
	r->cfgr = wk_get32(out + WK_LT_CFGR);
	r->done = wk_get32(out + WK_LT_DONE);
	r->errors = wk_get32(out + WK_LT_ERRORS);
	r->incomplete = wk_get32(out + WK_LT_INCOMPLETE);
	r->want = wk_get32(out + WK_LT_BAD_WANT);
	r->got = wk_get32(out + WK_LT_BAD_GOT);
	for (int k = 0; k < 6; k++) {
		r->min[k] = out[WK_LT_W1 + 2 * k];
		r->max[k] = out[WK_LT_W1 + 2 * k + 1];
	}
	return 0;
}

static void lt_merge(struct lt *a, const struct lt *b);

/* n round trips, in commands of at most LT_CHUNK */
static int link_test(uint32_t n, struct lt *r)
{
	struct lt one;
	memset(r, 0, sizeof(*r));
	memset(r->min, 0xff, sizeof(r->min));
	uint32_t done = 0;
	do {
		uint32_t k = n - done < LT_CHUNK ? n - done : LT_CHUNK;
		int st = link_test_one(k, &one);
		if (st) return st;
		r->result = one.result;
		r->cfgr = one.cfgr;
		if (one.result) return 0;
		lt_merge(r, &one);
		done += one.done;
		if (!one.done) break;
	} while (done < n);
	return 0;
}

static void lt_merge(struct lt *a, const struct lt *b)
{
	if (!a->errors && b->errors) { a->want = b->want; a->got = b->got; }
	a->done += b->done;
	a->errors += b->errors;
	a->incomplete += b->incomplete;
	for (int k = 0; k < 6; k++) {
		if (b->min[k] < a->min[k]) a->min[k] = b->min[k];
		if (b->max[k] > a->max[k]) a->max[k] = b->max[k];
	}
}

static void print_pulses(const struct lt *r)
{
	static const char *kind[6] = {
		"written 1s", "written 0s", "read header 1s", "read header 0s",
		"read 1s", "read 0s"
	};
	printf("measured low times, falling edge to line high again "
	       "(ticks, ns):\n");
	for (int k = 0; k < 6; k++) {
		if (r->min[k] > r->max[k]) continue;    /* none seen */
		printf("  %-15s %3u-%-3u  %4.0f-%-4.0f\n", kind[k], r->min[k], r->max[k],
		       NS(r->min[k]), NS(r->max[k]));
	}
}

static void no_chip(const struct lt *r)
{
	printf("no chip answered: the configuration register read %08x%s\n", r->cfgr,
	       r->cfgr == 0xffffffffu ? " (nothing pulls the line low: no module, or "
	                                "not in the contacts)" :
	       !r->cfgr ? " (the line is held low)" : "");
}

int do_swio_test(int argc, char **argv, int load)
{
	uint32_t n = argc > 0 ? (uint32_t)strtoul(argv[0], NULL, 0) : 1000;
	struct lt total, r;
	memset(&total, 0, sizeof(total));
	memset(total.min, 0xff, sizeof(total.min));

	/* with load: short tests with pings in between, so that USB traffic
	 * and SWIO packets overlap all the time */
	uint32_t chunk = load ? 200 : LT_CHUNK;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	unsigned pings = 0, ping_bad = 0;
	if (load) { cl.poll_min_us = 0; cl.poll_max_us = 0; }

	for (uint32_t done = 0; done < n; ) {
		uint32_t k = n - done < chunk ? n - done : chunk;
		int st = link_test_one(k, &r);
		if (st) die_status("swio-test", st);
		if (r.result) { no_chip(&r); return 1; }
		lt_merge(&total, &r);
		done += r.done;
		if (load) {
			uint8_t in[WK_PAYLOAD_MAX], out[WK_PAYLOAD_MAX];
			int len = 0;
			memset(in, 0, 2);
			for (int i = 2; i < WK_PAYLOAD_MAX; i++) in[i] = (uint8_t)rand();
			st = wk_cmd(&cl, WK_CMD_PING, in, WK_PAYLOAD_MAX, out, sizeof(out), &len, 2000);
			pings++;
			if (st || len != WK_PAYLOAD_MAX - 2 || memcmp(out, in + 2, len)) ping_bad++;
		}
		if (n >= 20000 && done % 10000 < chunk)
			fprintf(stderr, "  %u/%u\n", done, n);
		if (!r.done) break;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	printf("%u round trips: %u wrong, %u packets incomplete (%.1f s, %.0f us each)\n",
	       total.done, total.errors, total.incomplete, secs,
	       total.done ? secs * 1e6 / total.done : 0.0);
	if (total.errors)
		printf("first wrong: wrote %08x, read %08x\n", total.want, total.got);
	if (load) printf("%u pings in between: %u failed\n", pings, ping_bad);
	print_pulses(&total);
	if (verbose) print_client_stats();
	return total.errors || total.incomplete || ping_bad ? 1 : 0;
}

/* ---- one packet, raw ---- */

int do_swio_raw(int argc, char **argv)
{
	if (argc < 2 || (strcmp(argv[0], "read") && strcmp(argv[0], "write")) ||
	    (!strcmp(argv[0], "write") && argc < 3)) {
		fprintf(stderr, "usage: wolfctl swio-raw read REG | write REG VALUE\n");
		return 2;
	}
	int wr = !strcmp(argv[0], "write");
	uint8_t in[6], out[WK_PAYLOAD_MAX], tm[WK_TM_LEN];
	int n = 0;
	in[0] = wr ? WK_DMI_WRITE : WK_DMI_READ;
	in[1] = (uint8_t)strtoul(argv[1], NULL, 0);
	wk_put32(in + 2, wr ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
	int st = get_timing(tm);
	if (st) die_status("swio-timing", st);
	st = wk_cmd(&cl, WK_CMD_SWIO_RAW, in, 6, out, sizeof(out), &n, 2000);
	if (st) die_status("swio-raw", st);
	int ncap = out[5];
	printf("%s register 0x%02x: %s", wr ? "write" : "read", in[1],
	       out[0] ? "complete" : "INCOMPLETE");
	if (!wr && out[0]) printf(", value %08x", wk_get32(out + 1));
	printf("\n%d captures (expected %d), ticks from the falling edge to high:\n",
	       ncap, WK_SWIO_BITS);
	for (int i = 0; i < ncap && i < 48; i++) {
		uint8_t c = out[6 + i];
		const char *what = i == 0 ? "start" : i < 8 ? "reg" : i == 8 ? "r/w" : "data";
		printf("  %2d %-5s %3u %5.0f ns", i, what, c, NS(c));
		if (!wr && i >= 9) printf("  -> %d", c < tm[WK_TM_THRESH]);
		printf("\n");
	}
	return out[0] ? 0 : 1;
}

/* ---- debug registers ---- */

int do_dmi(int argc, char **argv)
{
	uint8_t in[WK_PAYLOAD_MAX], out[WK_PAYLOAD_MAX];
	int nops = 0, len = 0, a = 0;
	uint8_t regs[WK_DMI_MAX], ops[WK_DMI_MAX];
	while (a < argc && nops < WK_DMI_MAX) {
		uint8_t *op = in + 6 * nops;
		if (!strcmp(argv[a], "r") && a + 1 < argc) {
			op[0] = WK_DMI_READ;
			op[1] = (uint8_t)strtoul(argv[a + 1], NULL, 0);
			wk_put32(op + 2, 0);
			a += 2;
		} else if (!strcmp(argv[a], "w") && a + 2 < argc) {
			op[0] = WK_DMI_WRITE;
			op[1] = (uint8_t)strtoul(argv[a + 1], NULL, 0);
			wk_put32(op + 2, (uint32_t)strtoul(argv[a + 2], NULL, 0));
			a += 3;
		} else {
			break;
		}
		ops[nops] = op[0];
		regs[nops] = op[1];
		nops++;
	}
	if (!nops || a < argc) {
		fprintf(stderr, "usage: wolfctl dmi (r REG | w REG VALUE)... (at most %d)\n",
		        WK_DMI_MAX);
		return 2;
	}
	int st = wk_cmd(&cl, WK_CMD_DMI, in, 6 * nops, out, sizeof(out), &len, 2000);
	if (st) die_status("dmi", st);
	int done = out[0], p = 2;
	for (int i = 0; i < done; i++) {
		if (ops[i] == WK_DMI_READ) {
			printf("r 0x%02x = %08x\n", regs[i], wk_get32(out + p));
			p += 4;
		} else {
			printf("w 0x%02x\n", regs[i]);
		}
	}
	if (out[1]) {
		printf("operation %d (register 0x%02x): the packet came back incomplete\n",
		       done + 1, regs[done]);
		return 1;
	}
	return 0;
}

/* ---- the timing scan ---- */

/* errors of a short link test with one timing (-1: no chip) */
static long try_timing(const uint8_t *tm, uint32_t n, struct lt *r)
{
	int st = set_timing(tm);
	if (st == WK_ST_BAD_ARG) return -2;
	if (st) die_status("swio-timing", st);
	st = link_test(n, r);
	if (st) die_status("swio-test", st);
	if (r->result) return -1;
	return (long)r->errors + r->incomplete;
}

/* the middle of the longest run of passing values */
static int middle(const long *err, int lo, int hi, int *a, int *b)
{
	int best = 0, bs = -1;
	for (int i = lo; i <= hi; ) {
		if (err[i] != 0) { i++; continue; }
		int j = i;
		while (j + 1 <= hi && err[j + 1] == 0) j++;
		if (j - i + 1 > best) { best = j - i + 1; bs = i; }
		i = j + 1;
	}
	if (bs < 0) return -1;
	*a = bs;
	*b = bs + best - 1;
	return bs + (best - 1) / 2;
}

/* one row of a scan: the value, the errors, the measured pulses */
static void scan_row(int v, long e, const struct lt *r, int show_read)
{
	printf("   %5d %5.0f  ", v, NS(v));
	if (e == -1) { printf("no answer\n"); return; }
	if (e == -2) { printf("refused\n"); return; }
	printf("%6ld", e);
	if (r->min[0] <= r->max[0])
		printf("  %5.0f-%-5.0f", NS(r->min[0]), NS(r->max[0]));
	else
		printf("  %11s", "-");
	if (show_read && r->min[4] <= r->max[4] && r->min[5] <= r->max[5])
		printf("     %3u-%-3u / %3u-%-3u", r->min[4], r->max[4], r->min[5], r->max[5]);
	printf("\n");
}

/* a scan without measurements, printed as runs: "25-55: 0 errors" */
static void print_runs(const long *err, const long *shown, int lo, int hi)
{
	for (int i = lo; i <= hi; ) {
		int j = i;
		while (j + 1 <= hi && shown[j + 1] == shown[i]) j++;
		printf("   %3d-%-3d %5.0f-%-5.0f ns  ", i, j, NS(i), NS(j));
		if (shown[i] == -1) printf("no answer\n");
		else if (shown[i] == -2) printf("refused\n");
		else if (i == j) printf("%ld errors\n", err[i]);
		else printf("%s\n", shown[i] ? "errors" : "0 errors");
		i = j + 1;
	}
}

/* if the scan stops halfway (an error exits), put the timing back */
static uint8_t scan_orig[WK_TM_LEN];
static int scan_restore;

static void scan_atexit(void)
{
	if (!scan_restore) return;
	scan_restore = 0;
	if (set_timing(scan_orig))
		fprintf(stderr, "wolfctl: could not put the timing back: "
		        "restart the Wolfskralle (unplug it)\n");
	else
		fprintf(stderr, "wolfctl: the timing is as it was before the scan\n");
}

int do_swio_scan(int argc, char **argv, int apply)
{
	uint32_t n = argc > 0 ? (uint32_t)strtoul(argv[0], NULL, 0) : 300;
	uint8_t orig[WK_TM_LEN], tm[WK_TM_LEN];
	struct lt r;
	long err[256], shown[256];
	int a, b;
	int st = get_timing(orig);
	if (st) die_status("swio-timing", st);
	memcpy(tm, orig, sizeof(tm));
	memcpy(scan_orig, orig, sizeof(orig));
	scan_restore = 1;
	atexit(scan_atexit);

	/* is anything there at all, with the current timing? */
	if (try_timing(tm, 20, &r) == -1) {
		no_chip(&r);
		set_timing(orig);
		return 1;
	}

	/* 1. the low time of a 1 in writes and packet headers; the read
	 * slots keep their pulse and threshold, so only the 1s change */
	int is_pp = tm[WK_TM_MODE] == 1;
	int field = is_pp ? WK_TM_ONE_PP : WK_TM_ONE_OD;
	printf("1. the low time of a 1 (%s; %u round trips each):\n",
	       is_pp ? "push-pull" : "open drain", n);
	printf("   ticks    ns  errors  measured 1s (ns)\n");
	int hi = tm[WK_TM_ZERO] - 1;
	if (hi > 30) hi = 30;
	for (int v = 1; v <= hi; v++) {
		tm[field] = (uint8_t)v;
		long e = try_timing(tm, n, &r);
		err[v] = e < 0 ? 999999 : e;
		scan_row(v, e, &r, 0);
	}
	int one = middle(err, 1, hi, &a, &b);
	if (one < 0) {
		printf("no low time worked: see docs/swio.md (troubleshooting)\n");
		set_timing(orig);
		return 1;
	}
	printf("   works from %d to %d ticks (%.0f-%.0f ns)%s: choose %d (%.0f ns)\n",
	       a, b, NS(a), NS(b), a == 1 ? " (or shorter)" : "", one, NS(one));
	tm[field] = (uint8_t)one;

	/* 2. the pulse that starts a read slot, with the threshold as it is */
	printf("2. the pulse that starts a read slot (threshold %u ticks):\n",
	       tm[WK_TM_THRESH]);
	printf("   ticks    ns  errors  measured 1s (ns)  read 1s / 0s (ticks)\n");
	int rhi = tm[WK_TM_THRESH] - 1;
	if (rhi > tm[WK_TM_ZERO] - 1) rhi = tm[WK_TM_ZERO] - 1;
	for (int v = 0; v < 256; v++) err[v] = 999999;
	for (int v = 1; v <= rhi; v++) {
		tm[WK_TM_READ] = (uint8_t)v;
		long e = try_timing(tm, n, &r);
		err[v] = e < 0 ? 999999 : e;
		scan_row(v, e, &r, 1);
	}
	int rd = middle(err, 1, rhi, &a, &b);
	if (rd < 0) {
		printf("   no read pulse worked\n");
		set_timing(orig);
		return 1;
	}
	/* a short pulse leaves the most room below the threshold: take the
	 * lower third of the range */
	rd = a + (b - a) / 3;
	printf("   works from %d to %d ticks (%.0f-%.0f ns)%s: choose %d (%.0f ns)\n",
	       a, b, NS(a), NS(b), a == 1 ? " (or shorter)" : "", rd, NS(rd));
	tm[WK_TM_READ] = (uint8_t)rd;

	/* 3. the read threshold, from where the read 1s and 0s fall */
	try_timing(tm, 2000, &r);
	int thresh = tm[WK_TM_THRESH];
	if (r.min[4] <= r.max[4] && r.min[5] <= r.max[5] && r.max[4] + 1 < r.min[5]) {
		thresh = (r.max[4] + r.min[5] + 1) / 2;
		printf("3. read 1s rise at %u-%u ticks, 0s at %u-%u: threshold %d "
		       "(margin %d ticks each side)\n", r.min[4], r.max[4], r.min[5], r.max[5],
		       thresh, (r.min[5] - r.max[4]) / 2);
	} else {
		printf("3. read 1s and 0s overlap or were not seen: threshold stays %d\n", thresh);
	}
	tm[WK_TM_THRESH] = (uint8_t)thresh;

	/* 4. the low time of a 0 */
	printf("4. the low time of a 0 (%u round trips each):\n", n);
	int zlo = tm[WK_TM_ONE_OD];
	if (tm[WK_TM_ONE_PP] > zlo) zlo = tm[WK_TM_ONE_PP];
	if (tm[WK_TM_READ] > zlo) zlo = tm[WK_TM_READ];
	zlo++;
	int zhi = tm[WK_TM_PERIOD] - 2;
	for (int v = 0; v < 256; v++) err[v] = 999999;
	for (int v = zlo; v <= zhi; v++) {
		tm[WK_TM_ZERO] = (uint8_t)v;
		long e = try_timing(tm, n, &r);
		err[v] = e < 0 ? 999999 : e;
		shown[v] = e;
		if (e > 0) shown[v] = 1;
		if (verbose) scan_row(v, e, &r, 0);
	}
	if (!verbose) print_runs(err, shown, zlo, zhi);
	int zero = middle(err, zlo, zhi, &a, &b);
	if (zero < 0) {
		printf("   no low time worked\n");
		set_timing(orig);
		return 1;
	}
	printf("   works from %d to %d ticks (%.0f-%.0f ns): choose %d (%.0f ns)\n",
	       a, b, NS(a), NS(b), zero, NS(zero));
	tm[WK_TM_ZERO] = (uint8_t)zero;

	/* 5. the result, checked */
	long e = try_timing(tm, 5000, &r);
	printf("5. checked with 5000 round trips: %ld errors\n", e);
	print_timing(tm);
	scan_restore = 0;
	if (apply && e == 0) {
		printf("applied (until the Wolfskralle restarts)\n");
	} else {
		set_timing(orig);
		printf(apply ? "not applied: the check failed\n"
		             : "not applied (use --apply, or wolfctl swio-timing)\n");
	}
	return e == 0 ? 0 : 1;
}
