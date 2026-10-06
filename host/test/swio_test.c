/*
 * swio_test: the SWIO packet encoding (firmware/swio_bits.h) against the
 * bit sequence proven on hardware by machdyne/basic's Werkzeug programmer
 * (start, 7-bit register, 1 = write, 32 bits, most significant first),
 * and the decoding of read slots.
 */

#include <stdio.h>
#include <stdlib.h>
#include "swio_bits.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	uint8_t p[SWIO_BITS], cap[SWIO_BITS];
	srand(1);
	for (int t = 0; t < 100000; t++) {
		uint8_t reg = (uint8_t)(rand() & 0x7f);
		uint32_t val = (uint32_t)rand() ^ ((uint32_t)rand() << 16);

		/* a write: (1 << 40) | reg << 33 | 1 << 32 | val, 41 bits */
		uint64_t want = (1ull << 40) | ((uint64_t)reg << 33) | (1ull << 32) | val;
		swio_encode(p, 1, reg, val, 6, 40, 7);
		for (int i = 0; i < SWIO_BITS; i++) {
			int b = (int)((want >> (40 - i)) & 1);
			CHECK(p[i] == (b ? 6 : 40), "write reg %02x val %08x bit %d", reg, val, i);
		}

		/* a read: (1 << 8) | reg << 1, 9 bits, then 32 read slots */
		uint32_t hdr = (1u << 8) | ((uint32_t)reg << 1);
		swio_encode(p, 0, reg, 0, 6, 40, 7);
		for (int i = 0; i < SWIO_HDR_BITS; i++)
			CHECK(p[i] == (((hdr >> (8 - i)) & 1) ? 6 : 40), "read header bit %d", i);
		for (int i = SWIO_HDR_BITS; i < SWIO_BITS; i++)
			CHECK(p[i] == 7, "read slot %d", i);

		/* the chip answers val: 1s rise early, 0s late */
		for (int i = 0; i < SWIO_BITS; i++) cap[i] = (uint8_t)(p[i] + 4);
		for (int i = 0; i < 32; i++)
			if (!((val >> (31 - i)) & 1)) cap[SWIO_HDR_BITS + i] = (uint8_t)(30 + rand() % 15);
		uint32_t got = 0;
		CHECK(swio_decode(cap, SWIO_BITS, 24, &got) == 0 && got == val,
		      "decode %08x got %08x", val, got);
		CHECK(swio_decode(cap, SWIO_BITS - 1, 24, &got) == -1, "short packet accepted");
		CHECK(swio_decode(cap, SWIO_BITS + 1, 24, &got) == -1, "long packet accepted");

		/* statistics sort the slots by the value expected */
		struct swio_stats s;
		swio_stats_init(&s);
		swio_stats_add(&s, cap, 0, reg, val);
		if (val != 0) CHECK(s.max[SWS_R1] == 11 && s.min[SWS_R1] == 11, "read 1s");
		if (val != 0xffffffffu) CHECK(s.min[SWS_R0] >= 30, "read 0s");
		CHECK(s.min[SWS_H1] == 10 && s.max[SWS_H1] == 10, "header 1s");
		if (failures > 20) break;
	}
	printf(failures ? "swio_test: FAILED (%d)\n" : "swio_test: 100000 packets: passed\n", failures);
	return failures != 0;
}
