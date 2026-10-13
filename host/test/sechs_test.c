/*
 * sechs_test: the Sechs path end to end: wolfctl's check (host/sechs.c)
 * and client, the Wolfskralle's protocol (wkdev.c), I2C commands
 * (i2ccmds.c) and bit-banged master (i2cm.c), and a simulated Sechs module
 * at the level of the two open-drain lines: start and stop conditions,
 * acknowledges, clock stretching, the core registers, INFO and an I2C
 * console that runs PRINT a+b. The module's behaviour follows machdyne/
 * basic's sechs.c and targets/ls1x/module.c (Sechs 0.5).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wkproto.h"
#include "wkdev.h"
#include "wkclient.h"
#include "i2cm.h"
#include "i2ccmds.h"
#include "sechs.h"

void wk_test_irq_off(void) { }
void wk_test_irq_on(void) { }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t now_us;

/* ---- the simulated module ---- */

static struct {
	/* how it is */
	int present, basic, fault, running, console_wrong, stretch_max_us;
	unsigned boot_ms;           /* answers this long after power-on */
	uint8_t addr;
	const char *mod;
	/* state */
	int powered;
	uint64_t on_at;
	uint8_t reg, info_i;
	char cin[64]; int cin_n;
	char cout[256]; int cout_n, cout_r;
} m;

static int power;               /* target power */

static void module_power(int on)
{
	if (on && !power) m.on_at = now_us;
	power = on;
	m.cin_n = m.cout_n = m.cout_r = 0;
	m.reg = 0;
}

static int alive(void)
{
	return m.present && power && now_us - m.on_at >= (uint64_t)m.boot_ms * 1000;
}

static char info_text[96];

static uint8_t status(void)
{
	uint8_t s = 0x10;                                   /* networked */
	if (now_us - m.on_at < (uint64_t)(m.boot_ms + 600) * 1000) s |= 0x01;  /* boot window */
	else if (m.running) s |= 0x04;
	if (m.fault) s |= 0x20;
	return s;
}

static void console_line(void)
{
	int a, b;
	char out[160];
	m.cin[m.cin_n] = 0;
	if (sscanf(m.cin, "PRINT %d+%d", &a, &b) == 2)
		snprintf(out, sizeof(out), "%s\r\n %d\r\nOK\r\n", m.cin, m.console_wrong ? a - b : a + b);
	else
		snprintf(out, sizeof(out), "%s\r\nSYNTAX ERROR\r\n", m.cin);
	for (char *p = out; *p && m.cout_n < (int)sizeof(m.cout); p++) m.cout[m.cout_n++] = *p;
	m.cin_n = 0;
}

static uint8_t reg_rd(void)
{
	uint8_t r = m.reg, v = 0;
	switch (r) {
	case 0x00: v = 'S'; break;
	case 0x01: v = '6'; break;
	case 0x02: v = 0x05; break;
	case 0x03: v = 0x87; break;
	case 0x04: v = status(); break;
	case 0x05: v = (uint8_t)(0x01 | (m.fault ? 0 : 0x02) | 0x04 | 0x08); break;
	case 0x06: v = (uint8_t)(m.fault ? 4 : 0); break;
	case 0x09: v = m.info_i < strlen(info_text) ? (uint8_t)info_text[m.info_i++] : 0; return v;
	case 0x18: v = (uint8_t)(sizeof(m.cin) - 1 - m.cin_n); break;
	case 0x19: v = (uint8_t)(m.cout_n - m.cout_r > 255 ? 255 : m.cout_n - m.cout_r); break;
	case 0x1A: v = m.cout_r < m.cout_n ? (uint8_t)m.cout[m.cout_r++] : 0; return v;
	default:
		if (r >= 0x80 && r < 0x90) v = (uint8_t)(r ^ 0x5A);   /* program registers */
	}
	m.reg++;
	return v;
}

static void reg_wr(uint8_t v)
{
	if (m.reg == 0x1A) {                        /* CDATA: console input */
		if (v == '\r') console_line();
		else if (m.cin_n < (int)sizeof(m.cin) - 1) m.cin[m.cin_n++] = (char)v;
		return;
	}
	m.reg++;
}

/* ---- the simulated lines (open drain: wired AND) ---- */

enum { S_IDLE, S_RX, S_TX, S_IGNORE };

static struct {
	int m_scl, m_sda, s_sda;        /* 1: released */
	uint64_t stretch_until;
	int scl, sda;                   /* the lines as they were */
	int state, bitn, skip_fall, is_addr, first_data, rd, acked;
	uint8_t byte;
	long starts, stops, bytes;
} b = { .m_scl = 1, .m_sda = 1, .s_sda = 1, .scl = 1, .sda = 1, .state = S_IDLE };

static int shorted;              /* SDA shorted to ground */
static int scl_line(void) { return b.m_scl && now_us >= b.stretch_until; }
static int sda_line(void) { return b.m_sda && b.s_sda && !shorted; }

static void stretch(void)
{
	if (m.stretch_max_us) b.stretch_until = now_us + (uint64_t)(rand() % m.stretch_max_us);
}

static void fall(void)
{
	if (b.skip_fall) { b.skip_fall = 0; return; }
	if (b.state == S_RX) {
		b.bitn++;
		if (b.bitn == 8) {
			if (b.is_addr) {
				if ((b.byte >> 1) == m.addr && alive()) {
					b.rd = b.byte & 1;
					b.s_sda = 0;                /* acknowledge */
					stretch();
				} else {
					b.state = S_IGNORE;
				}
			} else {
				if (b.first_data) {
					m.reg = b.byte;
					if (m.reg == 0x09) m.info_i = 0;
					b.first_data = 0;
				} else {
					reg_wr(b.byte);
				}
				b.bytes++;
				b.s_sda = 0;
				stretch();
			}
		} else if (b.bitn == 9) {
			b.s_sda = 1;
			b.bitn = 0;
			int was_addr = b.is_addr;
			b.is_addr = 0;
			b.byte = 0;
			if (was_addr && b.rd) {
				b.state = S_TX;
				b.byte = reg_rd();
				b.s_sda = (b.byte >> 7) & 1;
			}
		}
	} else if (b.state == S_TX) {
		b.bitn++;
		if (b.bitn < 8) b.s_sda = (b.byte >> (7 - b.bitn)) & 1;
		else if (b.bitn == 8) b.s_sda = 1;      /* the master acknowledges */
		else {
			b.bytes++;
			if (b.acked) {
				b.byte = reg_rd();
				b.bitn = 0;
				b.s_sda = (b.byte >> 7) & 1;
				stretch();
			} else {
				b.state = S_IGNORE;
			}
		}
	}
}

static void rise(void)
{
	if (b.state == S_RX && b.bitn < 8) b.byte = (uint8_t)(b.byte << 1 | sda_line());
	if (b.state == S_TX && b.bitn == 8) b.acked = !sda_line();
}

static void update(void)
{
	int scl = scl_line(), sda = sda_line();
	if (scl && b.scl && sda != b.sda) {
		if (!sda) {                             /* start (or repeated start) */
			b.starts++;
			b.state = S_RX;
			b.bitn = 0;
			b.byte = 0;
			b.is_addr = 1;
			b.first_data = 1;
			b.skip_fall = 1;
			b.s_sda = 1;
		} else {                                /* stop */
			b.stops++;
			b.state = S_IDLE;
			b.s_sda = 1;
		}
	}
	if (!b.scl && scl) rise();
	if (b.scl && !scl) fall();
	b.scl = scl;
	b.sda = sda_line();
}

void i2cm_scl(int release) { b.m_scl = release; update(); }
void i2cm_sda(int release) { b.m_sda = release; update(); }
int i2cm_scl_in(void) { update(); return scl_line(); }
int i2cm_sda_in(void) { return sda_line(); }
void i2cm_delay(void) { now_us += 6; update(); }
void i2cm_delay_us(uint32_t us) { now_us += us; update(); }

/* ---- the simulated Wolfskralle ---- */

int i2ccmd_ready(void) { return power; }

static uint8_t handler(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                       uint8_t *out, uint8_t *outlen)
{
	uint8_t st;
	if (i2ccmd_handle(cmd, in, inlen, out, outlen, &st)) return st;
	if (cmd == WK_CMD_POWER && inlen >= 1) {
		if (in[0] != WK_POWER_QUERY) module_power(in[0] == WK_POWER_ON);
		memset(out, 0, WK_PWR_LEN);
		out[0] = (uint8_t)power;
		*outlen = WK_PWR_LEN;
		return WK_ST_OK;
	}
	return WK_ST_BAD_CMD;
}

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
	now_us += 1000;
	return n;
}

static void t_sleep(void *ctx, unsigned us) { (void)ctx; now_us += us; update(); }
static uint64_t t_now(void *ctx) { (void)ctx; return now_us; }

static struct wk_client cl;

/* ---- the tests ---- */

static void module_new(void)
{
	memset(&m, 0, sizeof(m));
	m.present = 1;
	m.basic = 1;
	m.addr = 0x0C;
	m.mod = "LS10A";
	m.boot_ms = 30;
	m.stretch_max_us = 40;
	power = 1;
	m.on_at = now_us;
}

static void set_info(void)
{
	if (m.basic)
		snprintf(info_text, sizeof(info_text), "fw=Machdyne BASIC\nmod=%s\nlang=basic\n", m.mod);
	else
		snprintf(info_text, sizeof(info_text), "fw=Tidegrow Timer 2\nmod=%s\n", m.mod);
}

static int check(int restart, const char *mod, const uint8_t *img, uint32_t len, struct sx_check *c)
{
	set_info();
	memset(c, 0, sizeof(*c));
	c->restart = restart;
	c->console = 1;
	c->module = mod;
	c->img = img;
	c->img_len = len;
	c->wait_ms = 12000;
	return sx_check_run(&cl, c);
}

int main(void)
{
	struct sx_check c;
	int r;
	srand(7);
	struct wk_transport t = { t_set, t_get, t_sleep, t_now, NULL };
	wk_client_init(&cl, &t);

	/* 1. the bit level: register reads of every length, with stretching */
	module_new();
	set_info();
	m.boot_ms = 0;
	m.stretch_max_us = 200;
	for (int i = 0; i < 2000; i++) {
		uint8_t reg = (uint8_t)(0x80 + rand() % 16), d[WK_I2C_MAX];
		int n = 1 + rand() % (0x90 - reg);
		r = i2cm_xfer(0x0C, &reg, 1, d, n);
		int ok = r == I2CM_OK;
		for (int k = 0; ok && k < n; k++) ok = d[k] == (uint8_t)((reg + k) ^ 0x5A);
		CHECK(ok, "register read %02x+%d: result %d", reg, n, r);
		if (failures > 5) break;
	}
	uint8_t reg = 0x09, d[WK_I2C_MAX];
	r = i2cm_xfer(0x0C, &reg, 1, d, 40);
	CHECK(!r && !memcmp(d, info_text, strlen(info_text)), "INFO read");
	CHECK(i2cm_xfer(0x33, NULL, 0, NULL, 0) == I2CM_NAK_ADDR, "nobody at 0x33");
	CHECK(i2cm_xfer(0x0C, NULL, 0, NULL, 0) == I2CM_OK, "address-only probe");

	/* 2. a slave left in the middle of a read holds SDA: recovered */
	i2cm_sda(0); i2cm_scl(0);                       /* a start */
	for (int i = 7; i >= 0; i--) {                  /* 0x0C read */
		i2cm_sda(((0x0C << 1 | 1) >> i) & 1); i2cm_scl(1); i2cm_scl(0);
	}
	i2cm_sda(1); i2cm_scl(1); i2cm_scl(0);          /* its acknowledge */
	for (int i = 0; i < 3; i++) { i2cm_scl(1); i2cm_scl(0); }
	/* (the master went away; the module's 'S' has a 0 in bit 7) */
	CHECK(!i2cm_sda_in(), "the module holds SDA low (test setup)");
	reg = 0x00;
	r = i2cm_xfer(0x0C, &reg, 1, d, 2);
	CHECK(!r && d[0] == 'S' && d[1] == '6', "recovered from a held SDA: %d", r);

	/* 3. a module that holds the clock for too long */
	m.stretch_max_us = 0;
	b.stretch_until = now_us + 200000;
	CHECK(i2cm_xfer(0x0C, &reg, 1, d, 2) == I2CM_STRETCH || i2cm_xfer(0x0C, &reg, 1, d, 2) ==
	      I2CM_STUCK, "too long a stretch reported");
	b.stretch_until = 0;

	/* 4. the check: BASIC, cold start, console */
	module_new();
	m.boot_ms = 40;
	r = check(1, NULL, NULL, 0, &c);
	CHECK(r == SX_GOOD && c.found && c.info.addr == 0x0C && c.console_ran,
	      "good module: %s", sx_check_message(r));
	CHECK(strstr(c.console_out, "5555") != NULL, "console output: %s", c.console_out);
	CHECK(!(c.info.status & 0x01), "read after the boot window");

	/* 5. with the module and the image it should run */
	static uint8_t img[16384];
	for (unsigned i = 0; i < sizeof(img); i++) img[i] = (uint8_t)rand();
	memcpy(img + 9000, "fw=Machdyne BASIC\nmod=LS10A\nlang=basic\n", 39);
	r = check(1, "LS10A", img, sizeof(img), &c);
	CHECK(r == SX_GOOD, "the image's: %s", sx_check_message(r));
	r = check(1, "LS11A", NULL, 0, &c);
	CHECK(r == SX_WRONG_MODULE, "LS11A expected: %s", sx_check_message(r));
	memcpy(img + 9000, "fw=Machdyne BASIC\nmod=LS10A\nlang=xyz!\n", 39);
	r = check(1, "LS10A", img, sizeof(img), &c);
	CHECK(r == SX_OTHER_IMAGE, "another image: %s", sx_check_message(r));

	/* 6. not BASIC; faulty; a program running; a console that is wrong */
	module_new(); m.basic = 0;
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_NOT_BASIC, "not BASIC: %s", sx_check_message(r));
	module_new(); m.fault = 1;
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_FAULTY, "faulty: %s", sx_check_message(r));
	module_new(); m.running = 1;
	r = check(1, NULL, NULL, 0, &c);
	CHECK(r == SX_GOOD && !c.console_ran, "running: %s, console %d", sx_check_message(r),
	      c.console_ran);
	CHECK(m.cin_n == 0 && m.cout_n == 0, "nothing typed at a running program");
	module_new(); m.console_wrong = 1;
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_CONSOLE_BAD, "console: %s", sx_check_message(r));

	/* 7. elsewhere on the bus; absent; late (an LS11 in programming
	 * mode); no power */
	module_new(); m.addr = 0x23;
	r = check(1, NULL, NULL, 0, &c);
	CHECK(r == SX_GOOD && c.info.addr == 0x23, "at 0x23: %s", sx_check_message(r));
	module_new(); m.present = 0;
	uint64_t t0 = now_us;
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_NONE, "absent: %s", sx_check_message(r));
	CHECK(now_us - t0 >= 12000000ull && now_us - t0 < 20000000ull, "waited %.1f s",
	      (now_us - t0) / 1e6);
	module_new(); m.boot_ms = 10000;
	r = check(1, NULL, NULL, 0, &c);
	CHECK(r == SX_GOOD && c.answered_ms >= 10000, "late: %s after %u ms", sx_check_message(r),
	      c.answered_ms);
	module_new();
	module_power(0);
	CHECK((r = check(0, NULL, NULL, 0, &c)) == SX_NO_POWER, "no power: %s", sx_check_message(r));

	/* 8. a shorted SDA: reported as a stuck bus, by the master and the check */
	module_new();
	shorted = 1;
	CHECK(i2cm_xfer(0x0C, NULL, 0, NULL, 0) == I2CM_STUCK, "shorted SDA: stuck");
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_BUS, "shorted SDA: %s", sx_check_message(r));
	shorted = 0;
	module_new();
	CHECK((r = check(1, NULL, NULL, 0, &c)) == SX_GOOD, "after the short: %s",
	      sx_check_message(r));

	if (failures)
		printf("sechs_test: FAILED (%d)\n", failures);
	else
		printf("sechs_test: bit level (%ld bytes, stretching, recovery), BASIC check: passed\n",
		       b.bytes);
	return failures != 0;
}
