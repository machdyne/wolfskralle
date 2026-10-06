/*
 * swio: SWIO to the target with TIM2 and two DMA channels, so the CPU
 * (and the USB interrupt) is free while a packet is on the wire.
 * See docs/swio.md.
 *
 * TIM2, partial remap 1, counting at 48 MHz (one tick = 20.8 ns):
 *   CH2 on PC2: PWM, active low, open drain (or push-pull for write
 *        packets in mode 1): the line is low from the start of each
 *        period until the compare value, then released;
 *   CH4 on PC1: input capture on the rising edge: when the line was high
 *        again, counted from the start of the period.
 * PC1 and PC2 meet at TSWIO through 100R each (5K1 pull-up to TPWR).
 *
 * DMA1 channel 2 (TIM2 update) loads the next compare value at the start
 * of each period (preloaded, so it takes effect one period later);
 * DMA1 channel 7 (TIM2 CH4) stores each capture. A compare value of 0
 * keeps the line high, so a packet is framed by idle periods.
 */

#include "ch32fun.h"
#include "board.h"
#include "swio.h"

#define DMA_OUT     DMA1_Channel2       /* TIM2_UP */
#define DMA_IN      DMA1_Channel7       /* TIM2_CH4 */

/* registers of the debug module */
#define DM_DATA0        0x04
#define DM_DMCONTROL    0x10
#define DM_CFGR         0x7D
#define DM_SHDWCFGR     0x7E
#define DM_CFG_KEY      0x5AA50400u     /* key, and "enable the chip's output" */

/* period one_od one_pp zero read thresh mode stop_us (see docs/swio.md;
 * from the first swio-scan on hardware, docs/hwtest.md) */
uint8_t swio_tm[WK_TM_LEN] = { 60, 10, 10, 40, 6, 28, 0, 4 };

uint8_t swio_cap[48];
uint8_t swio_ncap;
uint32_t swio_packets, swio_bad;

static uint8_t ob[1 + SWIO_BITS + 2];   /* idle, the bits, idle, idle */
static uint8_t is_on;

int swio_set_timing(const uint8_t *t)
{
	uint8_t p = t[WK_TM_PERIOD], z = t[WK_TM_ZERO];
	if (p < 20 || p > 250) return -1;
	if (!t[WK_TM_ONE_OD] || !t[WK_TM_ONE_PP] || !t[WK_TM_READ]) return -1;
	if (t[WK_TM_ONE_OD] >= z || t[WK_TM_ONE_PP] >= z || t[WK_TM_READ] >= z) return -1;
	if (z > p - 2) return -1;
	if (t[WK_TM_THRESH] <= t[WK_TM_READ] || t[WK_TM_THRESH] >= p) return -1;
	if (t[WK_TM_MODE] > 1) return -1;
	if (!t[WK_TM_STOP_US] || t[WK_TM_STOP_US] > 200) return -1;
	for (int i = 0; i < WK_TM_LEN; i++) swio_tm[i] = t[i];
	if (is_on) TIM2->ATRLR = swio_tm[WK_TM_PERIOD] - 1;
	return 0;
}

int swio_is_on(void)
{
	return is_on;
}

void swio_on(void)
{
	if (is_on) return;
	RCC->APB1PCENR |= RCC_APB1Periph_TIM2;
	RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;
	RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC;

	AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_TIM2_REMAP) | AFIO_PCFR1_TIM2_REMAP_PARTIALREMAP1;

	TIM2->CTLR1 = 0;
	TIM2->PSC = 0;
	TIM2->ATRLR = swio_tm[WK_TM_PERIOD] - 1;
	TIM2->CH2CVR = 0;                                   /* line high */
	TIM2->CHCTLR1 = TIM_OC2M_2 | TIM_OC2M_1 | TIM_OC2PE; /* CH2: PWM 1, preloaded */
	TIM2->CHCTLR2 = TIM_CC4S_0;                         /* CH4: capture TI4, no filter */
	TIM2->CCER = TIM_CC2E | TIM_CC2P | TIM_CC4E;        /* CH2 active low; CH4 rising */
	TIM2->CTLR1 = TIM_ARPE | TIM_URS;                   /* DMA on overflow only */
	TIM2->SWEVGR = TIM_UG;
	TIM2->DMAINTENR = TIM_UDE | TIM_CC4DE;
	TIM2->CTLR1 |= TIM_CEN;

	DMA_OUT->CFGR = 0;
	DMA_OUT->PADDR = (uint32_t)&TIM2->CH2CVR;
	DMA_OUT->CFGR = DMA_CFGR1_DIR | DMA_CFGR1_MINC | DMA_CFGR1_PSIZE_0 | DMA_CFGR1_PL;
	DMA_IN->CFGR = 0;
	DMA_IN->PADDR = (uint32_t)&TIM2->CH4CVR;
	DMA_IN->CFGR = DMA_CFGR1_MINC | DMA_CFGR1_PSIZE_0 | DMA_CFGR1_PL;

	funPinMode(PIN_SWIO_IN, GPIO_CFGLR_IN_FLOAT);
	funPinMode(PIN_SWIO_OUT, GPIO_CFGLR_OUT_50Mhz_AF_OD);
	is_on = 1;
	Delay_Us(100);                                      /* the line high for a moment */
}

void swio_off(void)
{
	DMA_OUT->CFGR &= ~DMA_CFGR1_EN;
	DMA_IN->CFGR &= ~DMA_CFGR1_EN;
	funPinMode(PIN_SWIO_OUT, GPIO_CFGLR_IN_FLOAT);
	funPinMode(PIN_SWIO_IN, GPIO_CFGLR_IN_FLOAT);
	TIM2->DMAINTENR = 0;
	TIM2->CTLR1 = 0;
	is_on = 0;
}

/* sends one packet and collects its captures; 0, or -1 if incomplete */
static int packet(int write, uint8_t reg, uint32_t val)
{
	int pp = write && swio_tm[WK_TM_MODE] == 1;
	uint8_t one = pp ? swio_tm[WK_TM_ONE_PP] : swio_tm[WK_TM_ONE_OD];
	uint32_t period = swio_tm[WK_TM_PERIOD];

	WDT_FEED();
	ob[0] = 0;
	swio_encode(ob + 1, write, reg, val, one, swio_tm[WK_TM_ZERO], swio_tm[WK_TM_READ]);
	ob[1 + SWIO_BITS] = 0;
	ob[2 + SWIO_BITS] = 0;

	if (pp) funPinMode(PIN_SWIO_OUT, GPIO_CFGLR_OUT_50Mhz_AF_PP);

	DMA_IN->CFGR &= ~DMA_CFGR1_EN;
	TIM2->DMAINTENR &= ~TIM_CC4DE;          /* drop a stale capture request */
	(void)TIM2->CH4CVR;
	TIM2->INTFR = (uint16_t)~TIM_CC4IF;
	TIM2->DMAINTENR |= TIM_CC4DE;
	DMA_IN->MADDR = (uint32_t)swio_cap;
	DMA_IN->CNTR = sizeof(swio_cap);
	DMA_IN->CFGR |= DMA_CFGR1_EN;

	DMA_OUT->CFGR &= ~DMA_CFGR1_EN;
	DMA_OUT->MADDR = (uint32_t)ob;
	DMA_OUT->CNTR = sizeof(ob);
	DMA_OUT->CFGR |= DMA_CFGR1_EN;

	/* SysTick counts at 48 MHz like the timer. Generous: the USB
	 * interrupt may take this loop's time, never the packet's. */
	uint32_t start = SysTick->CNT;
	uint32_t limit = (sizeof(ob) + 2) * period + 2 * DELAY_MS_TIME;
	int ok = 1;
	while (DMA_OUT->CNTR)
		if (SysTick->CNT - start > limit) { ok = 0; break; }
	/* the last bit's capture: at most one more period */
	start = SysTick->CNT;
	while (SysTick->CNT - start < 2 * period) ;

	swio_ncap = (uint8_t)(sizeof(swio_cap) - DMA_IN->CNTR);
	DMA_IN->CFGR &= ~DMA_CFGR1_EN;
	DMA_OUT->CFGR &= ~DMA_CFGR1_EN;
	TIM2->CH2CVR = 0;                       /* the line stays high */

	if (pp) funPinMode(PIN_SWIO_OUT, GPIO_CFGLR_OUT_50Mhz_AF_OD);
	Delay_Us(swio_tm[WK_TM_STOP_US]);
	swio_packets++;
	if (!ok || swio_ncap != SWIO_BITS) {
		swio_bad++;
		return -1;
	}
	return 0;
}

int swio_write(uint8_t reg, uint32_t val)
{
	return packet(1, reg, val);
}

int swio_read(uint8_t reg, uint32_t *val)
{
	if (packet(0, reg, 0)) return -1;
	return swio_decode(swio_cap, swio_ncap, swio_tm[WK_TM_THRESH], val);
}

/* The link alone, as machdyne/basic's ch32_link_test: enable the chip's
 * debug output, then write and read back DATA0 n times. The chip keeps
 * running. Also measures every pulse. */
void swio_link_test(uint32_t n, uint8_t *out)
{
	struct swio_stats st;
	uint32_t cfgr = 0, errors = 0, incomplete = 0, want0 = 0, got0 = 0, i;

	swio_stats_init(&st);
	for (i = 0; i < 2; i++) {
		swio_write(DM_SHDWCFGR, DM_CFG_KEY);
		swio_write(DM_CFGR, DM_CFG_KEY);
	}
	swio_write(DM_DMCONTROL, 0x00000001u);      /* debug module on, no halt */
	swio_write(DM_DMCONTROL, 0x00000001u);

	int chip = !swio_read(DM_CFGR, &cfgr) && (cfgr & 0xFFFF0000u) == 0x5AA50000u;

	for (i = 0; chip && i < n; i++) {
		uint32_t want = 0xA5A50000u ^ (i * 0x9E3779B9u), got = 0;
		int bad = 0;
		if (swio_write(DM_DATA0, want)) {
			bad = 1;
			incomplete++;
		} else {
			swio_stats_add(&st, swio_cap, 1, DM_DATA0, want);
		}
		if (swio_read(DM_DATA0, &got)) {
			bad = 1;
			incomplete++;
		} else {
			swio_stats_add(&st, swio_cap, 0, DM_DATA0, want);
			if (got != want) bad = 1;
		}
		if (bad) {
			if (!errors) { want0 = want; got0 = got; }
			errors++;
		}
	}

	out[WK_LT_RESULT] = chip ? 0 : 1;
	wk_put32(out + WK_LT_CFGR, cfgr);
	wk_put32(out + WK_LT_DONE, i);
	wk_put32(out + WK_LT_ERRORS, errors);
	wk_put32(out + WK_LT_INCOMPLETE, incomplete);
	wk_put32(out + WK_LT_BAD_WANT, want0);
	wk_put32(out + WK_LT_BAD_GOT, got0);
	for (int k = 0; k < SWS_KINDS; k++) {
		out[WK_LT_W1 + 2 * k] = st.min[k];
		out[WK_LT_W1 + 2 * k + 1] = st.max[k];
	}
}
