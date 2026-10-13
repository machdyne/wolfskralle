/*
 * sechs: a Sechs master through the Wolfskralle, and the BASIC check.
 * See sechs.h and docs/sechs.md.
 */

#include <stdio.h>
#include <string.h>
#include "wkproto.h"
#include "sechs.h"

int sx_xfer(struct wk_client *cl, uint8_t addr, const uint8_t *w, int wlen,
            uint8_t *r, int rlen)
{
	uint8_t in[WK_PAYLOAD_MAX], out[WK_PAYLOAD_MAX];
	int n = 0;
	if (wlen < 0 || wlen > WK_PAYLOAD_MAX - 2 || rlen < 0 || rlen > WK_I2C_MAX) return SX_E_USB;
	in[0] = addr;
	in[1] = (uint8_t)rlen;
	if (wlen) memcpy(in + 2, w, wlen);
	int st = wk_cmd(cl, WK_CMD_I2C, in, 2 + wlen, out, sizeof(out), &n, 3000);
	if (st == WK_ST_NO_POWER) return SX_E_POWER;
	if (st || n < 1) return SX_E_USB;
	if (out[0] == WK_I2C_OK) {
		if (n < 1 + rlen) return SX_E_USB;
		if (rlen) memcpy(r, out + 1, rlen);
	}
	return out[0];
}

int sx_scan(struct wk_client *cl, uint8_t *found, int max)
{
	uint8_t out[32];
	int n = 0, k = 0;
	int st = wk_cmd(cl, WK_CMD_I2C_SCAN, NULL, 0, out, sizeof(out), &n, 5000);
	if (st == WK_ST_NO_POWER) return SX_E_POWER;
	if (st || n < 17) return SX_E_USB;
	if (out[0]) return -10 - out[0];        /* the bus fails */
	for (int a = 0x08; a <= 0x77 && k < max; a++)
		if (out[1 + a / 8] & (1u << (a % 8))) found[k++] = (uint8_t)a;
	return k;
}

static int reg_read(struct wk_client *cl, uint8_t addr, uint8_t reg, uint8_t *r, int n)
{
	return sx_xfer(cl, addr, &reg, 1, r, n);
}

int sx_is_module(struct wk_client *cl, uint8_t addr)
{
	uint8_t s[2];
	return !reg_read(cl, addr, SX_SIG0, s, 2) && s[0] == 'S' && s[1] == '6';
}

int sx_read_info(struct wk_client *cl, uint8_t addr, struct sx_info *i)
{
	uint8_t r[SX_INFO_MAX];
	memset(i, 0, sizeof(*i));
	i->addr = addr;
	int k = reg_read(cl, addr, SX_SIG0, r, 7);
	if (k) return k;
	if (r[0] != 'S' || r[1] != '6') return WK_I2C_NAK_DATA;     /* not Sechs */
	i->ver = r[2];
	i->caps = r[3];
	i->status = r[4];
	i->ok = r[5];
	i->fault = r[6];
	/* INFO is a port register: writing its number starts it over */
	k = reg_read(cl, addr, SX_INFO, r, SX_INFO_MAX);
	if (k) return k;
	int n = 0;
	while (n < SX_INFO_MAX && r[n]) {
		i->text[n] = (char)(r[n] >= 32 || r[n] == '\n' ? r[n] : '?');
		n++;
	}
	i->text[n] = 0;
	return 0;
}

int sx_info_get(const char *text, const char *key, char *v, int vmax)
{
	size_t kl = strlen(key);
	const char *p = text;
	v[0] = 0;
	while (*p) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			size_t m = len - kl - 1;
			if ((int)m > vmax - 1) m = (size_t)vmax - 1;
			memcpy(v, p + kl + 1, m);
			v[m] = 0;
			return 1;
		}
		if (!e) break;
		p = e + 1;
	}
	return 0;
}

const char *sx_i2c_result(int r)
{
	switch (r) {
	case WK_I2C_OK:       return "ok";
	case WK_I2C_NAK_ADDR: return "no answer at that address";
	case WK_I2C_NAK_DATA: return "a byte was not acknowledged";
	case WK_I2C_STUCK:    return "the bus is stuck (a line stays low)";
	case WK_I2C_STRETCH:  return "the module held the clock low for too long";
	case SX_E_POWER:      return "target power is off";
	case SX_E_USB:        return "the Wolfskralle did not answer";
	default:              return "?";
	}
}

const char *sx_status_text(uint8_t s, char *buf, int n)
{
	static const char *names[7] = { "boot window", "halted", "running a program",
	                                "console active", "networked", "FAULT", "DEGRADED" };
	buf[0] = 0;
	for (int i = 0; i < 7; i++)
		if (s & (1u << i))
			snprintf(buf + strlen(buf), n - strlen(buf), "%s%s", buf[0] ? ", " : "", names[i]);
	if (!buf[0]) snprintf(buf, n, "idle");
	return buf;
}

const char *sx_fault_text(uint8_t f)
{
	switch (f) {
	case 0: return "none";
	case 1: return "the program drives A/B on a bus";
	case 2: return "C/D contradict the program's pin declaration";
	case 3: return "program error";
	case 4: return "storage check failed";
	default: return "unknown fault";
	}
}

/* ---- the console ---- */

static uint64_t now_ms(struct wk_client *cl) { return cl->t.now_us(cl->t.ctx) / 1000; }
static void sleep_ms(struct wk_client *cl, unsigned ms) { cl->t.sleep_us(cl->t.ctx, ms * 1000); }

/* reads what the console has (COUT, then up to that many from CDATA) */
static int drain(struct wk_client *cl, uint8_t addr, char *out, int *len, int outmax)
{
	int got = 0;
	for (;;) {
		uint8_t n, d[WK_I2C_MAX];
		int k = reg_read(cl, addr, SX_COUT, &n, 1);
		if (k) return k < 0 ? k : -10 - k;
		if (!n) return got;
		if (n > WK_I2C_MAX) n = WK_I2C_MAX;
		k = reg_read(cl, addr, SX_CDATA, d, n);
		if (k) return k < 0 ? k : -10 - k;
		for (int i = 0; i < n; i++)
			if (*len < outmax - 1 && d[i] != '\r') out[(*len)++] = (char)d[i];
		out[*len] = 0;
		got += n;
	}
}

int sx_console(struct wk_client *cl, uint8_t addr, const char *line,
               char *out, int outmax, unsigned quiet_ms, unsigned max_ms)
{
	char buf[128];
	int len = 0, k;
	snprintf(buf, sizeof(buf), "%s\r", line);
	int left = (int)strlen(buf);
	const char *p = buf;
	out[0] = 0;
	uint64_t t0 = now_ms(cl);

	while (left > 0) {
		uint8_t room;
		k = reg_read(cl, addr, SX_CIN, &room, 1);
		if (k) return k < 0 ? k : -10 - k;
		int n = room < left ? room : left;
		if (n > 32) n = 32;
		if (n) {
			uint8_t w[33];
			w[0] = SX_CDATA;
			memcpy(w + 1, p, n);
			k = sx_xfer(cl, addr, w, 1 + n, NULL, 0);
			if (k) return k < 0 ? k : -10 - k;
			p += n;
			left -= n;
		}
		if ((k = drain(cl, addr, out, &len, outmax)) < 0) return k;
		if (now_ms(cl) - t0 > max_ms) return len;
		if (!n) sleep_ms(cl, 5);
	}
	uint64_t quiet = now_ms(cl);
	while (now_ms(cl) - quiet < quiet_ms && now_ms(cl) - t0 < max_ms) {
		if ((k = drain(cl, addr, out, &len, outmax)) < 0) return k;
		if (k > 0) quiet = now_ms(cl);
		else sleep_ms(cl, 20);
	}
	return len;
}

/* ---- the check ---- */

static void say(struct sx_check *c, const char *fmt, const char *a)
{
	char m[200];
	if (!c->say) return;
	snprintf(m, sizeof(m), fmt, a);
	c->say(c->ctx, m);
}

static int power(struct wk_client *cl, int on)
{
	uint8_t in[3] = { (uint8_t)(on ? WK_POWER_ON : WK_POWER_OFF), 0, 0 }, out[16];
	int n;
	return wk_cmd(cl, WK_CMD_POWER, in, 3, out, sizeof(out), &n, 2000) ? -1 : 0;
}

static int contains(const uint8_t *img, uint32_t len, const char *t)
{
	size_t n = strlen(t);
	if (!n) return 0;
	for (uint32_t i = 0; i + n <= len; i++)
		if (!memcmp(img + i, t, n)) return 1;
	return 0;
}

/* the module to talk to: the address asked for, or the default, or the
 * only Sechs module on the bus; 0 if none answers yet, <0 on error */
static int find(struct wk_client *cl, struct sx_check *c)
{
	uint8_t a = c->addr ? c->addr : SX_DEFAULT_ADDR;
	int k = reg_read(cl, a, SX_SIG0, (uint8_t[2]){0}, 2);
	if (k < 0) return k;
	if (k == WK_I2C_STUCK || k == WK_I2C_STRETCH) return -10 - k;
	if (!k && sx_is_module(cl, a)) return a;
	if (c->addr) return 0;
	uint8_t found[16];
	int n = sx_scan(cl, found, 16);
	if (n < 0) return n;
	for (int i = 0; i < n; i++)
		if (sx_is_module(cl, found[i])) return found[i];
	return 0;
}

int sx_check_run(struct wk_client *cl, struct sx_check *c)
{
	char v[64], m[96];
	int a = 0;
	c->found = c->console_ran = 0;
	c->console_out[0] = 0;
	memset(&c->info, 0, sizeof(c->info));

	if (c->restart) {
		say(c, "%s", "restarting the module (target power off and on)");
		if (power(cl, 0)) return SX_USB;
		sleep_ms(cl, 300);
		if (power(cl, 1)) return SX_USB;
	}
	uint64_t t0 = now_ms(cl);

	/* the module answers from its boot window on (500-1000 ms after
	 * power-on); an LS11 that starts in programming mode answers only
	 * after it (10 s) */
	say(c, "%s", "waiting for a Sechs module");
	for (;;) {
		a = find(cl, c);
		if (a == SX_E_POWER) return SX_NO_POWER;
		if (a == SX_E_USB) return SX_USB;
		if (a < 0) return SX_BUS;
		if (a > 0) break;
		if (now_ms(cl) - t0 > c->wait_ms) return SX_NONE;
		sleep_ms(cl, 100);
	}
	c->found = 1;
	c->answered_ms = (unsigned)(now_ms(cl) - t0);

	/* past its boot window, so that STATUS says what it settled on */
	int k;
	for (;;) {
		k = sx_read_info(cl, (uint8_t)a, &c->info);
		if (k == SX_E_USB) return SX_USB;
		if (k) return SX_BUS;
		if (!(c->info.status & SX_ST_BOOT) || now_ms(cl) - t0 > c->wait_ms + 1500) break;
		sleep_ms(cl, 100);
	}
	snprintf(m, sizeof(m), "a Sechs module at 0x%02X", a);
	say(c, "%s", m);

	/* is it BASIC, for the right module, from this image? */
	if (!sx_info_get(c->info.text, "fw", v, sizeof(v)) || strncmp(v, "Machdyne BASIC", 14))
		return SX_NOT_BASIC;
	if (c->module) {
		sx_info_get(c->info.text, "mod", v, sizeof(v));
		if (strcmp(v, c->module)) return SX_WRONG_MODULE;
	}
	if (c->img && !contains(c->img, c->img_len, c->info.text)) return SX_OTHER_IMAGE;

	/* is it well? */
	if ((c->info.status & (SX_ST_FAULT | SX_ST_DEGRADED)) || c->info.fault ||
	    (c->info.ok & 0x07) != 0x07)
		return SX_FAULTY;

	/* does the interpreter compute? (only if no program runs: typing at
	 * a running program would interrupt it) */
	if (c->console && (c->info.caps & SX_CAP_I2C_CON) && !(c->info.status & SX_ST_RUNNING)) {
		say(c, "%s", "asking the console: PRINT 1234+4321");
		k = sx_console(cl, (uint8_t)a, "PRINT 1234+4321", c->console_out,
		               sizeof(c->console_out), 300, 3000);
		if (k == SX_E_USB) return SX_USB;
		if (k < 0) return SX_BUS;
		c->console_ran = 1;
		if (!strstr(c->console_out, "5555")) return SX_CONSOLE_BAD;
	}
	return SX_GOOD;
}

const char *sx_check_message(int r)
{
	switch (r) {
	case SX_GOOD:         return "Machdyne BASIC is installed and runs";
	case SX_NONE:         return "no Sechs module answered (no BASIC, not started, or not seated)";
	case SX_NOT_BASIC:    return "a Sechs module answered, but not Machdyne BASIC";
	case SX_WRONG_MODULE: return "Machdyne BASIC for another module";
	case SX_OTHER_IMAGE:  return "its identity is not the one in the image";
	case SX_FAULTY:       return "BASIC runs, but reports a fault or is degraded";
	case SX_CONSOLE_BAD:  return "the console did not answer PRINT 1234+4321 with 5555";
	case SX_BUS:          return "the Sechs bus fails (a line stays low, or the clock is held)";
	case SX_USB:          return "the Wolfskralle stopped answering";
	case SX_NO_POWER:     return "target power is off (wolfctl power on)";
	default:              return "?";
	}
}
