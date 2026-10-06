/*
 * Wolfskralle firmware (CH32V003). See docs/plan.md and docs/protocol.md.
 *
 * Phase 1: USB (rv003usb, HID feature reports), the protocol (wkdev.c),
 * target power, the LED and the supply measurement.
 * Phase 2: SWIO (swio.c): timing, debug register access, the link test.
 * Phase 3: the debug module (ch32dbg.c): connect, stop, identify, read.
 * Phase 4: programming (ch32dbg.c, dbgcmds.c), behind the write gate.
 */

#include "ch32fun.h"
#include <string.h>
#include "rv003usb.h"
#include "usb_config.h"
#include "wkproto.h"
#include "wkdev.h"
#include "board.h"
#include "swio.h"
#include "ch32dbg.h"
#include "dbgcmds.h"

/* ---- time ---- */

static uint32_t ms_now;              /* ms since start */
static uint32_t tick_last, tick_acc;

static void tick(void)
{
	uint32_t now = SysTick->CNT;
	tick_acc += now - tick_last;
	tick_last = now;
	if (tick_acc >= DELAY_MS_TIME) {
		ms_now += tick_acc / DELAY_MS_TIME;
		tick_acc %= DELAY_MS_TIME;
	}
}

/* ---- supply voltage: the 1.2 V internal reference on ADC channel 8 ---- */

#define VREFINT_MV 1200

static void vdd_init(void)
{
	funAnalogInit();
	ADC1->SAMPTR2 = (ADC1->SAMPTR2 & ~(7u << (3 * 8))) | (6u << (3 * 8)); /* 73 cycles */
	ADC1->CTLR2 |= ADC_TSVREFE;
}

static uint16_t vdd_mv(void)
{
	int raw = funAnalogRead(8);      /* 10 bits */
	if (raw <= 0) return 0;
	return (uint16_t)((uint32_t)VREFINT_MV * 1023 / (uint32_t)raw);
}

/* ---- target power ---- */

static uint8_t tpower;

static void tpower_set(int on)
{
	if (!on) {
		swio_off();             /* never drive the pins of an unpowered target */
		dbg_halted = 0;
	}
	tpower = on ? 1 : 0;
	funDigitalWrite(PIN_TPWRON, on ? FUN_HIGH : FUN_LOW);
}

/* ---- LED ---- */

static uint8_t led_mode = WK_LED_AUTO;
static uint32_t led_dark_until;      /* AUTO: dark briefly after each command */

static void led_update(void)
{
	int on;
	if (led_mode == WK_LED_ON) on = 1;
	else if (led_mode == WK_LED_OFF) on = 0;
	else if (!rv003usb_internal_data.my_address)
		on = (ms_now / 500) & 1;     /* not enumerated (yet): blink */
	else
		on = (int32_t)(ms_now - led_dark_until) >= 0;
	funDigitalWrite(PIN_LED, on ? FUN_HIGH : FUN_LOW);
}

/* ---- watchdog and reset cause ---- */

static uint8_t reset_cause;

/* 4 s: everything long feeds it (SWIO packets, the main loop, waits), so
 * only a hang lets it run out; it then restarts the Wolfskralle, which
 * reports why in INFO. Stopped while a debugger halts the CPU. */
static void watchdog_init(void)
{
	uint32_t f = RCC->RSTSCKR;
	reset_cause = (f & RCC_IWDGRSTF) ? WK_RESET_WATCHDOG :
	              (f & RCC_PORRSTF) ? WK_RESET_POWER :
	              (f & RCC_SFTRSTF) ? WK_RESET_SOFTWARE :
	              (f & RCC_PINRSTF) ? WK_RESET_PIN : WK_RESET_OTHER;
	RCC->RSTSCKR |= RCC_RMVF;

	__set_DEBUG_CR(__get_DEBUG_CR() | DBGMCU_IWDG_STOP);
	RCC->RSTSCKR |= RCC_LSION;
	while (!(RCC->RSTSCKR & RCC_LSIRDY)) ;
	IWDG->CTLR = 0x5555;                 /* unlock */
	IWDG->PSCR = IWDG_Prescaler_128;     /* 128 kHz / 128: 1 ms */
	IWDG->RLDR = 4000;
	IWDG->CTLR = 0xAAAA;
	IWDG->CTLR = 0xCCCC;                 /* start */
}

/* ---- serial number from the chip's unique ID ---- */

uint16_t wk_serial_desc[1 + WK_SERIAL_CHARS] __attribute__((aligned(4)));

static void serial_init(void)
{
	const uint32_t *uid = (const uint32_t *)0x1FFFF7E8;   /* ESIG_UNIID1 */
	uint32_t w[2] = { uid[0], uid[1] ^ uid[2] };
	wk_serial_desc[0] = (3 << 8) | (2 + 2 * WK_SERIAL_CHARS);
	for (int i = 0; i < WK_SERIAL_CHARS; i++) {
		uint32_t nib = (w[i / 8] >> (28 - 4 * (i % 8))) & 0xf;
		wk_serial_desc[1 + i] = nib < 10 ? '0' + nib : 'A' + nib - 10;
	}
}

/* ---- commands ---- */

static uint8_t cmd_info(uint8_t *out, uint8_t *outlen)
{
	memset(out, 0, WK_INFO_LEN);
	out[WK_INFO_PROTO] = WK_PROTO_VERSION;
	out[WK_INFO_BOARD] = BOARD_ID;
	wk_put16(out + WK_INFO_FW, WK_FW_VERSION);
	wk_put32(out + WK_INFO_UPTIME, ms_now);
	wk_put16(out + WK_INFO_VDD, vdd_mv());
	out[WK_INFO_TPOWER] = tpower;
	out[WK_INFO_LED] = led_mode;
	wk_put32(out + WK_INFO_CMDS, wkdev_count.cmds);
	wk_put32(out + WK_INFO_DUPS, wkdev_count.dups);
	wk_put32(out + WK_INFO_BAD, wkdev_count.bad);
	wk_put32(out + WK_INFO_REJECTED, wkdev_count.rejected);
	memcpy(out + WK_INFO_NAME, "Wolfskralle", 11);
	out[WK_INFO_SWIO] = (uint8_t)swio_is_on();
	out[WK_INFO_RESET] = reset_cause;
	*outlen = WK_INFO_LEN;
	return WK_ST_OK;
}

static uint8_t cmd_ping(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 2) return WK_ST_BAD_ARG;
	uint16_t delay = wk_get16(in);
	if (delay > WK_PING_DELAY_MAX) return WK_ST_BAD_ARG;
	while (delay) {                  /* in steps, feeding the watchdog */
		uint16_t d = delay > 100 ? 100 : delay;
		Delay_Ms(d);
		WDT_FEED();
		delay -= d;
	}
	memcpy(out, in + 2, inlen - 2);
	*outlen = inlen - 2;
	return WK_ST_OK;
}

/* Switches target power and watches the supply for monitor_ms: the
 * target's capacitors charge from the same 3.3V rail as this MCU. */
static uint8_t cmd_power(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 1 || in[0] > WK_POWER_QUERY) return WK_ST_BAD_ARG;
	uint16_t mon = inlen >= 3 ? wk_get16(in + 1) : 0;
	if (mon > WK_POWER_MON_MAX_MS) return WK_ST_BAD_ARG;

	uint16_t before = vdd_mv(), vmin = 0xffff;
	uint32_t sum = 0, n = 0;

	if (in[0] != WK_POWER_QUERY) tpower_set(in[0] == WK_POWER_ON);

	uint32_t start = SysTick->CNT;
	while (mon && SysTick->CNT - start < (uint32_t)mon * DELAY_MS_TIME) {
		uint16_t v = vdd_mv();
		if (v < vmin) vmin = v;
		sum += v;
		n++;
	}

	out[WK_PWR_STATE] = tpower;
	wk_put16(out + WK_PWR_VDD_BEFORE, before);
	wk_put16(out + WK_PWR_VDD_MIN, n ? vmin : before);
	wk_put16(out + WK_PWR_VDD_AVG, n ? (uint16_t)(sum / n) : before);
	wk_put16(out + WK_PWR_SAMPLES, n > 0xffff ? 0xffff : (uint16_t)n);
	*outlen = WK_PWR_LEN;
	return WK_ST_OK;
}

static uint8_t cmd_led(const uint8_t *in, uint8_t inlen)
{
	if (inlen < 1 || in[0] > WK_LED_AUTO) return WK_ST_BAD_ARG;
	led_mode = in[0];
	return WK_ST_OK;
}

/* SWIO needs target power; switched on on first use */
static int swio_ready(void)
{
	if (!tpower) return 0;
	swio_on();
	return 1;
}

static uint8_t cmd_swio(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 1 || in[0] > 1) return WK_ST_BAD_ARG;
	if (in[0]) {
		if (!swio_ready()) return WK_ST_NO_POWER;
	} else {
		swio_off();
	}
	out[0] = (uint8_t)swio_is_on();
	*outlen = 1;
	return WK_ST_OK;
}

static uint8_t cmd_swio_timing(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen) {
		if (inlen != WK_TM_LEN || swio_set_timing(in)) return WK_ST_BAD_ARG;
	}
	memcpy(out, swio_tm, WK_TM_LEN);
	*outlen = WK_TM_LEN;
	return WK_ST_OK;
}

static uint8_t cmd_dmi(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	int n = inlen / 6;
	if (!n || inlen % 6 || n > WK_DMI_MAX) return WK_ST_BAD_ARG;
	for (int i = 0; i < n; i++)
		if (in[6 * i] > WK_DMI_READ) return WK_ST_BAD_ARG;
	if (!swio_ready()) return WK_ST_NO_POWER;

	uint8_t done = 0, failed = 0, *p = out + 2;
	for (int i = 0; i < n && !failed; i++) {
		const uint8_t *op = in + 6 * i;
		if (op[0] == WK_DMI_WRITE) {
			failed = swio_write(op[1], wk_get32(op + 2)) ? 1 : 0;
		} else {
			uint32_t v = 0;
			failed = swio_read(op[1], &v) ? 1 : 0;
			if (!failed) { wk_put32(p, v); p += 4; }
		}
		if (!failed) done++;
	}
	out[0] = done;
	out[1] = failed;
	*outlen = (uint8_t)(p - out);
	return WK_ST_OK;
}

static uint8_t cmd_swio_test(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 4) return WK_ST_BAD_ARG;
	uint32_t n = wk_get32(in);
	if (n > WK_SWIO_TEST_MAX) return WK_ST_BAD_ARG;
	if (!swio_ready()) return WK_ST_NO_POWER;
	swio_link_test(n, out);
	*outlen = WK_LT_LEN;
	return WK_ST_OK;
}

static uint8_t cmd_swio_raw(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	if (inlen < 6 || in[0] > WK_DMI_READ) return WK_ST_BAD_ARG;
	if (!swio_ready()) return WK_ST_NO_POWER;
	uint32_t v = wk_get32(in + 2);
	int r = in[0] == WK_DMI_WRITE ? swio_write(in[1], v) : swio_read(in[1], &v);
	out[0] = r ? 0 : 1;
	wk_put32(out + 1, v);
	out[5] = swio_ncap;
	memcpy(out + 6, swio_cap, swio_ncap);
	*outlen = 6 + swio_ncap;
	return WK_ST_OK;
}

/* ---- hooks for ch32dbg.c ---- */

int dbg_dmi_write(uint8_t reg, uint32_t val) { return swio_write(reg, val); }
int dbg_dmi_read(uint8_t reg, uint32_t *val) { return swio_read(reg, val); }
void dbg_delay_us(uint32_t us)
{
	Delay_Us(us);
	WDT_FEED();
}

void dbg_target_power(int on)
{
	tpower_set(on);
	if (on) swio_on();
}

int dbg_have_reset(void) { return HAVE_TRESET; }

void dbg_reset_line(int hold)
{
#if HAVE_TRESET
	/* open drain: low, or released (the module pulls RESETN up) */
	funDigitalWrite(PIN_TRESET, FUN_LOW);
	funPinMode(PIN_TRESET, hold ? GPIO_CFGLR_OUT_10Mhz_OD : GPIO_CFGLR_IN_FLOAT);
#else
	(void)hold;
#endif
}

/* ---- debug and flash commands: dbgcmds.c ---- */

int dbgcmd_swio_ready(void) { return swio_ready(); }

/* ---- PROFILE: where the time went ---- */

static uint32_t handler_us, handler_cmds;

/* a microsecond clock that keeps counting across SysTick's wrap (89 s) */
uint32_t dbg_us(void)
{
	static uint32_t last, us, rem;
	uint32_t now = SysTick->CNT, d = now - last + rem;
	last = now;
	us += d / (FUNCONF_SYSTEM_CORE_CLOCK / 1000000);
	rem = d % (FUNCONF_SYSTEM_CORE_CLOCK / 1000000);
	return us;
}

static uint8_t cmd_profile(const uint8_t *in, uint8_t inlen, uint8_t *out, uint8_t *outlen)
{
	wk_put32(out + WK_PF_HANDLER_US, handler_us);
	wk_put32(out + WK_PF_COMMANDS, handler_cmds);
	wk_put32(out + WK_PF_GETS, wkdev_count.gets);
	wk_put32(out + WK_PF_BUSY_GETS, wkdev_count.busy_gets);
	wk_put32(out + WK_PF_SWIO_PACKETS, swio_packets);
	wk_put32(out + WK_PF_SWIO_BAD, swio_bad);
	wk_put32(out + WK_PF_PRECHECK_US, dbg_prof.precheck_us);
	wk_put32(out + WK_PF_ERASE_US, dbg_prof.erase_us);
	wk_put32(out + WK_PF_PROGRAM_US, dbg_prof.program_us);
	wk_put32(out + WK_PF_VERIFY_US, dbg_prof.verify_us);
	wk_put32(out + WK_PF_READ_US, dbg_prof.read_us);
	wk_put32(out + WK_PF_WAITS, dbg_prof.waits);
	wk_put32(out + WK_PF_PAGES, dbg_prof.pages);
	wk_put32(out + WK_PF_FALLBACKS, dbg_prof.fallbacks);
	*outlen = WK_PF_LEN;
	if (inlen >= 1 && in[0]) {
		handler_us = handler_cmds = 0;
		__disable_irq();
		wkdev_count.gets = wkdev_count.busy_gets = 0;
		__enable_irq();
		swio_packets = swio_bad = 0;
		memset(&dbg_prof, 0, sizeof(dbg_prof));
	}
	return WK_ST_OK;
}

static uint8_t handler(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                       uint8_t *out, uint8_t *outlen)
{
	uint8_t st;
	led_dark_until = ms_now + 40;
	if (led_mode == WK_LED_AUTO) funDigitalWrite(PIN_LED, FUN_LOW);

	uint32_t t0 = dbg_us();
	handler_cmds++;
	if (dbgcmd_handle(cmd, in, inlen, out, outlen, &st)) {
		handler_us += dbg_us() - t0;
		return st;
	}
	if (cmd == WK_CMD_PROFILE) return cmd_profile(in, inlen, out, outlen);

	switch (cmd) {
	case WK_CMD_INFO:  return cmd_info(out, outlen);
	case WK_CMD_PING:  return cmd_ping(in, inlen, out, outlen);
	case WK_CMD_POWER: return cmd_power(in, inlen, out, outlen);
	case WK_CMD_LED:   return cmd_led(in, inlen);
	case WK_CMD_SWIO:        return cmd_swio(in, inlen, out, outlen);
	case WK_CMD_SWIO_TIMING: return cmd_swio_timing(in, inlen, out, outlen);
	case WK_CMD_DMI:         return cmd_dmi(in, inlen, out, outlen);
	case WK_CMD_SWIO_TEST:   return cmd_swio_test(in, inlen, out, outlen);
	case WK_CMD_SWIO_RAW:    return cmd_swio_raw(in, inlen, out, outlen);
	default:           return WK_ST_BAD_CMD;
	}
}

/* ---- rv003usb callbacks (interrupt context: short, never waiting) ---- */

void usb_handle_user_in_request(struct usb_endpoint *e, uint8_t *scratchpad,
                                int endp, uint32_t sendtok, struct rv003usb_internal *ist)
{
	/* only the interrupt endpoint gets here: it never has data */
	(void)e; (void)scratchpad; (void)endp; (void)sendtok; (void)ist;
	usb_send_data(0, 0, 2, 0x5A);    /* NAK */
}

void usb_handle_hid_set_report_start(struct usb_endpoint *e, int reqLen, uint32_t wvi)
{
	e->opaque = 0;
	e->max_len = wkdev_set_start(wvi, reqLen);
}

void usb_handle_user_data(struct usb_endpoint *e, int current_endpoint,
                          uint8_t *data, int len, struct rv003usb_internal *ist)
{
	(void)ist;
	if (current_endpoint) return;
	int offset = e->count << 3;
	if (offset < (int)e->max_len)
		wkdev_set_data(offset, data, len);
	e->count++;
}

void usb_handle_hid_get_report_start(struct usb_endpoint *e, int reqLen, uint32_t wvi)
{
	int n;
	e->opaque = (uint8_t *)wkdev_get_start(wvi, reqLen, &n);
	e->max_len = n;
}

/* ---- main ---- */

int main(void)
{
	SystemInit();
	RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_AFIO;

	/* target power: on, as R9 already holds it while the MCU starts */
	funPinMode(PIN_TPWRON, GPIO_CFGLR_OUT_10Mhz_PP);
	tpower_set(1);

	funPinMode(PIN_LED, GPIO_CFGLR_OUT_10Mhz_PP);
	funDigitalWrite(PIN_LED, FUN_HIGH);

	/* PC1, PC2 (SWIO), PC5, PC6 (Sechs) stay floating inputs (the reset
	 * state) until used, and SWIO is switched off with target power, so
	 * nothing can feed a powered-down target */

	vdd_init();
	serial_init();
	watchdog_init();

	/* off the bus for a moment, so that the host notices a restart */
	Delay_Ms(50);
	usb_setup();

	tick_last = SysTick->CNT;
	while (1) {
		WDT_FEED();
		tick();
		wkdev_poll(handler);
		led_update();
	}
}
