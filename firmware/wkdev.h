/*
 * wkdev: the device side of the Wolfskralle protocol (docs/protocol.md).
 *
 * Hardware-independent: the USB interrupt calls wkdev_set_start,
 * wkdev_set_data and wkdev_get_start; the main loop calls wkdev_poll,
 * which runs one command at a time. The host tests build this file with
 * a simulated USB layer (host/test/proto_test.c).
 *
 * The rules that make the link reliable:
 * - the interrupt never waits: it only copies bytes and picks a buffer;
 * - while a command runs, a GET returns a fixed BUSY frame and a SET is
 *   dropped (counted), so the request and the reply are never torn;
 * - the reply stays until the next request is executed, so a reply that
 *   was lost can be fetched again;
 * - a request identical to the last one executed (seq, cmd and CRC) is
 *   answered from the kept reply, not executed again;
 * - a request that fails its CRC is dropped (counted) and the kept reply
 *   is left alone; the host notices that its request was not executed
 *   and sends it again.
 */

#ifndef WKDEV_H
#define WKDEV_H

#include <stdint.h>

/* Executes cmd. in/inlen: the request payload. Writes the reply payload
 * to out (at most WK_PAYLOAD_MAX bytes) and its length to *outlen.
 * Returns a WK_ST_* status. Runs in the main loop; may take long. */
typedef uint8_t (*wkdev_handler)(uint8_t cmd, const uint8_t *in, uint8_t inlen,
                                 uint8_t *out, uint8_t *outlen);

struct wkdev_counters {
	uint32_t cmds;      /* commands executed */
	uint32_t dups;      /* repeated requests answered from the kept reply */
	uint32_t bad;       /* requests that failed their CRC or length */
	uint32_t rejected;  /* SETs that arrived while a command was running */
	uint32_t gets;      /* GETs answered */
	uint32_t busy_gets; /* ... of them BUSY */
};

extern volatile struct wkdev_counters wkdev_count;

/* interrupt side: wvi is rv003usb's lValueLSBIndexMSB (wValue, wIndex) */
int wkdev_set_start(uint32_t wvi, int len);         /* returns bytes to accept */
void wkdev_set_data(int offset, const uint8_t *d, int n);
const uint8_t *wkdev_get_start(uint32_t wvi, int len, int *sendlen);

/* main loop side: runs a pending request; 1 if it ran one */
int wkdev_poll(wkdev_handler h);

/* 1 while a request is pending or running */
int wkdev_busy(void);

/* back to the start-up state (the tests: a simulated restart) */
void wkdev_reset(void);

#endif
