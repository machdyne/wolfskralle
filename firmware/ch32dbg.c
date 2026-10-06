/*
 * ch32dbg: the target's debug module. See ch32dbg.h and docs/debug.md.
 *
 * The sequences follow machdyne/basic's tools/sechs/ch32prog.c, which
 * programs CH32V003 modules from Werkzeug and was verified on an LS10A
 * (connect, halt with a core reset, identify, memory reads through the
 * program buffer, release).
 */

#include "ch32dbg.h"

int dbg_halted;
uint32_t dbg_flash_size, dbg_page;
int dbg_chip, dbg_unlocked;
uint32_t dbg_refused;
static int dbg_rdprt;
static uint32_t pb0, pb1;       /* what the program buffer holds (0: unknown) */
int dbg_fast;
int dbg_fast_allowed = DBG_FAST_READ | DBG_FAST_WRITE;
struct dbg_prof dbg_prof;

/* abstract commands (RISC-V debug spec): access register x8/x9 (32 bits) */
#define CMD_READ_X8     0x00221008u     /* DATA0 = x8 */
#define CMD_WRITE_X8    0x00231008u     /* x8 = DATA0 */
#define CMD_POSTEXEC    0x00040000u     /* then run the program buffer */
#define CMD_WRITE_X9    0x00231009u     /* x9 = DATA0 */
#define CMD_READ_X9     0x00221009u     /* DATA0 = x9 */
#define CMD_WRITE_X10   0x0023100Au     /* x10 = DATA0 */
#define CMD_WRITE_X11   0x0023100Bu     /* x11 = DATA0 */
#define CMD_EXEC        0x00240000u     /* run the program buffer only */

/* the fast paths: two-word programs (PROGBUF0, PROGBUF1) */
#define PB_RD0          0x04114004u     /* c.lw x9, 0(x8); c.addi x8, 4 */
#define PB_RD1          0x00019002u     /* c.ebreak; c.nop */
#define PB_WR0          0xC10CC004u     /* c.sw x9, 0(x8); c.sw x11, 0(x10) */
#define PB_WR1          0x90020411u     /* c.addi x8, 4; c.ebreak */
#define PB_LW_X8_X8     0x90024000u     /* c.lw x8, 0(x8); c.ebreak */
#define PB_SW_X9_X8     0x9002C004u     /* c.sw x9, 0(x8); c.ebreak */

/* DMCONTROL */
#define DMC_ACTIVE      0x00000001u
#define DMC_NDMRESET    0x00000002u     /* (WCH: resets the core) */
#define DMC_ACKHAVERESET 0x10000000u
#define DMC_RESUMEREQ   0x40000000u
#define DMC_HALTREQ     0x80000000u

#define DMS_ALLHALTED   (1u << 9)

/* waits for an abstract command; clears and reports an error */
static int done(void)
{
	uint32_t cs;
	for (int t = 0; t < 100; t++) {
		if (dbg_dmi_read(DM_ABSTRACTCS, &cs)) continue;     /* a damaged packet: ask again */
		if (!(cs & (1u << 12))) {               /* not busy */
			if (cs & (7u << 8)) {               /* cmderr */
				dbg_dmi_write(DM_ABSTRACTCS, 7u << 8);
				return -1;
			}
			return 0;
		}
	}
	return -1;
}

/* the key that enables the chip's debug output, and a halt request */
static void wake(void)
{
	dbg_dmi_write(DM_SHDWCFGR, DM_CFG_KEY);
	dbg_dmi_write(DM_CFGR, DM_CFG_KEY);
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_ACTIVE);
}

/* 1 if the configuration register reads back the key: only a chip does */
static int answers(struct dbg_info *info)
{
	uint32_t v = 0;
	for (int i = 0; i < 2; i++) {
		dbg_dmi_write(DM_SHDWCFGR, DM_CFG_KEY);
		dbg_dmi_write(DM_CFGR, DM_CFG_KEY);
	}
	int ok = !dbg_dmi_read(DM_CFGR, &v);
	info->cfgr = v;
	return ok && (v & 0xFFFF0000u) == 0x5AA50000u;
}

/* Restarts the target and asks it to stop as early as possible, before
 * its program runs far (or turns SWIO off). Board v1 holds it in reset;
 * board v0 can only switch its power off and on. */
static int restart_and_catch(struct dbg_info *info)
{
	if (dbg_have_reset()) {
		info->how = DBG_HOW_RESET;
		dbg_reset_line(1);
		dbg_delay_us(1000);
		dbg_reset_line(0);
		wake();
		return answers(info);
	}
	info->how = DBG_HOW_POWER;
	dbg_target_power(0);
	dbg_delay_us(100000);       /* TPWR's 10 uF discharge (D1, 1K5: ~15 ms) */
	dbg_target_power(1);
	/* the chip starts within a few ms of power: knock until it answers */
	for (int t = 0; t < 200; t++) {
		wake();
		if (answers(info)) return 1;
		dbg_delay_us(250);
	}
	return 0;
}

/* Stops the core, then resets it with the halt request held, so it waits
 * at its reset vector. */
static int halt(struct dbg_info *info)
{
	uint32_t st = 0;
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_ACTIVE);
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_ACTIVE);
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_NDMRESET | DMC_ACTIVE);
	dbg_delay_us(2000);
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_ACTIVE);
	dbg_dmi_write(DM_DMCONTROL, DMC_HALTREQ | DMC_ACKHAVERESET | DMC_ACTIVE);
	for (int t = 0; t < 100; t++) {
		if (!dbg_dmi_read(DM_DMSTATUS, &st) && (st & DMS_ALLHALTED)) {
			dbg_dmi_write(DM_DMCONTROL, DMC_ACTIVE);    /* clear the request */
			dbg_dmi_write(DM_AUTOEXEC, 0);
			dbg_dmi_write(DM_ABSTRACTCS, 7u << 8);
			info->dmstatus = st;
			return 0;
		}
		dbg_delay_us(1000);
	}
	info->dmstatus = st;
	return -1;
}

/* loads the program buffer, unless it already holds the program
 * (p1 0: a one-word program, PROGBUF1 does not matter) */
static int pb_load(uint32_t p0, uint32_t p1)
{
	if (pb0 != p0) {
		pb0 = 0;
		if (dbg_dmi_write(DM_PROGBUF0, p0)) return -1;
		pb0 = p0;
	}
	if (p1 && pb1 != p1) {
		pb1 = 0;
		if (dbg_dmi_write(DM_PROGBUF0 + 1, p1)) return -1;
		pb1 = p1;
	}
	return 0;
}

static int pb(uint32_t p) { return pb_load(p, 0); }

/* A SWIO packet can come back damaged (docs/swio.md). Then whether the
 * chip took it is unknown, so only what is safe to do twice is repeated:
 * a read, and a memory write that leaves the same state when repeated.
 * A flash key or the start of programming is never repeated here: a
 * failed one fails the step, and the step above starts over (the page is
 * erased again). */
#define TRIES 3

static int read_word_once(uint32_t addr, uint32_t *val)
{
	/* one check of ABSTRACTCS at the end: an error (or a command still
	 * busy when the next came: SWIO packets are tens of microseconds
	 * apart, the chip needs a few cycles) stays set until cleared */
	if (pb(PB_LW_X8_X8) ||
	    dbg_dmi_write(DM_DATA0, addr) ||
	    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X8 | CMD_POSTEXEC) ||
	    dbg_dmi_write(DM_COMMAND, CMD_READ_X8) ||
	    dbg_dmi_read(DM_DATA0, val) || done()) return -1;
	return 0;
}

static int read_word(uint32_t addr, uint32_t *val)
{
	for (int t = 0; t < TRIES; t++)
		if (!read_word_once(addr, val)) return 0;
	return -1;
}

/* Auto-execution must be off before anything else touches DATA0: left
 * on, the next DATA0 access would run the program buffer again. A damaged
 * packet leaves its state unknown, so it is switched off and read back
 * until it reads off; if that cannot be confirmed, the session ends
 * (dbg_halted = 0: nothing more is written until a new CONNECT). */
static int autoexec_off(void)
{
	for (int t = 0; t < 8; t++) {
		uint32_t v = 1;
		if (!dbg_dmi_write(DM_AUTOEXEC, 0) && !dbg_dmi_read(DM_AUTOEXEC, &v) && v == 0)
			return 0;
	}
	dbg_halted = 0;
	dbg_unlocked = 0;
	return -1;
}

/* n >= 3 words with auto-execution: one SWIO packet a word. The
 * program loads the word at x8 into x9 and steps x8; each read of DATA0
 * repeats "DATA0 = x9, run the program". It reads two words ahead, so
 * auto-execution stops two words before the end: nothing past addr+4n is
 * read. Any error (also "busy", if the chip were slower than SWIO) shows
 * in ABSTRACTCS at the end; then the caller reads the slow way. */
static int fast_read(uint32_t addr, uint32_t *w, int n)
{
	int i;
	if (pb_load(PB_RD0, PB_RD1) ||
	    dbg_dmi_write(DM_DATA0, addr) ||
	    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X8) ||
	    dbg_dmi_write(DM_COMMAND, CMD_EXEC) ||
	    dbg_dmi_write(DM_COMMAND, CMD_READ_X9 | CMD_POSTEXEC) ||
	    dbg_dmi_write(DM_AUTOEXEC, 1)) goto fail;
	for (i = 0; i < n - 2; i++)
		if (dbg_dmi_read(DM_DATA0, &w[i])) goto fail;
	if (autoexec_off() ||
	    dbg_dmi_read(DM_DATA0, &w[n - 2]) ||
	    dbg_dmi_write(DM_COMMAND, CMD_READ_X9) ||
	    dbg_dmi_read(DM_DATA0, &w[n - 1])) goto fail;
	return done();
fail:
	autoexec_off();
	done();
	return -1;
}

int dbg_mem_read(uint32_t addr, uint32_t *w, int n)
{
	if (!dbg_halted) return DBG_NOT_HALTED;
	if (addr & 3) return DBG_READ_ERR;
	if ((dbg_fast & DBG_FAST_READ) && n >= 3 && !fast_read(addr, w, n)) return DBG_OK;
	for (int i = 0; i < n; i++)
		if (read_word(addr + 4 * (uint32_t)i, &w[i])) return DBG_READ_ERR;
	return DBG_OK;
}

int dbg_connect(int mode, struct dbg_info *info)
{
	uint8_t *z = (uint8_t *)info;
	for (unsigned i = 0; i < sizeof(*info); i++) z[i] = 0;
	info->how = DBG_HOW_ATTACH;
	dbg_halted = 0;
	dbg_unlocked = 0;
	dbg_chip = 0;
	dbg_flash_size = dbg_page = 0;          /* the gate: shut */

	int ok = 0;
	if (mode != DBG_MODE_RESTART) {
		wake();
		ok = answers(info);
	}
	if (!ok && mode != DBG_MODE_ATTACH)
		ok = restart_and_catch(info);
	if (!ok) return DBG_NO_CHIP;

	pb0 = pb1 = 0;
	dbg_fast = 0;
	if (halt(info)) return DBG_NO_HALT;
	dbg_halted = 1;

	if (dbg_dmi_read(DM_HARTINFO, &info->hartinfo) ||
	    dbg_dmi_read(DM_CHIPID, &info->chipid_dm)) return DBG_READ_ERR;
	if (dbg_mem_read(CH32_CHIPID_ADDR, &info->chipid_mem, 1) ||
	    dbg_mem_read(CH32_FLASH_OBR, &info->obr, 1) ||
	    dbg_mem_read(CH32_FLACAP_ADDR, &info->flacap, 1) ||
	    dbg_mem_read(CH32_UID_ADDR, info->uid, 3)) return DBG_READ_ERR;

	/* the write gate opens only for the chips proven (machdyne/basic):
	 * a CH32V005 by its chip ID; a CH32V003 by its chip ID, hartinfo and
	 * the ID word in memory */
	uint32_t id = info->chipid_dm;
	if ((id & 0xFFF00000u) == 0x00500000u) {
		dbg_chip = 5;
		dbg_flash_size = 32768;
		dbg_page = 256;
	} else if ((!id || (id & 0xFFF00F00u) == 0x00300500u) &&
	           (info->chipid_mem >> 20) == 0x003 && (info->hartinfo & 0x7FF) == 0x0F4) {
		dbg_chip = 3;
		dbg_flash_size = 16384;
		dbg_page = 64;
	}
	dbg_rdprt = (info->obr & 2) != 0;

	/* the fast paths need a program buffer of two words; reads must give
	 * the same as the plain method (here: the start of the flash) */
	uint32_t cs = 0;
	if (!dbg_dmi_read(DM_ABSTRACTCS, &cs)) info->progbufsize = (cs >> 24) & 0x1F;
	if (info->progbufsize >= 2) {
		uint32_t a[8], b[8];
		int same = 1;
		if (dbg_mem_read(CH32_FLASH, a, 8)) same = 0;   /* (the plain way) */
		dbg_fast = DBG_FAST_READ;
		if (same && dbg_mem_read(CH32_FLASH, b, 8) == DBG_OK)
			for (int i = 0; i < 8; i++) if (a[i] != b[i]) same = 0;
		dbg_fast = (same ? DBG_FAST_READ : 0) | DBG_FAST_WRITE;
		dbg_fast &= dbg_fast_allowed;
	}
	info->fast = (uint8_t)dbg_fast;
	return DBG_OK;
}

/* ---- writing ---- */

/* The write gate: the identified chip's main flash, and the flash
 * controller registers that programming needs. Never OBKEYR (the option
 * byte key), the option bytes or anything else. */
static int allowed(uint32_t addr)
{
	if (addr >= CH32_FLASH && addr < CH32_FLASH + dbg_flash_size) return 1;
	if (!dbg_flash_size) return 0;
	return addr == FLASH_KEYR || addr == FLASH_STATR || addr == FLASH_CTLR ||
	       addr == FLASH_ADDR || addr == FLASH_MODEKEYR;
}

static int mem_write(uint32_t addr, uint32_t val)
{
	if (!allowed(addr)) {
		dbg_refused++;
		return DBG_FORBIDDEN;
	}
	/* not repeated: a flash key (a second one would break the sequence)
	 * and the start of programming; starting an erase twice is harmless */
	int once = addr == FLASH_KEYR || addr == FLASH_MODEKEYR ||
	           (addr == FLASH_CTLR && (val & CTLR_STRT) && !(val & CTLR_PAGE_ER));
	for (int t = 0; t < (once ? 1 : TRIES); t++) {
		if (pb(PB_SW_X9_X8) ||
		    dbg_dmi_write(DM_DATA0, val) ||
		    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X9) ||
		    dbg_dmi_write(DM_DATA0, addr) ||
		    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X8 | CMD_POSTEXEC) || done())
			continue;
		return DBG_OK;
	}
	return DBG_READ_ERR;
}

/* waits for the flash controller; clears its flags */
static int flash_wait(void)
{
	uint32_t s = STATR_BSY;
	for (int t = 0; t < 2000 && (s & STATR_BSY); t++) {
		if (read_word(FLASH_STATR, &s)) return -1;
		if (s & STATR_BSY) dbg_prof.waits++;
	}
	if (mem_write(FLASH_STATR, 0)) return -1;
	return (s & (STATR_BSY | STATR_WRPRTERR)) ? -1 : 0;
}

int dbg_flash_begin(void)
{
	uint32_t c = 0;
	dbg_unlocked = 0;
	if (!dbg_halted) return DBG_NOT_HALTED;
	if (!dbg_chip) return DBG_UNSUPPORTED;
	if (dbg_rdprt) return DBG_PROTECTED;
	if (mem_write(FLASH_KEYR, DBG_FLASH_KEY1) || mem_write(FLASH_KEYR, DBG_FLASH_KEY2) ||
	    mem_write(FLASH_MODEKEYR, DBG_FLASH_KEY1) || mem_write(FLASH_MODEKEYR, DBG_FLASH_KEY2) ||
	    read_word(FLASH_CTLR, &c)) return DBG_LOCKED;
	if (c & (CTLR_LOCK | CTLR_FLOCK)) return DBG_LOCKED;
	dbg_unlocked = 1;
	return DBG_OK;
}

int dbg_flash_end(void)
{
	if (!dbg_unlocked) return DBG_OK;
	dbg_unlocked = 0;
	return mem_write(FLASH_CTLR, CTLR_LOCK) ? DBG_READ_ERR : DBG_OK;
}

static int erase_page(uint32_t a)
{
	if (flash_wait() ||
	    mem_write(FLASH_CTLR, CTLR_PAGE_ER) ||
	    mem_write(FLASH_ADDR, a) ||
	    mem_write(FLASH_CTLR, CTLR_PAGE_ER | CTLR_STRT) ||
	    flash_wait() || mem_write(FLASH_CTLR, 0)) return -1;
	return 0;
}

/* fast page programming: buffer reset, each word loaded, start */
static int program_page(uint32_t a, const uint32_t *w)
{
	if (mem_write(FLASH_CTLR, CTLR_PAGE_PG) ||
	    mem_write(FLASH_CTLR, CTLR_PAGE_PG | CTLR_BUF_RST) || flash_wait())
		return -1;
	for (uint32_t i = 0; i < dbg_page / 4; i++)
		if (mem_write(a + 4 * i, w[i]) ||
		    mem_write(FLASH_CTLR, CTLR_PAGE_PG | CTLR_BUF_LOAD) || flash_wait())
			return -1;
	if (mem_write(FLASH_ADDR, a) ||
	    mem_write(FLASH_CTLR, CTLR_PAGE_PG | CTLR_STRT) || flash_wait() ||
	    mem_write(FLASH_CTLR, 0)) return -1;
	return 0;
}

/* the same page programming with auto-execution: one SWIO packet a word.
 * x8 = the page, x10 = FLASH_CTLR, x11 = PAGE_PG | BUF_LOAD; each write of
 * DATA0 runs "x9 = DATA0; store x9 at x8; store x11 at x10 (load the
 * word into the page buffer); x8 += 4". The program stores only inside
 * this page and to FLASH_CTLR, which the gate is asked about first.
 * *damaged: a SWIO packet came back damaged (then a failure says nothing
 * about the fast path itself). */
static int fast_program(uint32_t a, const uint32_t *w, int *damaged)
{
	uint32_t i, n = dbg_page / 4;
	*damaged = 0;
	if (!allowed(a) || !allowed(a + dbg_page - 4) || !allowed(FLASH_CTLR)) {
		dbg_refused++;
		return -2;
	}
	/* loading the page buffer can be repeated (each word goes to its own
	 * place in it), so a damaged packet costs a reload, not an erase */
	for (int t = 0; t < TRIES; t++) {
		if (mem_write(FLASH_CTLR, CTLR_PAGE_PG) ||
		    mem_write(FLASH_CTLR, CTLR_PAGE_PG | CTLR_BUF_RST) || flash_wait())
			return -1;
		if (pb_load(PB_WR0, PB_WR1) ||
		    dbg_dmi_write(DM_DATA0, FLASH_CTLR) || dbg_dmi_write(DM_COMMAND, CMD_WRITE_X10) ||
		    dbg_dmi_write(DM_DATA0, CTLR_PAGE_PG | CTLR_BUF_LOAD) ||
		    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X11) ||
		    dbg_dmi_write(DM_DATA0, a) || dbg_dmi_write(DM_COMMAND, CMD_WRITE_X8) ||
		    done()) goto damaged;
		if (dbg_dmi_write(DM_DATA0, w[0]) ||
		    dbg_dmi_write(DM_COMMAND, CMD_WRITE_X9 | CMD_POSTEXEC) ||
		    dbg_dmi_write(DM_AUTOEXEC, 1)) goto damaged;
		for (i = 1; i < n; i++)
			if (dbg_dmi_write(DM_DATA0, w[i])) goto damaged;
		if (autoexec_off()) goto damaged;
		if (done()) return -1;          /* the chip refused or was busy */
		if (flash_wait() ||
		    mem_write(FLASH_ADDR, a) ||
		    mem_write(FLASH_CTLR, CTLR_PAGE_PG | CTLR_STRT) || flash_wait() ||
		    mem_write(FLASH_CTLR, 0)) return -1;
		return 0;
damaged:
		*damaged = 1;
		if (autoexec_off()) return -1;  /* (the session has ended) */
		done();
	}
	return -1;
}

/* 1 if the page reads back as w; 0 if not, or if a read failed */
static int page_matches(uint32_t a, const uint32_t *w)
{
	uint32_t v[16];
	for (uint32_t i = 0; i < dbg_page / 4; i += 16) {
		if (dbg_mem_read(a + 4 * i, v, 16)) return 0;
		for (int k = 0; k < 16; k++)
			if (v[k] != w[i + k]) return 0;
	}
	return 1;
}

int dbg_page_write(uint32_t addr, const uint32_t *w, int differs, int *written,
                   int *attempts)
{
	uint32_t t0;
	*written = 0;
	*attempts = 0;
	if (!dbg_halted) return DBG_NOT_HALTED;
	if (!dbg_unlocked) return DBG_NOT_STARTED;
	if (addr < CH32_FLASH || addr >= CH32_FLASH + dbg_flash_size ||
	    ((addr - CH32_FLASH) & (dbg_page - 1))) {
		dbg_refused++;
		return DBG_FORBIDDEN;
	}
	t0 = dbg_us();
	int same = !differs && page_matches(addr, w);
	dbg_prof.precheck_us += dbg_us() - t0;
	if (same) return DBG_OK;                        /* already so: untouched */
	dbg_prof.pages++;
	uint32_t refused = dbg_refused;

	int blank = 1;
	for (uint32_t i = 0; i < dbg_page / 4; i++)
		if (w[i] != 0xFFFFFFFFu) blank = 0;

	/* up to five attempts: a damaged SWIO packet can cost one */
	for (int attempt = 1; attempt <= 5; attempt++) {
		*attempts = attempt;
		*written = 1;
		t0 = dbg_us();
		int er = erase_page(addr);
		dbg_prof.erase_us += dbg_us() - t0;
		if (er) continue;
		int used_fast = 0, damaged = 0;
		if (!blank) {
			int fast = (dbg_fast & DBG_FAST_WRITE) != 0;
			used_fast = fast;
			t0 = dbg_us();
			int r = fast ? fast_program(addr, w, &damaged) : program_page(addr, w);
			dbg_prof.program_us += dbg_us() - t0;
			if (r == -2) return DBG_FORBIDDEN;
			if (r) {
				if (fast && !damaged) { dbg_fast &= ~DBG_FAST_WRITE; dbg_prof.fallbacks++; }
				continue;
			}
		}
		t0 = dbg_us();
		int ok = page_matches(addr, w);
		dbg_prof.verify_us += dbg_us() - t0;
		if (ok) return DBG_OK;
		/* written the fast way and wrong, with no damaged packet: this
		 * chip does not take it; the plain way from now on */
		if (used_fast && !damaged && (dbg_fast & DBG_FAST_WRITE)) {
			dbg_fast &= ~DBG_FAST_WRITE;
			dbg_prof.fallbacks++;
		}
	}
	if (!dbg_halted) return DBG_NOT_HALTED;     /* (auto-execution stuck: session ended) */
	return dbg_refused != refused ? DBG_FORBIDDEN : DBG_VERIFY;
}

int dbg_resume(void)
{
	if (dbg_halted) dbg_flash_end();        /* the flash locked again */
	dbg_halted = 0;
	dbg_unlocked = 0;
	dbg_dmi_write(DM_DMCONTROL, DMC_NDMRESET | DMC_ACTIVE);     /* core reset, no halt */
	dbg_delay_us(2000);
	dbg_dmi_write(DM_DMCONTROL, DMC_ACTIVE);
	dbg_dmi_write(DM_DMCONTROL, DMC_ACKHAVERESET | DMC_ACTIVE);
	dbg_dmi_write(DM_DMCONTROL, DMC_RESUMEREQ | DMC_ACTIVE);
	if (dbg_have_reset()) {                                     /* a clean start */
		dbg_reset_line(1);
		dbg_delay_us(1000);
		dbg_reset_line(0);
	}
	return DBG_OK;
}

uint32_t dbg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

static int dbg_crc32_2(uint32_t addr, uint32_t len, uint32_t *crc);

int dbg_crc32(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint32_t t0 = dbg_us();
	int r = dbg_crc32_2(addr, len, crc);
	dbg_prof.read_us += dbg_us() - t0;
	return r;
}

static int dbg_crc32_2(uint32_t addr, uint32_t len, uint32_t *crc)
{
	uint32_t w[16], c = 0;
	if (len & 3) return DBG_READ_ERR;
	while (len) {
		int n = len / 4 < 16 ? (int)(len / 4) : 16;
		int r = dbg_mem_read(addr, w, n);
		if (r) return r;
		uint8_t b[64];
		for (int i = 0; i < n; i++) {
			b[4 * i] = (uint8_t)w[i];
			b[4 * i + 1] = (uint8_t)(w[i] >> 8);
			b[4 * i + 2] = (uint8_t)(w[i] >> 16);
			b[4 * i + 3] = (uint8_t)(w[i] >> 24);
		}
		c = dbg_crc32_update(c, b, 4 * (uint32_t)n);
		addr += 4 * (uint32_t)n;
		len -= 4 * (uint32_t)n;
	}
	*crc = c;
	return DBG_OK;
}
