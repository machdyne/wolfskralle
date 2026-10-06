/*
 * swio_bits: SWIO packets as pulse widths, and back. Hardware-independent
 * (the host tests use it too). See docs/swio.md.
 *
 * A packet is 41 bits, most significant first, each a low pulse whose
 * length says what it is (WCH QingKe V2 debug manual; the sequence proven
 * on an LS10A by machdyne/basic's Werkzeug programmer):
 *
 *   start (1), register address (7 bits), 1 = write / 0 = read,
 *   then 32 bits: the data (write), or 32 read slots (read).
 *
 * A read slot starts with a short low pulse from the host; the chip then
 * holds the line low for a 0 or lets it rise for a 1.
 *
 * What comes back is, for every bit, the time from its falling edge to
 * the line being high again (a capture, in timer ticks).
 */

#ifndef SWIO_BITS_H
#define SWIO_BITS_H

#include <stdint.h>

#define SWIO_BITS       41
#define SWIO_HDR_BITS   9

/* the 41 low times of a packet */
static inline void swio_encode(uint8_t *out, int write, uint8_t reg, uint32_t val,
                               uint8_t one, uint8_t zero, uint8_t rd)
{
	uint32_t hdr = (1u << 8) | ((uint32_t)(reg & 0x7f) << 1) | (write ? 1u : 0u);
	for (int i = 0; i < SWIO_HDR_BITS; i++)
		out[i] = (hdr >> (SWIO_HDR_BITS - 1 - i)) & 1 ? one : zero;
	for (int i = 0; i < 32; i++)
		out[SWIO_HDR_BITS + i] = !write ? rd : ((val >> (31 - i)) & 1) ? one : zero;
}

/* the value of a read packet from its captures; -1 if incomplete */
static inline int swio_decode(const uint8_t *cap, int ncap, uint8_t thresh, uint32_t *val)
{
	if (ncap != SWIO_BITS) return -1;
	uint32_t v = 0;
	for (int i = 0; i < 32; i++)
		v = (v << 1) | (cap[SWIO_HDR_BITS + i] < thresh ? 1u : 0u);
	*val = v;
	return 0;
}

/* shortest and longest capture of each kind of bit */
enum { SWS_W1, SWS_W0, SWS_H1, SWS_H0, SWS_R1, SWS_R0, SWS_KINDS };

struct swio_stats {
	uint8_t min[SWS_KINDS], max[SWS_KINDS];
};

static inline void swio_stats_init(struct swio_stats *s)
{
	for (int k = 0; k < SWS_KINDS; k++) { s->min[k] = 0xff; s->max[k] = 0; }
}

static inline void swio_stats_one(struct swio_stats *s, int kind, uint8_t c)
{
	if (c < s->min[kind]) s->min[kind] = c;
	if (c > s->max[kind]) s->max[kind] = c;
}

/* adds a complete packet's captures; for a read, expect is the value it
 * should have returned (to sort the read slots into 1s and 0s) */
static inline void swio_stats_add(struct swio_stats *s, const uint8_t *cap, int write,
                                  uint8_t reg, uint32_t val_or_expect)
{
	uint32_t hdr = (1u << 8) | ((uint32_t)(reg & 0x7f) << 1) | (write ? 1u : 0u);
	for (int i = 0; i < SWIO_HDR_BITS; i++) {
		int b = (hdr >> (SWIO_HDR_BITS - 1 - i)) & 1;
		swio_stats_one(s, write ? (b ? SWS_W1 : SWS_W0) : (b ? SWS_H1 : SWS_H0), cap[i]);
	}
	for (int i = 0; i < 32; i++) {
		int b = (val_or_expect >> (31 - i)) & 1;
		swio_stats_one(s, write ? (b ? SWS_W1 : SWS_W0) : (b ? SWS_R1 : SWS_R0),
		               cap[SWIO_HDR_BITS + i]);
	}
}

#endif
