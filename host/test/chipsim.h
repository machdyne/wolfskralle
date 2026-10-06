/*
 * chipsim: a simulated CH32V003 or CH32V005 behind the Wolfskralle's SWIO,
 * for the tests of firmware/ch32dbg.c. It provides the dbg_* hooks.
 *
 * Adapted from machdyne/basic's tools/sechs/test/ch32sim.h (the simulated
 * chip of the Werkzeug programmer's tests), with what the Wolfskralle
 * adds: target power that the programmer switches, and SWIO as the
 * Wolfskralle sees it (a chip that does not answer reads as all ones; a
 * damaged packet is reported as incomplete).
 *
 * It is stricter than the real chip: abstract commands only while halted;
 * flash writes only through the controller's page programming, and
 * anything the failsafe rules forbid (docs/programming.md) is counted in
 * sim_violations. A power cut can be scheduled at any debug transaction
 * (sim_cut_at); during an erase or program it tears the page.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

#define SIM_FLASH       0x08000000u
#define SIM_FLASH_MAX   32768u

static struct {
	/* kept across power cycles */
	uint8_t flash[SIM_FLASH_MAX];
	uint32_t size;
	uint32_t chip_id, id7f, hartinfo, flacap, uid[3];
	int rdprt;              /* read-protected */
	int absent;             /* nothing in the contacts */
	int swio_off_fw;        /* its program turns SWIO off once running */
	int power;              /* target power */
	/* lost at a power cycle or reset */
	int cfg_ok, halted, running, boot_left;
	uint32_t data0, progbuf[8], x8, x9, x10, x11, cmderr, autoexec, lastcmd;
	int progbufsize;        /* words (8 like basic's model; 1 tests the fallback) */
	int no_autoexec;        /* auto-execution not implemented */
	int wrong_autoexec;     /* auto-execution that misbehaves (a fallback test) */
	int locked, flocked, key_n, mkey_n;
	uint32_t page, ctlr, addr, statr, latch_a, latch_v, pagebuf[64];
} chip;

static long sim_mem_writes, sim_power_cycles, sim_dmi, sim_resumes;
static long sim_violations, sim_erases, sim_programs;
static long sim_read_past;              /* reads just past the flash's end */
static int sim_incomplete_pct;          /* damaged packets, percent */
static long sim_cut_at = -1;            /* a power cut at this transaction */
static jmp_buf sim_cut;

static void violation(const char *what, uint32_t a)
{
	if (sim_violations++ < 5) printf("  violation: %s at %08x\n", what, a);
}

static void chip_boot(void)
{
	chip.cfg_ok = chip.halted = 0;
	chip.running = 1;
	chip.boot_left = 6;     /* transactions until its program turns SWIO off */
	chip.cmderr = 0;
	chip.autoexec = 0;
	chip.lastcmd = 0;
	chip.locked = chip.flocked = 1;
	chip.key_n = chip.mkey_n = 0;
	chip.ctlr = chip.statr = chip.addr = 0;
}

static void chip_new(int v005, uint32_t fill)
{
	memset(&chip, 0, sizeof(chip));
	chip.size = v005 ? 32768 : 16384;
	chip.page = v005 ? 256 : 64;
	chip.id7f = v005 ? 0x00500500 : 0x00300500;
	chip.chip_id = v005 ? 0xFFFFFFFF : 0x00300500;
	chip.hartinfo = v005 ? 0x00212000 : 0x002120f4;
	chip.flacap = v005 ? 0xFFFF0020 : 0xFFFF0010;
	chip.uid[0] = 0x11223344; chip.uid[1] = 0x55667788; chip.uid[2] = 0x99AABBCC;
	for (uint32_t i = 0; i < chip.size; i++) chip.flash[i] = (uint8_t)(fill + i * 7 + (i >> 8));
	chip.progbufsize = 8;
	chip.power = 1;
	chip_boot();
}

static int answers(void)
{
	if (chip.absent || !chip.power) return 0;
	if (chip.running && chip.boot_left > 0) chip.boot_left--;
	if (chip.swio_off_fw && chip.running && chip.boot_left == 0) return 0;
	return 1;
}

static uint32_t mem_rd(uint32_t a)
{
	if (a >= SIM_FLASH + chip.size && a < SIM_FLASH + chip.size + 0x100) sim_read_past++;
	if (a >= SIM_FLASH && a + 3 < SIM_FLASH + chip.size) {
		if (chip.rdprt) return 0xE339E339;      /* (what a protected flash gives) */
		uint32_t o = a - SIM_FLASH;
		return chip.flash[o] | chip.flash[o + 1] << 8 | chip.flash[o + 2] << 16 |
		       (uint32_t)chip.flash[o + 3] << 24;
	}
	if (a == 0x1FFFF7C4) return chip.chip_id;
	if (a == 0x1FFFF7E0) return chip.flacap;
	if (a >= 0x1FFFF7E8 && a < 0x1FFFF7F4) return chip.uid[(a - 0x1FFFF7E8) / 4];
	if (a == 0x4002201C) return chip.rdprt ? 2 : 0;
	if (a == 0x40022010) return chip.ctlr | (chip.locked ? 0x80 : 0) | (chip.flocked ? 0x8000 : 0);
	if (a == 0x4002200C) return chip.statr;
	return 0;
}

/* a torn erase or program: some bytes changed, some not */
static void tear_page(uint32_t a, int erase)
{
	for (uint32_t i = 0; i < chip.page; i++) {
		if (rand() & 1) continue;
		uint8_t *b = &chip.flash[a - SIM_FLASH + i];
		if (erase) *b = 0xFF;
		else *b &= ((uint8_t *)chip.pagebuf)[i] | (uint8_t)rand();
	}
}

static void mem_wr(uint32_t a, uint32_t v)
{
	sim_mem_writes++;
	if (a >= SIM_FLASH && a < SIM_FLASH + chip.size) {
		if (!(chip.ctlr & 0x10000) || chip.locked || chip.flocked)
			violation("write to flash outside page programming", a);
		chip.latch_a = a;
		chip.latch_v = v;
		return;
	}
	switch (a) {
	case 0x40022004:    /* KEYR */
		if (chip.key_n == 0 && v == 0x45670123) chip.key_n = 1;
		else if (chip.key_n == 1 && v == 0xCDEF89AB) chip.locked = 0;
		else chip.key_n = 0;
		return;
	case 0x40022024:    /* MODEKEYR */
		if (chip.mkey_n == 0 && v == 0x45670123) chip.mkey_n = 1;
		else if (chip.mkey_n == 1 && v == 0xCDEF89AB && !chip.locked) chip.flocked = 0;
		else chip.mkey_n = 0;
		return;
	case 0x4002200C: chip.statr = 0; return;
	case 0x40022014: chip.addr = v; return;
	case 0x40022010:    /* CTLR */
		if (v & 0x80) {
			chip.locked = chip.flocked = 1;
			chip.ctlr = 0;
			return;
		}
		if (chip.locked || chip.flocked) {
			chip.statr |= 0x10;     /* WRPRTERR */
			return;
		}
		chip.ctlr = v & ~0x40u;
		if ((v & 0x80000) && (v & 0x10000)) memset(chip.pagebuf, 0xFF, chip.page);
		if ((v & 0x40000) && (v & 0x10000))
			chip.pagebuf[(chip.latch_a & (chip.page - 1)) / 4] = chip.latch_v;
		if (v & 0x40) {         /* STRT */
			uint32_t p = chip.addr;
			if (p < SIM_FLASH || p >= SIM_FLASH + chip.size || (p & (chip.page - 1))) {
				violation("erase or program outside the main flash", p);
				chip.statr |= 0x10;
				return;
			}
			int erase = (v & 0x20000) != 0;
			if (sim_dmi == sim_cut_at) {
				tear_page(p, erase);
				longjmp(sim_cut, 1);
			}
			if (erase) {
				memset(&chip.flash[p - SIM_FLASH], 0xFF, chip.page);
				sim_erases++;
			} else if (v & 0x10000) {
				for (uint32_t i = 0; i < chip.page; i++)
					chip.flash[p - SIM_FLASH + i] &= ((uint8_t *)chip.pagebuf)[i];
				sim_programs++;
			}
			chip.statr |= 0x20;     /* EOP */
		}
		return;
	}
	if (a == 0x40022008) violation("option byte key (OBKEYR)", a);
	else if (a >= 0x1FFFF800 && a < 0x1FFFF900) violation("option bytes", a);
	else violation("write to an unexpected address", a);
}

/* runs the program buffer; past its end, an implicit ebreak */
static void execute(void)
{
	for (int i = 0; i < 2 * chip.progbufsize; i++) {
		uint16_t ins = (chip.progbuf[i / 2] >> (16 * (i & 1))) & 0xFFFF;
		if (ins == 0x9002) return;                          /* c.ebreak */
		if (ins == 0x4000) chip.x8 = mem_rd(chip.x8);       /* c.lw x8,0(x8) */
		else if (ins == 0x4004) chip.x9 = mem_rd(chip.x8);  /* c.lw x9,0(x8) */
		else if (ins == 0xC004) mem_wr(chip.x8, chip.x9);   /* c.sw x9,0(x8) */
		else if (ins == 0xC10C) mem_wr(chip.x10, chip.x11); /* c.sw x11,0(x10) */
		else if (ins == 0x0411) chip.x8 += 4;               /* c.addi x8,4 */
		else if (ins == 0x0001) ;                           /* c.nop */
		else { chip.cmderr = 3; return; }
	}
}

static void command(uint32_t c)
{
	chip.lastcmd = c;
	if (chip.cmderr) return;
	if (!chip.halted) { chip.cmderr = 4; return; }
	if ((c >> 24) != 0 || ((c >> 20) & 7) != 2) { chip.cmderr = 2; return; }
	if (c & 0x20000) {
		uint32_t *r = (c & 0xFFFF) == 0x1008 ? &chip.x8 :
		              (c & 0xFFFF) == 0x1009 ? &chip.x9 :
		              (c & 0xFFFF) == 0x100A ? &chip.x10 :
		              (c & 0xFFFF) == 0x100B ? &chip.x11 : NULL;
		if (!r) { chip.cmderr = 2; return; }
		if (c & 0x10000) *r = chip.data0;
		else chip.data0 = *r;
	}
	if (c & 0x40000) execute();
}

/* auto-execution: an access to DATA0 repeats the last command */
static void autoexec(void)
{
	if (!(chip.autoexec & 1) || chip.no_autoexec) return;
	if (chip.wrong_autoexec) { chip.x8 += 4; return; }     /* skips a word */
	command(chip.lastcmd);
}

static int damaged(void)
{
	return sim_incomplete_pct && rand() % 100 < sim_incomplete_pct;
}

int dbg_dmi_write(uint8_t reg, uint32_t v)
{
	if (++sim_dmi == sim_cut_at) longjmp(sim_cut, 1);
	if (damaged()) return -1;           /* (the chip may or may not have it) */
	if (!answers()) return 0;           /* writes are not acknowledged */
	switch (reg) {
	case 0x7D: case 0x7E:
		if ((v >> 16) == 0x5AA5) chip.cfg_ok = 1;
		break;
	case 0x10:
		if (v & 2) {                    /* core reset */
			int cfg = chip.cfg_ok;
			chip_boot();
			chip.cfg_ok = cfg;
			chip.halted = (v >> 31) & 1;
			chip.running = !chip.halted;
		} else if (v >> 31) {
			chip.halted = 1;
			chip.running = 0;
		}
		if ((v >> 30) & 1) {
			chip.halted = 0;
			chip.running = 1;
			sim_resumes++;
		}
		break;
	case 0x04: chip.data0 = v; autoexec(); break;
	case 0x16: if (v & 0x700) chip.cmderr = 0; break;
	case 0x17: command(v); break;
	case 0x18: chip.autoexec = v; break;
	default:
		if (reg >= 0x20 && reg < 0x20 + chip.progbufsize) chip.progbuf[reg - 0x20] = v;
	}
	return 0;
}

int dbg_dmi_read(uint8_t reg, uint32_t *v)
{
	if (++sim_dmi == sim_cut_at) longjmp(sim_cut, 1);
	if (damaged()) return -1;
	if (!answers() || !chip.cfg_ok) { *v = 0xFFFFFFFF; return 0; }  /* nobody pulls low */
	switch (reg) {
	case 0x7D: *v = 0x5AA50400; break;
	case 0x11: *v = 0x00000002 | (chip.halted ? 0x300 : 0xC00); break;
	case 0x12: *v = chip.hartinfo; break;
	case 0x7F: *v = chip.id7f; break;
	case 0x04: *v = chip.data0; autoexec(); break;
	case 0x18: *v = chip.autoexec; break;
	case 0x16: *v = (uint32_t)chip.progbufsize << 24 | 0x00000002 | chip.cmderr << 8; break;
	default: *v = 0;
	}
	return 0;
}

void dbg_delay_us(uint32_t us) { (void)us; }

void dbg_target_power(int on)
{
	if (on && !chip.power) {
		sim_power_cycles++;
		chip.power = 1;
		chip_boot();
	} else if (!on) {
		chip.power = 0;
	}
}

int dbg_have_reset(void) { return 0; }          /* board v0 */
void dbg_reset_line(int hold) { (void)hold; }
