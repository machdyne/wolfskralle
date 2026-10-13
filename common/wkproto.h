/*
 * Wolfskralle USB protocol: shared by the firmware and the host.
 * See docs/protocol.md.
 *
 * Every request and every reply is one HID feature report of
 * WK_REPORT_LEN bytes (the report ID included):
 *
 *   [0]    report ID (WK_REPORT_ID)
 *   [1]    seq      chosen by the host; a reply carries its request's seq
 *   [2]    cmd      WK_CMD_*; a reply carries its request's cmd
 *   [3]    status   0 in requests; WK_ST_* in replies
 *   [4]    len      payload bytes (0..WK_PAYLOAD_MAX)
 *   [5]    check    0 in requests; in replies, the low byte of the
 *                   request's CRC (ties a reply to one exact request)
 *   [6..7] crc      CRC-16/CCITT-FALSE over bytes 1..5 and the payload,
 *                   little endian
 *   [8..]  payload  little-endian fields
 */

#ifndef WKPROTO_H
#define WKPROTO_H

#include <stdint.h>

#define WK_USB_VID          0x1209
#define WK_USB_PID          0x0001      /* pid.codes test PID: see docs */
#define WK_PROTO_VERSION    7

#define WK_REPORT_ID        0x01        /* 0xFD belongs to rv003usb (reboot) */
#define WK_REPORT_LEN       128
#define WK_HDR_LEN          8
#define WK_PAYLOAD_MAX      (WK_REPORT_LEN - WK_HDR_LEN)

/* commands */
#define WK_CMD_INFO         0x01
#define WK_CMD_PING         0x02
#define WK_CMD_POWER        0x03
#define WK_CMD_LED          0x04
#define WK_CMD_SWIO         0x10    /* SWIO on/off */
#define WK_CMD_SWIO_TIMING  0x11    /* get or set the SWIO timing */
#define WK_CMD_DMI          0x12    /* debug register reads and writes */
#define WK_CMD_SWIO_TEST    0x13    /* the link alone: write and read back */
#define WK_CMD_SWIO_RAW     0x14    /* one packet, with the measured pulses */
#define WK_CMD_CONNECT      0x20    /* connect, stop the core, identify */
#define WK_CMD_RESUME       0x21    /* restart the target's program */
#define WK_CMD_MEM_READ     0x22    /* read words of a stopped target */
#define WK_CMD_CRC32        0x23    /* CRC-32 of a range, read on the device */
#define WK_CMD_FLASH_BEGIN  0x30    /* unlock the flash of the identified chip */
#define WK_CMD_PAGE_LOAD    0x31    /* data into the device's page buffer */
#define WK_CMD_PAGE_WRITE   0x32    /* make a flash page hold the buffer */
#define WK_CMD_FLASH_END    0x33    /* lock the flash again */
#define WK_CMD_VERIFY_PAGES 0x34    /* which pages differ from their CRCs */
#define WK_CMD_PROFILE      0x40    /* where the time went */
#define WK_CMD_I2C          0x50    /* one I2C transaction on the Sechs bus */
#define WK_CMD_I2C_SCAN     0x51    /* which addresses answer on the Sechs bus */

/* status in replies */
#define WK_ST_OK            0x00
#define WK_ST_BUSY          0x01    /* still executing: ask again */
#define WK_ST_NO_REPLY      0x02    /* nothing executed since start */
#define WK_ST_BAD_FRAME     0x03    /* the request failed its CRC or length */
#define WK_ST_BAD_CMD       0x04    /* unknown command */
#define WK_ST_BAD_ARG       0x05    /* payload wrong for the command */
#define WK_ST_NO_POWER      0x06    /* SWIO needs target power on */
#define WK_ST_SWIO_ERR      0x07    /* a SWIO packet came back incomplete */

/* boards */
#define WK_BOARD_V0         0
#define WK_BOARD_V1         1

/* POWER: request payload [op u8][monitor_ms u16] */
#define WK_POWER_OFF        0
#define WK_POWER_ON         1
#define WK_POWER_QUERY      2
#define WK_POWER_MON_MAX_MS 200

/* LED: request payload [mode u8] */
#define WK_LED_OFF          0
#define WK_LED_ON           1
#define WK_LED_AUTO         2

/* PING: request payload [delay_ms u16][data...]; the reply echoes data */
#define WK_PING_DELAY_MAX   5000

/* INFO reply payload (byte offsets) */
#define WK_INFO_PROTO       0   /* u8  WK_PROTO_VERSION */
#define WK_INFO_BOARD       1   /* u8  WK_BOARD_* */
#define WK_INFO_FW          2   /* u16 firmware version, major<<8 | minor */
#define WK_INFO_UPTIME      4   /* u32 ms since start */
#define WK_INFO_VDD         8   /* u16 mV, measured against the internal reference */
#define WK_INFO_TPOWER      10  /* u8  target power 0/1 */
#define WK_INFO_LED         11  /* u8  WK_LED_* */
#define WK_INFO_CMDS        12  /* u32 commands executed */
#define WK_INFO_DUPS        16  /* u32 repeated requests answered from the kept reply */
#define WK_INFO_BAD         20  /* u32 requests that failed their CRC or length */
#define WK_INFO_REJECTED    24  /* u32 requests that arrived while busy (dropped) */
#define WK_INFO_NAME        28  /* char[16] "Wolfskralle", NUL padded */
#define WK_INFO_SWIO        44  /* u8  SWIO on (1) or off (0) */
#define WK_INFO_RESET       45  /* u8  why the Wolfskralle last started: WK_RESET_* */
#define WK_INFO_LEN         46

#define WK_RESET_POWER      0   /* power-on */
#define WK_RESET_PIN        1   /* its reset pin (or a debugger) */
#define WK_RESET_SOFTWARE   2   /* software (e.g. after flashing) */
#define WK_RESET_WATCHDOG   3   /* the watchdog: the firmware hung */
#define WK_RESET_OTHER      4

/* POWER reply payload (byte offsets) */
#define WK_PWR_STATE        0   /* u8  0/1 after the operation */
#define WK_PWR_VDD_BEFORE   1   /* u16 mV before switching */
#define WK_PWR_VDD_MIN      3   /* u16 lowest mV while monitoring */
#define WK_PWR_VDD_AVG      5   /* u16 average mV while monitoring */
#define WK_PWR_SAMPLES      7   /* u16 samples taken while monitoring */
#define WK_PWR_LEN          9

/* SWIO timing, in timer ticks of 1/48 us (20.8 ns); payload of
 * SWIO_TIMING (8 bytes; empty request: just read it) */
#define WK_TM_PERIOD        0   /* one bit, in ticks */
#define WK_TM_ONE_OD        1   /* low time of a 1, open drain */
#define WK_TM_ONE_PP        2   /* low time of a 1, push-pull (mode 1 writes) */
#define WK_TM_ZERO          3   /* low time of a 0 */
#define WK_TM_READ          4   /* low time that starts a read bit */
#define WK_TM_THRESH        5   /* a read bit whose line rose before this is a 1 */
#define WK_TM_MODE          6   /* 0: open drain; 1: write packets push-pull */
#define WK_TM_STOP_US       7   /* line high after each packet, us */
#define WK_TM_LEN           8

/* SWIO: request [on u8]; reply [on u8] */

/* DMI: request: up to WK_DMI_MAX ops of 6 bytes [op u8][reg u8][value u32];
 * reply: [ops done u8][failed u8: 0, or 1 if a read came back incomplete]
 * then a u32 for each read done, in order */
#define WK_DMI_WRITE        0
#define WK_DMI_READ         1
#define WK_DMI_MAX          20

/* SWIO_TEST: request [count u32] (max WK_SWIO_TEST_MAX); reply below.
 * The pulse statistics are what the Wolfskralle measured on the line: for
 * each kind of bit, the shortest and longest time from the falling edge
 * to the line being high again, in ticks. */
#define WK_SWIO_TEST_MAX    200000
#define WK_LT_RESULT        0   /* u8  0 ok, 1 no chip answered */
#define WK_LT_CFGR          1   /* u32 what the configuration register read */
#define WK_LT_DONE          5   /* u32 round trips done */
#define WK_LT_ERRORS        9   /* u32 round trips that read back wrong */
#define WK_LT_INCOMPLETE    13  /* u32 packets that came back incomplete */
#define WK_LT_BAD_WANT      17  /* u32 the first wrong round trip: written */
#define WK_LT_BAD_GOT       21  /* u32 ... and read */
#define WK_LT_W1            25  /* u8 min, max: 1s in write packets */
#define WK_LT_W0            27  /* u8 min, max: 0s in write packets */
#define WK_LT_H1            29  /* u8 min, max: 1s in read packet headers */
#define WK_LT_H0            31  /* u8 min, max: 0s in read packet headers */
#define WK_LT_R1            33  /* u8 min, max: read bits that should be 1 */
#define WK_LT_R0            35  /* u8 min, max: read bits that should be 0 */
#define WK_LT_LEN           37

/* SWIO_RAW: request [op u8][reg u8][value u32];
 * reply [ok u8][value u32][captures n u8][n captures, u8 ticks] */
#define WK_SWIO_BITS        41  /* bits in every packet */

/* Results inside the replies of CONNECT, RESUME, MEM_READ and CRC32
 * (the frame status is OK when the command ran) */
#define WK_DBG_OK           0
#define WK_DBG_NO_CHIP      1   /* nothing answers on SWIO */
#define WK_DBG_NO_HALT      2   /* it answers, but the core would not stop */
#define WK_DBG_READ_ERR     3   /* a memory read failed */
#define WK_DBG_NOT_HALTED   4   /* connect first */
#define WK_DBG_UNSUPPORTED  5   /* not a CH32V003/CH32V005: never written */
#define WK_DBG_PROTECTED    6   /* read-protected: never written */
#define WK_DBG_LOCKED       7   /* the flash would not unlock */
#define WK_DBG_NOT_STARTED  8   /* FLASH_BEGIN first */
#define WK_DBG_FORBIDDEN    9   /* outside the write gate: refused */
#define WK_DBG_VERIFY       10  /* a page did not verify, three times */
#define WK_DBG_BAD_BUFFER   11  /* the page buffer's CRC is not the one sent */

/* CONNECT: request [mode u8]: 0 auto (attach; if nothing answers, restart
 * the target), 1 attach only, 2 restart first (board v0: power cycle;
 * v1: reset); plus WK_CN_MODE_SLOW: no fast paths. Reply: */
#define WK_CN_MODE_SLOW     0x80
#define WK_CN_RESULT        0   /* u8  WK_DBG_* */
#define WK_CN_HOW           1   /* u8  0 attached, 1 power-cycled, 2 reset */
#define WK_CN_CFGR          2   /* u32 the configuration register */
#define WK_CN_DMSTATUS      6   /* u32 */
#define WK_CN_HARTINFO      10  /* u32 */
#define WK_CN_CHIPID_DM     14  /* u32 debug register 0x7F */
#define WK_CN_CHIPID_MEM    18  /* u32 the word at 0x1FFFF7C4 */
#define WK_CN_OBR           22  /* u32 FLASH_OBR (bit 1: read-protected) */
#define WK_CN_FLACAP        26  /* u32 the word at 0x1FFFF7E0 (low 16 bits: flash KB) */
#define WK_CN_UID           30  /* u32 x3 the unique ID at 0x1FFFF7E8 */
#define WK_CN_PROGBUF       42  /* u8  program buffer words (ABSTRACTCS) */
#define WK_CN_FAST          43  /* u8  fast paths in use: 1 reads, 2 writes */
#define WK_CN_LEN           44

/* RESUME: reply [result u8] */
/* MEM_READ: request [addr u32][words u8, 1..WK_MEM_READ_MAX];
 * reply [result u8][words u32...] */
#define WK_MEM_READ_MAX     29
/* CRC32: request [addr u32][bytes u32, a multiple of 4, max WK_CRC_MAX];
 * reply [result u8][crc u32] (as zlib's crc32) */
#define WK_CRC_MAX          (256u * 1024u)

/* FLASH_BEGIN: reply [result u8][page bytes u16][flash bytes u32]
 * PAGE_LOAD: request [offset u16][data...] (offset + data <= WK_PAGE_MAX);
 *   reply [result u8]
 * PAGE_WRITE: request [addr u32][crc u32 of the page's bytes][flags u8]
 *   [offset u16][data...] (the last two optional: data into the page
 *   buffer first, as PAGE_LOAD); flag 1: the page is blank (all 0xFF):
 *   the buffer is not used.
 *   reply [result u8][written u8: 0 unchanged, 1 erased/written][attempts u8]
 * FLASH_END: reply [result u8] */
#define WK_PAGE_MAX         256
#define WK_PAGE_LOAD_MAX    112
#define WK_PW_BLANK         1
#define WK_PW_DIFFERS       2   /* known to differ (VERIFY_PAGES): not read first */
#define WK_PW_DATA_MAX      (WK_PAYLOAD_MAX - 11)   /* 109 */

/* VERIFY_PAGES: request [addr u32 of the first page][count u8, 1..28]
 *   [crc u32 of each page]; reply [result u8][differs u32: bit i set if
 *   page i does not match its CRC] */
#define WK_VERIFY_PAGES_MAX 28

/* PROFILE: request [reset u8] (1: zero the counters after reading);
 * reply: u32 counters at these offsets */
#define WK_PF_HANDLER_US    0   /* time running commands */
#define WK_PF_COMMANDS      4
#define WK_PF_GETS          8   /* GETs answered */
#define WK_PF_BUSY_GETS     12  /* ... of them BUSY */
#define WK_PF_SWIO_PACKETS  16
#define WK_PF_SWIO_BAD      20  /* packets that came back incomplete */
#define WK_PF_PRECHECK_US   24
#define WK_PF_ERASE_US      28
#define WK_PF_PROGRAM_US    32
#define WK_PF_VERIFY_US     36
#define WK_PF_READ_US       40  /* CRCs */
#define WK_PF_WAITS         44  /* flash controller busy polls */
#define WK_PF_PAGES         48  /* pages erased/written */
#define WK_PF_FALLBACKS     52  /* fast writes given up */
#define WK_PF_LEN           56

/* The Sechs bus: Zwölf pins 1 (SCL) and 2 (SDA), bit-banged at about
 * 40 kHz (docs/sechs.md). Needs target power (the pull-ups are on TPWR);
 * switches SWIO off (on an LS11, pin 1 is also SWIO).
 * I2C: request [addr u8, 7-bit][rlen u8, 0..WK_I2C_MAX][wdata...];
 *   write wdata, then with a repeated start read rlen bytes; neither: an
 *   address-only probe. Reply [result u8][rdata...] (rdata only if ok).
 * I2C_SCAN: request none; reply [result u8][16 bytes: bit a set if address
 *   a (0x08-0x77) acknowledged] */
#define WK_I2C_MAX          112
#define WK_I2C_OK           0
#define WK_I2C_NAK_ADDR     1   /* nobody answered at that address */
#define WK_I2C_NAK_DATA     2   /* a written byte was not acknowledged */
#define WK_I2C_STUCK        3   /* a line stays low: no free bus */
#define WK_I2C_STRETCH      4   /* SCL held low for too long */

static inline uint16_t wk_crc16(uint16_t crc, const uint8_t *p, unsigned n)
{
	while (n--) {
		crc ^= (uint16_t)(*p++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
			                     : (uint16_t)(crc << 1);
	}
	return crc;
}

/* the CRC of a frame: header bytes 1..5, then the payload */
static inline uint16_t wk_frame_crc(const uint8_t *f)
{
	unsigned len = f[4] <= WK_PAYLOAD_MAX ? f[4] : WK_PAYLOAD_MAX;
	uint16_t crc = wk_crc16(0xffff, f + 1, 5);
	return wk_crc16(crc, f + WK_HDR_LEN, len);
}

static inline void wk_frame_seal(uint8_t *f)
{
	uint16_t crc = wk_frame_crc(f);
	f[6] = crc & 0xff;
	f[7] = crc >> 8;
}

/* 1 if the frame's report ID, length field and CRC are right */
static inline int wk_frame_ok(const uint8_t *f)
{
	if (f[0] != WK_REPORT_ID || f[4] > WK_PAYLOAD_MAX) return 0;
	uint16_t crc = wk_frame_crc(f);
	return f[6] == (crc & 0xff) && f[7] == (crc >> 8);
}

static inline void wk_put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wk_put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t wk_get16(const uint8_t *p)
{ return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t wk_get32(const uint8_t *p)
{ return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#endif
