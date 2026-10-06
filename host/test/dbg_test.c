/*
 * dbg_test: firmware/ch32dbg.c against a simulated CH32V003 and CH32V005
 * (chipsim.h): connecting (attached, and by power cycle), stopping,
 * identifying, reading memory, the CRC, restarting, and that nothing
 * ever writes the target's memory.
 */

#include <stdio.h>
#include "ch32dbg.h"
#include "chipsim.h"

uint32_t dbg_us(void) { return 0; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t flash_crc(void)
{
	return dbg_crc32_update(0, chip.flash, chip.size);
}

static void whole_flash_reads_back(const char *what)
{
	static uint32_t w[SIM_FLASH_MAX / 4];
	for (uint32_t a = 0; a < chip.size; a += 4 * 29) {
		int n = (chip.size - a) / 4 < 29 ? (int)((chip.size - a) / 4) : 29;
		CHECK(dbg_mem_read(SIM_FLASH + a, w + a / 4, n) == DBG_OK, "%s: read at %x", what, a);
	}
	CHECK(dbg_crc32_update(0, (uint8_t *)w, chip.size) == flash_crc(), "%s: contents", what);
	uint32_t crc = 0;
	CHECK(dbg_crc32(SIM_FLASH, chip.size, &crc) == DBG_OK && crc == flash_crc(),
	      "%s: device CRC %08x, want %08x", what, crc, flash_crc());
}

int main(void)
{
	struct dbg_info d;

	/* the CRC is zlib's (the standard check value) */
	CHECK(dbg_crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32");

	/* 1. a running CH32V003: attach, stop, identify, read, restart */
	chip_new(0, 1);
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_OK, "v003 connect");
	CHECK(d.how == DBG_HOW_ATTACH && sim_power_cycles == 0, "v003: attached, no power cycle");
	CHECK(chip.halted && dbg_halted, "v003 halted");
	CHECK(d.chipid_dm == 0x00300500 && d.chipid_mem == 0x00300500, "v003 ids");
	CHECK((d.hartinfo & 0x7ff) == 0x0f4, "v003 hartinfo");
	CHECK((d.flacap & 0xffff) == 16 && !(d.obr & 2), "v003 size, not protected");
	CHECK(d.uid[0] == 0x11223344 && d.uid[2] == 0x99AABBCC, "v003 uid");
	CHECK((d.cfgr >> 16) == 0x5AA5, "v003 cfgr");
	whole_flash_reads_back("v003");
	CHECK(dbg_resume() == DBG_OK && chip.running && !chip.halted && !dbg_halted, "v003 resume");
	uint32_t w;
	CHECK(dbg_mem_read(SIM_FLASH, &w, 1) == DBG_NOT_HALTED, "read without halt refused");

	/* 2. a CH32V005 */
	chip_new(1, 2);
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_OK, "v005 connect");
	CHECK((d.chipid_dm >> 20) == 0x005 && (d.flacap & 0xffff) == 32, "v005 identity");
	whole_flash_reads_back("v005");
	dbg_resume();

	/* 3. nothing in the contacts: attach only does not power-cycle;
	 * auto tries one restart */
	chip_new(0, 3);
	chip.absent = 1;
	sim_power_cycles = 0;
	CHECK(dbg_connect(DBG_MODE_ATTACH, &d) == DBG_NO_CHIP && d.cfgr == 0xFFFFFFFF,
	      "absent, attach: %08x", d.cfgr);
	CHECK(sim_power_cycles == 0, "attach must not power-cycle");
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_NO_CHIP && sim_power_cycles == 1,
	      "absent, auto: one power cycle (%ld)", sim_power_cycles);

	/* 4. a program that turns SWIO off: attaching fails, the power
	 * cycle catches it before it does */
	chip_new(0, 4);
	chip.swio_off_fw = 1;
	chip.boot_left = 0;                 /* it has been running for a while */
	sim_power_cycles = 0;
	CHECK(dbg_connect(DBG_MODE_ATTACH, &d) == DBG_NO_CHIP, "swio off: attach fails");
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_OK && d.how == DBG_HOW_POWER,
	      "swio off: caught after a power cycle");
	CHECK(sim_power_cycles == 1, "one power cycle");
	whole_flash_reads_back("caught");
	dbg_resume();

	/* 5. restart on request, with a well-behaved chip */
	chip_new(0, 5);
	sim_power_cycles = 0;
	CHECK(dbg_connect(DBG_MODE_RESTART, &d) == DBG_OK && d.how == DBG_HOW_POWER &&
	      sim_power_cycles == 1, "restart mode");
	dbg_resume();

	/* 6. read-protected: identified and reported, never unlocked */
	chip_new(0, 6);
	chip.rdprt = 1;
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_OK && (d.obr & 2), "protected reported");
	dbg_resume();

	/* 7. damaged packets: a read either succeeds with the right data or
	 * fails; it never returns wrong data */
	chip_new(0, 7);
	CHECK(dbg_connect(DBG_MODE_AUTO, &d) == DBG_OK, "connect before damage");
	sim_incomplete_pct = 20;    /* (reads retry: heavy damage to see failures) */
	int ok = 0, failed = 0;
	for (int t = 0; t < 3000; t++) {
		uint32_t a = (uint32_t)(rand() % (chip.size / 4)) * 4, v[1];
		int r = dbg_mem_read(SIM_FLASH + a, v, 1);
		if (r == DBG_OK) {
			ok++;
			uint32_t want = chip.flash[a] | chip.flash[a + 1] << 8 |
			                chip.flash[a + 2] << 16 | (uint32_t)chip.flash[a + 3] << 24;
			CHECK(v[0] == want, "damaged link returned wrong data at %x", a);
		} else {
			failed++;
		}
		if (failures > 5) break;
	}
	sim_incomplete_pct = 0;
	CHECK(failed > 0 && ok > 0, "damage was exercised (%d ok, %d failed)", ok, failed);
	dbg_resume();

	/* 8. phase 3 is read-only */
	CHECK(sim_mem_writes == 0, "%ld writes to target memory", sim_mem_writes);

	if (failures)
		printf("dbg_test: FAILED (%d)\n", failures);
	else
		printf("dbg_test: connect, power-cycle catch, identify, read, CRC, resume: "
		       "passed (%ld debug transactions)\n", sim_dmi);
	return failures != 0;
}
