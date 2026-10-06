/*
 * ch32dbg: the target's debug module, above the link (SWIO): connect,
 * stop the core, read what the chip is, read its memory, restart it.
 * See docs/debug.md.
 *
 * Hardware-independent: the firmware provides the hooks below (swio.c,
 * target power, the reset line on board v1); the host tests provide a
 * simulated chip (host/test/dbg_test.c).
 *
 * Memory writes go through one function, mem_write, behind the write
 * gate (docs/programming.md): the identified chip's main flash and the
 * five flash controller registers that programming needs; never OBKEYR,
 * the option bytes or anything else. Before a supported chip is
 * identified, nothing at all.
 */

#ifndef CH32DBG_H
#define CH32DBG_H

#include <stdint.h>

/* ---- hooks, provided by the caller ---- */
int dbg_dmi_write(uint8_t reg, uint32_t val);   /* 0, or -1 */
int dbg_dmi_read(uint8_t reg, uint32_t *val);   /* 0, or -1 if incomplete */
void dbg_delay_us(uint32_t us);
void dbg_target_power(int on);                  /* also stops/starts the link */
int dbg_have_reset(void);                       /* board v1: TRESET wired */
void dbg_reset_line(int hold);                  /* board v1 */

/* ---- debug module registers ---- */
#define DM_DATA0        0x04
#define DM_DMCONTROL    0x10
#define DM_DMSTATUS     0x11
#define DM_HARTINFO     0x12
#define DM_ABSTRACTCS   0x16
#define DM_COMMAND      0x17
#define DM_AUTOEXEC     0x18
#define DM_PROGBUF0     0x20
#define DM_CFGR         0x7D
#define DM_SHDWCFGR     0x7E
#define DM_CHIPID       0x7F

#define DM_CFG_KEY      0x5AA50400u     /* key, and "enable the chip's output" */

/* addresses read by dbg_connect */
#define CH32_CHIPID_ADDR    0x1FFFF7C4u /* CH32V003: 0x003xxxxx */
#define CH32_FLACAP_ADDR    0x1FFFF7E0u /* flash size in KB (low 16 bits) */
#define CH32_UID_ADDR       0x1FFFF7E8u /* 96-bit unique ID */
#define CH32_FLASH_OBR      0x4002201Cu /* bit 1: read protection */

/* how dbg_connect reached the chip */
#define DBG_HOW_ATTACH  0       /* talked to the running chip */
#define DBG_HOW_POWER   1       /* power-cycled it and caught it early */
#define DBG_HOW_RESET   2       /* held it in reset (board v1) */

/* modes for dbg_connect */
#define DBG_MODE_AUTO   0       /* attach; if nothing answers, restart it */
#define DBG_MODE_ATTACH 1       /* attach only */
#define DBG_MODE_RESTART 2      /* restart first (v0: power cycle; v1: reset) */

/* results */
#define DBG_OK          0
#define DBG_NO_CHIP     1       /* nothing answers */
#define DBG_NO_HALT     2       /* answers, but the core would not stop */
#define DBG_READ_ERR    3       /* a memory read failed */
#define DBG_NOT_HALTED  4       /* needs a connected, halted chip */
#define DBG_UNSUPPORTED 5       /* not a CH32V003/CH32V005: the gate stays shut */
#define DBG_PROTECTED   6       /* read-protected: not programmed */
#define DBG_LOCKED      7       /* the flash would not unlock */
#define DBG_NOT_STARTED 8       /* dbg_flash_begin first */
#define DBG_FORBIDDEN   9       /* outside the write gate: refused */
#define DBG_VERIFY      10      /* a page did not verify, five times */

/* flash controller (CH32V003/V005) */
#define CH32_FLASH          0x08000000u
#define CH32_PAGE_MAX       256u
#define FLASH_KEYR          0x40022004u
#define FLASH_OBKEYR        0x40022008u     /* never written */
#define FLASH_STATR         0x4002200Cu
#define FLASH_CTLR          0x40022010u
#define FLASH_ADDR          0x40022014u
#define FLASH_MODEKEYR      0x40022024u
#define DBG_FLASH_KEY1          0x45670123u
#define DBG_FLASH_KEY2          0xCDEF89ABu
#define CTLR_LOCK           0x00000080u
#define CTLR_FLOCK          0x00008000u
#define CTLR_STRT           0x00000040u
#define CTLR_PAGE_PG        0x00010000u
#define CTLR_PAGE_ER        0x00020000u
#define CTLR_BUF_LOAD       0x00040000u
#define CTLR_BUF_RST        0x00080000u
#define STATR_BSY           0x00000001u
#define STATR_WRPRTERR      0x00000010u

/* the write gate: the identified chip (0: none, gate shut) */
extern uint32_t dbg_flash_size, dbg_page;
extern int dbg_chip;            /* 3: CH32V003, 5: CH32V005, 0: none */
extern int dbg_unlocked;        /* dbg_flash_begin done */
extern uint32_t dbg_refused;    /* writes the gate refused (a bug if not 0) */

struct dbg_info {
	uint8_t how;            /* DBG_HOW_* */
	uint8_t progbufsize;    /* program buffer words (ABSTRACTCS) */
	uint8_t fast;           /* DBG_FAST_* in use */
	uint32_t cfgr;          /* what the configuration register read */
	uint32_t dmstatus, hartinfo, chipid_dm;
	uint32_t chipid_mem, obr, flacap;
	uint32_t uid[3];
};

extern int dbg_halted;      /* connected and stopped */

/* the fast paths (docs/programming.md, speed): a two-word program in the
 * program buffer, repeated by auto-execution. Reads are checked against
 * the plain method when connecting; writes are dropped for the rest of
 * the session if a page written that way does not verify. */
#define DBG_FAST_READ   1
#define DBG_FAST_WRITE  2
extern int dbg_fast;        /* DBG_FAST_* in use */
extern int dbg_fast_allowed; /* DBG_FAST_* the caller allows (default both) */

/* Connects, enables the debug output, stops the core at its reset vector
 * and reads the identity. Writes nothing to memory. */
int dbg_connect(int mode, struct dbg_info *info);

/* Restarts the target's program (core reset, run). */
int dbg_resume(void);

/* n words from addr (4-aligned) of a halted chip */
int dbg_mem_read(uint32_t addr, uint32_t *w, int n);

/* CRC-32 (as zlib) of len bytes (a multiple of 4) from addr */
int dbg_crc32(uint32_t addr, uint32_t len, uint32_t *crc);

uint32_t dbg_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);

/* Unlocks the flash of a halted, identified, unprotected CH32V003 or
 * CH32V005. */
int dbg_flash_begin(void);

/* Makes the page at addr hold w (dbg_page bytes): unchanged if it already
 * does; otherwise erase, program (unless w is all 0xFF) and verify, up to
 * five times. *written: 1 if it was erased or written; *attempts. */
int dbg_page_write(uint32_t addr, const uint32_t *w, int differs, int *written,
                   int *attempts);
/* (differs: the caller already knows the page differs, from VERIFY_PAGES:
 * it is not read first. It is still verified after writing.) */

/* where the time goes (wolfctl -v flash): microseconds and counts */
struct dbg_prof {
	uint32_t precheck_us;   /* reading a page before writing it */
	uint32_t erase_us;
	uint32_t program_us;
	uint32_t verify_us;     /* reading a page back after writing it */
	uint32_t read_us;       /* CRCs (VERIFY_PAGES, CRC32) */
	uint32_t waits;         /* reads of FLASH_STATR while it was busy */
	uint32_t pages;         /* pages erased or written */
	uint32_t fallbacks;     /* fast writes given up */
};
extern struct dbg_prof dbg_prof;
uint32_t dbg_us(void);      /* hook: a free-running microsecond clock */

/* Locks the flash again. */
int dbg_flash_end(void);

#endif
