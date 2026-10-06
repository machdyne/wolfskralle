/*
 * wkdev: the device side of the Wolfskralle protocol. See wkdev.h and
 * docs/protocol.md.
 */

#include <string.h>
#include "wkproto.h"
#include "wkdev.h"

#ifdef WK_HOST_TEST
/* the tests run the main loop in one thread and the "interrupt" in
 * another; these lock the mutex the interrupt functions run under */
void wk_test_irq_off(void);
void wk_test_irq_on(void);
#define IRQ_OFF()   wk_test_irq_off()
#define IRQ_ON()    wk_test_irq_on()
#else
#include "ch32fun.h"
#define IRQ_OFF()   __disable_irq()
#define IRQ_ON()    __enable_irq()
#endif

#define BARRIER()   __asm__ volatile ("" ::: "memory")

enum { ST_IDLE, ST_RX, ST_PENDING, ST_EXEC };

static volatile uint8_t state = ST_IDLE;
static volatile uint8_t rx_len;           /* bytes expected in this SET */
static uint8_t have_reply;                /* tx holds a reply */
static uint8_t last_seq, last_cmd;        /* the request tx answers */
static uint16_t last_crc;

static uint8_t rx[WK_REPORT_LEN] __attribute__((aligned(4)));
static uint8_t tx[WK_REPORT_LEN] __attribute__((aligned(4)));

volatile struct wkdev_counters wkdev_count;

/* fixed frames, so the interrupt never computes anything:
 * seq 0, cmd 0, the status, no payload, and the CRC of that header */
static const uint8_t busy_frame[WK_REPORT_LEN] __attribute__((aligned(4))) =
	{ WK_REPORT_ID, 0, 0, WK_ST_BUSY, 0, 0, 0x3c, 0x26 };
static const uint8_t noreply_frame[WK_REPORT_LEN] __attribute__((aligned(4))) =
	{ WK_REPORT_ID, 0, 0, WK_ST_NO_REPLY, 0, 0, 0x6c, 0x7f };

/* wValue's low byte is the report ID, its high byte the type (3: feature) */
static int our_report(uint32_t wvi)
{
	return (wvi & 0xffff) == (0x0300 | WK_REPORT_ID);
}

int wkdev_set_start(uint32_t wvi, int len)
{
	if (!our_report(wvi)) return 0;
	if (state == ST_PENDING || state == ST_EXEC) {
		/* one request at a time: this one is dropped; the host sees BUSY,
		 * then a reply with another seq, and sends it again */
		wkdev_count.rejected++;
		return 0;
	}
	if (len > WK_REPORT_LEN) len = WK_REPORT_LEN;
	rx_len = (uint8_t)len;
	state = ST_RX;
	return len;
}

void wkdev_set_data(int offset, const uint8_t *d, int n)
{
	if (state != ST_RX || offset >= rx_len) return;
	if (offset + n > rx_len) n = rx_len - offset;
	memcpy(rx + offset, d, n);
	if (offset + n >= rx_len)
		state = ST_PENDING;
}

const uint8_t *wkdev_get_start(uint32_t wvi, int len, int *sendlen)
{
	const uint8_t *f;
	if (!our_report(wvi)) { *sendlen = 0; return 0; }
	/* only the bytes in use: the header, and the payload. The USB is
	 * bit-banged here, so each 8 bytes cost the CPU a packet's time; a
	 * BUSY answer is one packet instead of sixteen. (A full 8-byte packet
	 * ends with an empty one: the host then knows the reply is short.) */
	int used = WK_HDR_LEN;
	wkdev_count.gets++;
	if (state == ST_PENDING || state == ST_EXEC) {
		f = busy_frame;
		wkdev_count.busy_gets++;
	} else if (have_reply) {
		f = tx;
		used += tx[4];
	} else {
		f = noreply_frame;
	}
	if (len > used) len = used;
	*sendlen = len;
	return f;
}

void wkdev_reset(void)
{
	state = ST_IDLE;
	have_reply = 0;
}

int wkdev_busy(void)
{
	return state == ST_PENDING || state == ST_EXEC;
}

int wkdev_poll(wkdev_handler h)
{
	int go;
	IRQ_OFF();
	go = (state == ST_PENDING);
	if (go) state = ST_EXEC;
	IRQ_ON();
	if (!go) return 0;

	/* from here until state returns to idle, the interrupt touches
	 * neither rx nor tx */
	/* a request may be shorter than a full report: header and payload */
	if (rx_len < WK_HDR_LEN || rx[4] > rx_len - WK_HDR_LEN ||
	    !wk_frame_ok(rx) || rx[3] != 0) {
		wkdev_count.bad++;
		goto done;
	}

	uint8_t seq = rx[1], cmd = rx[2];
	uint16_t crc = wk_get16(rx + 6);

	if (have_reply && seq == last_seq && cmd == last_cmd && crc == last_crc) {
		wkdev_count.dups++;       /* a repeat: the kept reply answers it */
		goto done;
	}

	uint8_t outlen = 0;
	uint8_t st = h(cmd, rx + WK_HDR_LEN, rx[4], tx + WK_HDR_LEN, &outlen);
	if (outlen > WK_PAYLOAD_MAX) outlen = WK_PAYLOAD_MAX;
	memset(tx + WK_HDR_LEN + outlen, 0, WK_PAYLOAD_MAX - outlen);
	tx[0] = WK_REPORT_ID;
	tx[1] = seq;
	tx[2] = cmd;
	tx[3] = st;
	tx[4] = outlen;
	tx[5] = (uint8_t)crc;           /* ties the reply to this exact request */
	wk_frame_seal(tx);
	last_seq = seq;
	last_cmd = cmd;
	last_crc = crc;
	have_reply = 1;
	wkdev_count.cmds++;

done:
	BARRIER();
	IRQ_OFF();
	state = ST_IDLE;
	IRQ_ON();
	return 1;
}
