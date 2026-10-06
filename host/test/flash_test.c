/*
 * flash_test: programming, end to end: wolfctl's job (host/flash.c) and
 * client (wkclient.c), the Wolfskralle's protocol (wkdev.c), command
 * handlers (dbgcmds.c) and debug module code (ch32dbg.c), and a simulated
 * CH32V003/CH32V005 (chipsim.h) whose flash controller counts anything the
 * failsafe rules forbid. Every test requires zero violations.
 *
 * Power cuts: at random debug transactions, the power goes (pages being
 * erased or written are torn), the Wolfskralle restarts, and the next
 * flash must succeed.
 */

#include <stdio.h>
#include "wkproto.h"
#include "wkdev.h"
#include "wkclient.h"
#include "ch32dbg.h"
#include "dbgcmds.h"
#include "flash.h"
#include "chipsim.h"

void wk_test_irq_off(void) { }
void wk_test_irq_on(void) { }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the simulated Wolfskralle, run as each request arrives ---- */

static uint8_t handler(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                       uint8_t *out, uint8_t *outlen)
{
	uint8_t st;
	if (dbgcmd_handle(cmd, in, inlen, out, outlen, &st)) return st;
	return WK_ST_BAD_CMD;
}

int dbgcmd_swio_ready(void) { return 1; }
uint32_t dbg_us(void) { return 0; }

static uint64_t fake_us;

static int t_set(void *ctx, const uint8_t *buf, int len)
{
	(void)ctx;
	int max = wkdev_set_start(0x0300 | WK_REPORT_ID, len);
	for (int off = 0; off < len; off += 8)
		if (off < max) wkdev_set_data(off, buf + off, len - off < 8 ? len - off : 8);
	wkdev_poll(handler);
	return 0;
}

static int t_get(void *ctx, uint8_t *buf, int len)
{
	(void)ctx;
	int n;
	const uint8_t *f = wkdev_get_start(0x0300 | WK_REPORT_ID, len, &n);
	if (!f) return -1;
	memcpy(buf, f, n);
	fake_us += 1000;
	return n;
}

static void t_sleep(void *ctx, unsigned us) { (void)ctx; fake_us += us; }
static uint64_t t_now(void *ctx) { (void)ctx; return fake_us; }

static struct wk_client cl;

static void wolfskralle_restart(void)
{
	wkdev_reset();
	dbg_halted = dbg_unlocked = 0;
	dbg_chip = 0;
	dbg_flash_size = dbg_page = 0;
	struct wk_transport t = { t_set, t_get, t_sleep, t_now, NULL };
	wk_client_init(&cl, &t);
}

/* ---- images ---- */

static uint8_t img[32768];

static uint32_t make_image(uint32_t len, const char *mod)
{
	for (uint32_t i = 0; i < len; i++) img[i] = (uint8_t)rand();
	if (mod) {
		char id[48];
		snprintf(id, sizeof(id), "fw=Machdyne BASIC\nmod=%s\n", mod);
		memcpy(img + len / 2, id, strlen(id));
	}
	return len;
}

static int flash_is(uint32_t len)
{
	for (uint32_t i = 0; i < chip.size; i++)
		if (chip.flash[i] != (i < len ? img[i] : 0xFF)) return 0;
	return 1;
}

static int run(uint32_t len, int force, struct flash_job *j)
{
	memset(j, 0, sizeof(*j));
	j->img = img;
	j->len = len;
	j->force = force;
	return flash_run(&cl, j);
}

static uint8_t before[SIM_FLASH_MAX];
static void keep(void) { memcpy(before, chip.flash, chip.size); }
static int unchanged(void) { return !memcmp(before, chip.flash, chip.size); }

int main(void)
{
	struct flash_job j;
	int r;
	srand(5);
	wolfskralle_restart();

	/* 1. a CH32V003 (LS10A): written, verified, locked, running */
	chip_new(0, 0x11);
	uint32_t len = make_image(12000, "LS10A");
	long d0 = sim_dmi;
	r = run(len, 0, &j);
	long normal_dmi = sim_dmi - d0;
	CHECK(r == FL_OK, "v003: %s", flash_message(r));
	CHECK(flash_is(len), "v003: flash differs");
	CHECK(chip.locked && chip.running && !chip.halted, "v003: locked and running after");
	CHECK(j.written == 256 && j.unchanged == 0, "v003: %u written, %u unchanged",
	      j.written, j.unchanged);
	printf("  v003, 12000 bytes: %ld debug transactions, %u USB commands\n",
	       normal_dmi, cl.st.cmds);

	/* 2. the same again: every page already right, nothing erased */
	long e0 = sim_erases;
	r = run(len, 0, &j);
	CHECK(r == FL_OK && flash_is(len) && sim_erases == e0 && j.unchanged == 256,
	      "again: %s, %ld erases, %u unchanged", flash_message(r), sim_erases - e0, j.unchanged);

	/* 2b. one byte changed in the middle of a page: only that page is
	 * written again */
	chip.flash[5000 + 37] ^= 0x10;
	e0 = sim_erases;
	r = run(len, 0, &j);
	CHECK(r == FL_OK && flash_is(len) && sim_erases == e0 + 1 && j.written == 1,
	      "one byte off: %s, %ld erases, %u written", flash_message(r), sim_erases - e0,
	      j.written);

	/* 3. a full 16 KB image, and a factory-new (blank) chip */
	chip_new(0, 0x22);
	memset(chip.flash, 0xFF, chip.size);
	len = make_image(16384, "LS10A");
	CHECK(run(len, 0, &j) == FL_OK && flash_is(len), "full 16 KB on a blank chip");

	/* 4. a CH32V005 (LS11A), 256-byte pages */
	chip_new(1, 0x33);
	len = make_image(20000, "LS11A");
	r = run(len, 0, &j);
	CHECK(r == FL_OK && flash_is(len), "v005: %s", flash_message(r));
	CHECK(j.pages == 128, "v005: %u pages", j.pages);

	/* 4b. the fast paths: in use on a chip that has them; and a chip
	 * without them (a one-word program buffer, no auto-execution, or an
	 * auto-execution that misbehaves) is still written right, the slow
	 * way */
	chip_new(0, 0x34);
	len = make_image(12000, "LS10A");
	CHECK(run(len, 0, &j) == FL_OK && flash_is(len) && dbg_fast == (DBG_FAST_READ | DBG_FAST_WRITE),
	      "fast paths in use (%d)", dbg_fast);
	chip_new(1, 0x3F);
	len = make_image(20000, "LS11A");
	memset(&j, 0, sizeof(j));
	j.img = img;
	j.len = len;
	j.mode = WK_CN_MODE_SLOW;
	r = flash_run(&cl, &j);
	CHECK(r == FL_OK && flash_is(len) && dbg_fast == 0, "--slow: %s", flash_message(r));
	for (int k = 0; k < 3; k++) {
		chip_new(k == 2, 0x35 + (uint32_t)k);
		if (k == 0) chip.progbufsize = 1;
		if (k == 1) chip.no_autoexec = 1;
		if (k == 2) chip.wrong_autoexec = 1;
		len = make_image(k == 2 ? 20000 : 12000, k == 2 ? "LS11A" : "LS10A");
		long v0 = sim_violations;
		r = run(len, 0, &j);
		CHECK(r == FL_OK && flash_is(len) && sim_violations == v0,
		      "no fast paths (%d): %s, fast %d", k, flash_message(r), dbg_fast);
		CHECK(!(dbg_fast & DBG_FAST_WRITE) || k == 1, "fast writes dropped (%d)", k);
	}

	/* 5. refusals: nothing written, the chip runs on */
	chip_new(0, 0x44);
	keep();
	len = make_image(8000, "LS11A");
	CHECK(run(len, 0, &j) == FL_WRONG_MODULE && unchanged() && chip.running,
	      "LS11A image on a CH32V003");
	len = make_image(20000, "LS10A");
	CHECK(run(len, 0, &j) == FL_TOO_BIG && unchanged() && chip.running, "too big");
	len = make_image(8000, NULL);
	d0 = sim_dmi;
	CHECK(run(len, 0, &j) == FL_BAD_IMAGE && sim_dmi == d0,
	      "not BASIC: refused before touching the module");
	CHECK(run(len, 1, &j) == FL_OK && flash_is(len), "not BASIC, forced");
	chip_new(0, 0x55);
	chip.rdprt = 1;
	keep();
	len = make_image(8000, "LS10A");
	CHECK(run(len, 0, &j) == FL_PROTECTED && unchanged(), "read-protected");
	chip_new(0, 0x66);
	chip.id7f = 0x00600600;             /* a CH32V006 */
	keep();
	CHECK(run(len, 1, &j) == FL_UNSUPPORTED && unchanged(), "unsupported chip, forced");
	chip_new(0, 0x77);
	chip.absent = 1;
	CHECK(run(len, 0, &j) == FL_NO_CHIP, "no module");

	/* 6. the Wolfskralle's own gate, with the host's checks bypassed */
	chip_new(0, 0x88);
	chip.id7f = 0x00600600;
	keep();
	struct dbg_info d;
	dbg_connect(DBG_MODE_AUTO, &d);
	CHECK(dbg_flash_begin() == DBG_UNSUPPORTED, "gate: unlock on an unsupported chip");
	uint32_t w[64];
	memset(w, 0, sizeof(w));
	int wr, at;
	CHECK(dbg_page_write(0x08000000, w, 0, &wr, &at) != DBG_OK && unchanged(),
	      "gate: write on an unsupported chip");
	chip_new(0, 0x99);
	dbg_connect(DBG_MODE_AUTO, &d);
	CHECK(dbg_flash_begin() == DBG_OK, "gate: unlock");
	uint32_t refused = dbg_refused;
	CHECK(dbg_page_write(0x08004000, w, 0, &wr, &at) == DBG_FORBIDDEN, "gate: past the flash");
	CHECK(dbg_page_write(0x1FFFF800, w, 0, &wr, &at) == DBG_FORBIDDEN, "gate: option bytes");
	CHECK(dbg_page_write(0x08000020, w, 0, &wr, &at) == DBG_FORBIDDEN, "gate: not page-aligned");
	CHECK(dbg_refused == refused + 3, "gate: refusals counted");
	dbg_resume();
	CHECK(chip.locked, "gate: locked again by resume");

	/* 7. damaged SWIO packets: written and verified, or a clean failure */
	int ok = 0, failed = 0;
	for (int t = 0; t < 6; t++) {
		chip_new(t & 1, 0xA0 + (uint32_t)t);
		len = make_image(t & 1 ? 24000 : 12000, t & 1 ? "LS11A" : "LS10A");
		sim_incomplete_pct = 1;
		r = run(len, 0, &j);
		sim_incomplete_pct = 0;
		if (r == FL_OK) {
			ok++;
			CHECK(flash_is(len), "damaged packets: reported ok, but the flash differs");
		} else {
			failed++;
			printf("  (damaged packets: %s; last result %d: %s)\n", flash_message(r), j.dbg,
			       chip_result(j.dbg));
		}
	}
	CHECK(ok >= 4, "damaged packets: only %d of 6 succeeded (%d failed)", ok, failed);
	printf("  1%% damaged packets: %d of 6 flashed (the rest failed cleanly)\n", ok);

	/* 8. power cuts anywhere: the next flash succeeds */
	int cuts = 0;
	for (int t = 0; t < 300; t++) {
		int v005 = t % 5 == 4;
		chip_new(v005, 0xC0 + (uint32_t)t);
		len = make_image(v005 ? 24000 : 9000 + (uint32_t)t * 23, v005 ? "LS11A" : "LS10A");
		wolfskralle_restart();
		sim_cut_at = sim_dmi + 1 + rand() % (v005 ? 3 * normal_dmi : normal_dmi);
		if (setjmp(sim_cut) == 0) {
			run(len, 0, &j);            /* finished before the cut */
		} else {
			cuts++;
			chip.power = 0;             /* the cut: everything restarts */
			chip.power = 1;
			chip_boot();
		}
		sim_cut_at = -1;
		wolfskralle_restart();
		r = run(len, 0, &j);
		CHECK(r == FL_OK && flash_is(len), "after a cut at %d: %s", t, flash_message(r));
		if (failures > 5) break;
	}
	printf("  %d power cuts, each followed by a good flash\n", cuts);

	CHECK(sim_violations == 0, "%ld violations", sim_violations);
	CHECK(sim_read_past == 0, "%ld reads past the end of the flash (fast reads must "
	      "not read ahead beyond what was asked)", sim_read_past);
	CHECK(dbg_refused == refused + 3, "the gate refused %u writes outside the tests of it",
	      dbg_refused - refused - 3);
	if (failures)
		printf("flash_test: FAILED (%d)\n", failures);
	else
		printf("flash_test: programming, refusals, write gate, damaged packets, power cuts: "
		       "passed (no violations)\n");
	return failures != 0;
}
