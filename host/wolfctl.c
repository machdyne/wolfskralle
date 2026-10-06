/*
 * wolfctl: command-line tool for the Wolfskralle programmer.
 * See docs/wolfctl.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wkproto.h"
#include "wkclient.h"
#include "wkhid.h"
#include "swiocmd.h"
#include "dbgcmd.h"

static const char *usage_text =
"usage: wolfctl [-d /dev/hidrawN] [-s serial] [-v] command [args]\n"
"\n"
"  list                         the Wolfskralles attached\n"
"  info                         firmware, supply voltage, counters\n"
"  power [on|off|status] [-m ms]\n"
"                               target power; -m watches the supply for\n"
"                               ms milliseconds after switching (max 200)\n"
"  led on|off|auto              the white LED (auto: lit, dark on activity)\n"
"  ping [count] [-l bytes] [-w ms] [-p n]\n"
"                               round trips with random data, checked;\n"
"                               -l payload size (default: random 0..118),\n"
"                               -w the device waits ms in each,\n"
"                               -p power-cycle the target every n pings\n"
"\n"
"  swio on|off                  the SWIO pins (on: needs target power)\n"
"  swio-timing [name=ticks...]  show or set the SWIO timing\n"
"                               (period one one-pp zero read thresh mode stop)\n"
"  swio-test [count] [--load]   write and read back a debug register count\n"
"                               times; --load: with USB traffic in between\n"
"  swio-scan [count] [--apply]  find the working timing for this module\n"
"  swio-raw read REG | write REG VALUE\n"
"                               one packet, with every pulse measured\n"
"  dmi (r REG | w REG VALUE)... debug module registers\n"
"\n"
"  id                           connect, stop the chip, show what it is,\n"
"                               restart it\n"
"  read [ADDR [LEN]] [-o FILE]  read memory (default: the whole flash),\n"
"                               read it twice; hex dump, or to FILE\n"
"  crc [ADDR [LEN]]             CRC-32 of memory (default: the whole flash)\n"
"  resume                       restart the chip's program\n"
"  flash IMAGE.bin [--force]    program a module (Machdyne BASIC for LS10A\n"
"                               or LS11A; --force: any firmware)\n"
"  verify IMAGE.bin             does the module hold this image?\n"
"  profile [--reset]            where the Wolfskralle's time went\n"
"                               (flash -v shows it for the flash)\n"
"     id, read and crc: --attach  only talk to the running chip\n"
"                       --restart restart it first (v0: power cycle)\n"
"                       --halt    leave it stopped afterwards\n"
"     id, read, crc, flash, verify: --slow  without the fast paths\n";

struct wkhid hid;
struct wk_client cl;
int verbose;

static void usage(void)
{
	fputs(usage_text, stderr);
	exit(2);
}

void die_status(const char *what, int st)
{
	fprintf(stderr, "wolfctl: %s: %s", what, wk_status_name(st));
	if (hid.gone) fprintf(stderr, ": the Wolfskralle was unplugged");
	else if (st == WK_E_TIMEOUT) fprintf(stderr, ": %s", wk_timeout_why(&cl));
	fprintf(stderr, "\n");
	if (verbose) print_client_stats();
	exit(1);
}

static int cmd(uint8_t c, const void *in, int inlen, void *out, int outmax, int *outlen,
               unsigned timeout_ms)
{
	return wk_cmd(&cl, c, in, inlen, out, outmax, outlen, timeout_ms);
}

static int get_info(uint8_t *info)
{
	int n = 0;
	int st = cmd(WK_CMD_INFO, NULL, 0, info, WK_PAYLOAD_MAX, &n, 2000);
	if (st == WK_ST_OK && n < WK_INFO_SWIO) return WK_ST_BAD_FRAME;
	return st;
}

void print_client_stats(void)
{
	printf("host:   %u commands, %u sets, %u gets, %u busy, %u resends, "
	       "%u set errors, %u get errors, %u bad replies, %u timeouts\n",
	       cl.st.cmds, cl.st.sets, cl.st.gets, cl.st.busy, cl.st.resends,
	       cl.st.set_errors, cl.st.get_errors, cl.st.bad_replies, cl.st.timeouts);
}

/* ---- commands ---- */

static int do_list(void)
{
	struct wkhid_found f[16];
	int n = wkhid_list(f, 16);
	for (int i = 0; i < n; i++)
		printf("%s  serial %s\n", f[i].path, f[i].serial);
	if (!n) printf("no Wolfskralle found\n");
	return 0;
}

static int do_info(void)
{
	uint8_t i[WK_PAYLOAD_MAX];
	int n = 0;
	memset(i, 0, sizeof(i));
	int st = cmd(WK_CMD_INFO, NULL, 0, i, sizeof(i), &n, 2000);
	if (st == WK_ST_OK && n < WK_INFO_SWIO) st = WK_ST_BAD_FRAME;
	if (st) die_status("info", st);
	uint16_t fw = wk_get16(i + WK_INFO_FW);
	uint32_t up = wk_get32(i + WK_INFO_UPTIME);
	static const char *leds[] = { "off", "on", "auto" };
	printf("device:      %s, %s\n", (char *)i + WK_INFO_NAME, hid.path);
	printf("board:       v%u\n", i[WK_INFO_BOARD]);
	printf("firmware:    %u.%u (protocol %u)\n", fw >> 8, fw & 0xff, i[WK_INFO_PROTO]);
	printf("uptime:      %u.%03u s\n", up / 1000, up % 1000);
	printf("supply:      %u mV\n", wk_get16(i + WK_INFO_VDD));
	printf("target:      power %s\n", i[WK_INFO_TPOWER] ? "on" : "off");
	printf("led:         %s\n", i[WK_INFO_LED] <= 2 ? leds[i[WK_INFO_LED]] : "?");
	printf("swio:        %s\n", i[WK_INFO_SWIO] ? "on" : "off");
	if (n >= WK_INFO_RESET + 1) {
		static const char *why[] = { "power-on", "reset pin or debugger", "software",
		                             "WATCHDOG (the firmware hung: please report)", "other" };
		uint8_t r = i[WK_INFO_RESET];
		printf("started by:  %s\n", r <= 4 ? why[r] : "?");
	}
	printf("commands:    %u executed, %u repeats answered, %u bad, %u rejected while busy\n",
	       wk_get32(i + WK_INFO_CMDS), wk_get32(i + WK_INFO_DUPS),
	       wk_get32(i + WK_INFO_BAD), wk_get32(i + WK_INFO_REJECTED));
	if (i[WK_INFO_PROTO] != WK_PROTO_VERSION)
		printf("warning: protocol %u, this wolfctl speaks %u\n",
		       i[WK_INFO_PROTO], WK_PROTO_VERSION);
	return 0;
}

static int power(int op, unsigned mon_ms, int quiet)
{
	uint8_t in[3], out[WK_PAYLOAD_MAX];
	int n = 0;
	in[0] = (uint8_t)op;
	wk_put16(in + 1, (uint16_t)mon_ms);
	int st = cmd(WK_CMD_POWER, in, 3, out, sizeof(out), &n, 2000 + mon_ms);
	if (st) die_status("power", st);
	if (n < WK_PWR_LEN) die_status("power", WK_ST_BAD_FRAME);
	if (!quiet || verbose) {
		printf("target power %s, supply %u mV", out[WK_PWR_STATE] ? "on" : "off",
		       wk_get16(out + WK_PWR_VDD_BEFORE));
		if (wk_get16(out + WK_PWR_SAMPLES))
			printf(" before; then for %u ms: lowest %u mV, average %u mV (%u samples)",
			       mon_ms, wk_get16(out + WK_PWR_VDD_MIN), wk_get16(out + WK_PWR_VDD_AVG),
			       wk_get16(out + WK_PWR_SAMPLES));
		printf("\n");
	}
	return 0;
}

static int do_power(int argc, char **argv, unsigned mon_ms)
{
	int op = WK_POWER_QUERY;
	if (argc > 0) {
		if (!strcmp(argv[0], "on")) op = WK_POWER_ON;
		else if (!strcmp(argv[0], "off")) op = WK_POWER_OFF;
		else if (!strcmp(argv[0], "status")) op = WK_POWER_QUERY;
		else usage();
	}
	if (mon_ms > WK_POWER_MON_MAX_MS) {
		fprintf(stderr, "wolfctl: -m: at most %u ms\n", WK_POWER_MON_MAX_MS);
		return 2;
	}
	return power(op, mon_ms, 0);
}

static int do_led(int argc, char **argv)
{
	if (argc < 1) usage();
	uint8_t m;
	if (!strcmp(argv[0], "off")) m = WK_LED_OFF;
	else if (!strcmp(argv[0], "on")) m = WK_LED_ON;
	else if (!strcmp(argv[0], "auto")) m = WK_LED_AUTO;
	else usage();
	int st = cmd(WK_CMD_LED, &m, 1, NULL, 0, NULL, 2000);
	if (st) die_status("led", st);
	return 0;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int do_ping(int argc, char **argv, int len, unsigned wait_ms, unsigned cycle_every)
{
	unsigned count = argc > 0 ? (unsigned)strtoul(argv[0], NULL, 0) : 10;
	if (len > WK_PAYLOAD_MAX - 2 || wait_ms > WK_PING_DELAY_MAX) usage();

	uint8_t info0[WK_PAYLOAD_MAX], info1[WK_PAYLOAD_MAX];
	int st = get_info(info0);
	if (st) die_status("info", st);

	srand((unsigned)time(NULL));
	unsigned ok = 0, bad = 0, cycles = 0;
	double tmin = 1e9, tmax = 0, tsum = 0;
	uint64_t bytes = 0;
	double t0 = now_ms();

	for (unsigned i = 0; i < count; i++) {
		uint8_t in[WK_PAYLOAD_MAX], out[WK_PAYLOAD_MAX];
		int n = len >= 0 ? len : rand() % (WK_PAYLOAD_MAX - 1);
		wk_put16(in, (uint16_t)wait_ms);
		for (int k = 0; k < n; k++) in[2 + k] = (uint8_t)rand();
		int outlen = -1;
		double t = now_ms();
		st = cmd(WK_CMD_PING, in, n + 2, out, sizeof(out), &outlen, 2000 + wait_ms);
		t = now_ms() - t;
		if (st == WK_ST_OK && outlen == n && !memcmp(out, in + 2, n)) {
			ok++;
			bytes += 2 * (uint64_t)(n + 2);
		} else {
			bad++;
			fprintf(stderr, "ping %u: %s%s\n", i,
			        st ? wk_status_name(st) : "wrong data echoed",
			        hid.gone ? " (the Wolfskralle was unplugged)" : "");
			if (hid.gone) {
				print_client_stats();
				return 1;
			}
		}
		if (t < tmin) tmin = t;
		if (t > tmax) tmax = t;
		tsum += t;
		if (cycle_every && (i + 1) % cycle_every == 0) {
			power(WK_POWER_OFF, 20, 1);
			power(WK_POWER_ON, 50, 1);
			cycles++;
		}
		if (count >= 1000 && (i + 1) % (count / 10) == 0)
			fprintf(stderr, "  %u/%u, %u failed\n", i + 1, count, bad);
	}
	double total = now_ms() - t0;

	st = get_info(info1);
	if (st) die_status("info", st);

	unsigned done = ok + bad;
	printf("%u pings: %u ok, %u failed", done, ok, bad);
	if (cycles) printf(" (target power cycled %u times)", cycles);
	printf("\n");
	if (done)
		printf("round trip: min %.2f ms, average %.2f ms, max %.2f ms; "
		       "%.0f payload bytes/s\n", tmin, tsum / done, tmax,
		       total > 0 ? bytes * 1000.0 / total : 0.0);
	print_client_stats();
	printf("device: %u repeats answered, %u bad requests, %u rejected while busy "
	       "(during this run)\n",
	       wk_get32(info1 + WK_INFO_DUPS) - wk_get32(info0 + WK_INFO_DUPS),
	       wk_get32(info1 + WK_INFO_BAD) - wk_get32(info0 + WK_INFO_BAD),
	       wk_get32(info1 + WK_INFO_REJECTED) - wk_get32(info0 + WK_INFO_REJECTED));
	return bad ? 1 : 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
	const char *path = NULL, *serial = NULL;
	unsigned mon_ms = 0, wait_ms = 0, cycle_every = 0;
	int len = -1, load = 0, apply = 0, mode = 0, keep_halted = 0, force = 0;
	const char *outfile = NULL;
	char *args[48];
	int nargs = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int more = i + 1 < argc;
		if (!strcmp(a, "-d") && more) path = argv[++i];
		else if (!strcmp(a, "-s") && more) serial = argv[++i];
		else if (!strcmp(a, "-v")) verbose = 1;
		else if (!strcmp(a, "-m") && more) mon_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "-l") && more) len = atoi(argv[++i]);
		else if (!strcmp(a, "-w") && more) wait_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "-p") && more) cycle_every = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(a, "--load")) load = 1;
		else if (!strcmp(a, "--attach")) mode = (mode & WK_CN_MODE_SLOW) | 1;
		else if (!strcmp(a, "--restart")) mode = (mode & WK_CN_MODE_SLOW) | 2;
		else if (!strcmp(a, "--halt")) keep_halted = 1;
		else if (!strcmp(a, "--force")) force = 1;
		else if (!strcmp(a, "--slow")) mode |= WK_CN_MODE_SLOW;
		else if (!strcmp(a, "--reset") && i > 1) args[nargs < 48 ? nargs++ : 47] = "reset";
		else if (!strcmp(a, "-o") && more) outfile = argv[++i];
		else if (!strcmp(a, "--apply")) apply = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
		else if (a[0] == '-' && !(a[1] >= '0' && a[1] <= '9')) usage();
		else if (nargs < 48) args[nargs++] = argv[i];
	}
	if (!nargs) usage();
	if (!strcmp(args[0], "list")) return do_list();

	char err[200];
	if (wkhid_open(&hid, path, serial, err, sizeof(err))) {
		fprintf(stderr, "wolfctl: %s\n", err);
		return 1;
	}
	struct wk_transport t;
	wkhid_transport(&hid, &t);
	wk_client_init(&cl, &t);

	int r;
	if (!strcmp(args[0], "info")) r = do_info();
	else if (!strcmp(args[0], "power")) r = do_power(nargs - 1, args + 1, mon_ms);
	else if (!strcmp(args[0], "led")) r = do_led(nargs - 1, args + 1);
	else if (!strcmp(args[0], "ping")) r = do_ping(nargs - 1, args + 1, len, wait_ms, cycle_every);
	else if (!strcmp(args[0], "swio")) r = do_swio(nargs - 1, args + 1);
	else if (!strcmp(args[0], "swio-timing")) r = do_swio_timing(nargs - 1, args + 1);
	else if (!strcmp(args[0], "swio-test")) r = do_swio_test(nargs - 1, args + 1, load);
	else if (!strcmp(args[0], "swio-scan")) r = do_swio_scan(nargs - 1, args + 1, apply);
	else if (!strcmp(args[0], "swio-raw")) r = do_swio_raw(nargs - 1, args + 1);
	else if (!strcmp(args[0], "dmi")) r = do_dmi(nargs - 1, args + 1);
	else if (!strcmp(args[0], "id")) r = do_id(mode, keep_halted);
	else if (!strcmp(args[0], "read")) r = do_read(nargs - 1, args + 1, mode, outfile, keep_halted);
	else if (!strcmp(args[0], "crc")) r = do_crc(nargs - 1, args + 1, mode, keep_halted);
	else if (!strcmp(args[0], "resume")) r = do_resume();
	else if (!strcmp(args[0], "flash")) r = do_flash(nargs - 1, args + 1, mode, force);
	else if (!strcmp(args[0], "verify")) r = do_verify(nargs - 1, args + 1, mode);
	else if (!strcmp(args[0], "profile")) r = do_profile(nargs > 1 && !strcmp(args[1], "reset"));
	else usage();

	if (verbose && strcmp(args[0], "ping") && strcmp(args[0], "swio-test") &&
	    strcmp(args[0], "flash"))
		print_client_stats();
	wkhid_close(&hid);
	return r;
}
