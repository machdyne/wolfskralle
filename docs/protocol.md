# Wolfskralle USB protocol

**Status: version 6 (phase 5: speed). The exchange is tested
against a simulated device with injected faults; not yet on hardware.**

The definitions are in `common/wkproto.h`, which the firmware and `wolfctl`
share. The device side is `firmware/wkdev.c`, the host side
`host/wkclient.c`.

## 1. Why it looks like this

The Wolfskralle's CH32V003 has no USB peripheral: rv003usb bit-bangs USB
at low speed in an interrupt. Low speed allows only control and interrupt
transfers, so the Wolfskralle is a HID device (no driver on any system)
and talks through **feature reports** (control transfers on endpoint 0).

USB itself detects damaged packets and retries them, but a transfer can
still fail as a whole: a host controller gives up after three errors, a
cable moves, the device is slow to answer. The protocol therefore assumes
that any request or reply can be lost, cut short or (in the tests)
corrupted, and makes every command run exactly once anyway.

## 2. USB device

| | |
|---|---|
| VID:PID | 1209:0001 (the pid.codes test PID, until a PID is assigned) |
| Strings | manufacturer "Machdyne", product "Wolfskralle", serial: 16 hex digits from the chip's unique ID |
| Interface | HID, no boot protocol, vendor usage page 0xFF57 |
| Report | feature report ID 1, 128 bytes including the ID |
| Endpoint 1 IN | interrupt, interval 255 ms, required by HID and always NAKed |
| Report 0xFD | rv003usb's reboot-into-bootloader report (not used by `wolfctl` yet) |

## 3. Frames

Every request and every reply is one feature report of up to 128 bytes.
Since version 6 only the bytes in use travel: the 8-byte header and the
payload (the device takes a shorter SET; a reply is as long as its
payload, so a BUSY answer is 8 bytes, one USB packet instead of sixteen).
The USB is bit-banged in the Wolfskralle, so every packet costs its CPU
time; this matters most while it is busy and the host keeps asking.

| Byte | Field | |
|---|---|---|
| 0 | report ID | 1 |
| 1 | seq | chosen by the host, new for every command; a reply carries its request's seq |
| 2 | cmd | a reply carries its request's cmd |
| 3 | status | 0 in requests; in replies, see below |
| 4 | len | payload length, 0 to 120 |
| 5 | check | 0 in requests; in replies, the low byte of the request's CRC |
| 6, 7 | CRC | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 1-5 and the payload, little endian |
| 8-127 | payload | little-endian fields; unused bytes 0 |

Statuses: 0 ok, 1 busy, 2 no reply (nothing has run since the device
started), 3 bad frame (not sent; reserved), 4 unknown command, 5 bad
argument.

## 4. Exchange

The host:

1. Sends the request (SET_REPORT).
2. Fetches a reply (GET_REPORT) until it has its answer:
   - the GET failed, or the reply's length or CRC is wrong: fetch again;
   - status busy: a command is running (its own, or an older one): wait
     (0.2 ms, doubling up to 20 ms) and fetch again. Each fetch costs the
     Wolfskralle's CPU time (its USB is bit-banged), so a busy device is
     asked rarely;
   - the reply has the request's seq, cmd and check byte: done;
   - anything else (another reply, "no reply"): the device is idle and
     does not have the request, so the host sends the same request again
     (step 1).

The device:

- receives a request in its USB interrupt, which only copies the bytes;
- runs it in the main loop; meanwhile every GET gets a fixed busy frame
  and every SET is dropped (counted as rejected), so neither the request
  nor the reply can be overwritten halfway;
- keeps the reply until the next request has run, so a lost reply can be
  fetched again;
- answers a request identical to the last one that ran (seq, cmd and
  CRC) from the kept reply, without running it again: if the host sent a
  request twice because the first answer got lost, it still runs once;
- drops a request that fails its CRC (counted as bad) and keeps its
  reply; the host then sees that its request did not arrive and sends it
  again.

A request that arrives while the device is busy is not queued: there is
room for one request in 2 KB of RAM, and the host never needs more,
because it sends the next request only after it has the last reply.

The commands that change something (power, the LED; later flash writes)
are safe to send again in any case, but the protocol does not rely on it.

## 5. Commands

| cmd | Name | Request payload | Reply payload |
|---|---|---|---|
| 0x01 | INFO | none | 44 bytes, below |
| 0x02 | PING | delay_ms u16 (max 5000), then up to 118 bytes | the bytes, after the delay |
| 0x03 | POWER | op u8 (0 off, 1 on, 2 query), monitor_ms u16 (max 200, optional) | 9 bytes, below |
| 0x04 | LED | mode u8 (0 off, 1 on, 2 auto) | none |
| 0x10 | SWIO | on u8 (needs target power) | on u8 |
| 0x11 | SWIO_TIMING | none (read), or the 8 timing bytes (set) | the 8 timing bytes |
| 0x12 | DMI | up to 20 ops of 6 bytes: op u8 (0 write, 1 read), register u8, value u32 | ops done u8, failed u8 (1: a packet came back incomplete), then a u32 for each read done |
| 0x13 | SWIO_TEST | count u32 (max 200,000) | 37 bytes, below |
| 0x14 | SWIO_RAW | op u8, register u8, value u32 | complete u8, value u32, captures n u8, n captures (u8 ticks) |
| 0x20 | CONNECT | mode u8 (0 auto, 1 attach, 2 restart) | 42 bytes, below |
| 0x21 | RESUME | none | result u8 |
| 0x22 | MEM_READ | address u32 (4-aligned), words u8 (1-29) | result u8, then the words (u32) if it is 0 |
| 0x23 | CRC32 | address u32, bytes u32 (a multiple of 4, max 256 KB) | result u8, CRC-32 u32 (as zlib) |
| 0x30 | FLASH_BEGIN | none | result u8, page bytes u16, flash bytes u32 |
| 0x31 | PAGE_LOAD | offset u16, up to 112 bytes (offset + bytes <= 256) | result u8 |
| 0x32 | PAGE_WRITE | address u32, CRC-32 u32 of the page's bytes, flags u8 (1: the page is blank, all 0xFF: the buffer is not used), then optionally offset u16 and up to 109 bytes for the page buffer (version 5) | result u8, written u8 (0: the page already held this), attempts u8 |
| 0x33 | FLASH_END | none | result u8 |
| 0x34 | VERIFY_PAGES | address u32 of the first page, count u8 (1-28), CRC-32 u32 of each page (version 5) | result u8, differs u32 (bit i: page i does not match) |
| 0x40 | PROFILE | reset u8 (1: zero the counters after reading) | 14 counters (u32), below (version 6) |
| 0x50 | I2C | address u8 (7-bit), rlen u8 (0-112), then the bytes to write | result u8, then rlen bytes if it is 0 (version 7) |
| 0x51 | I2C_SCAN | none | result u8, 16 bytes: bit a set if address a (0x08-0x77) acknowledged (version 7) |

Version 7: the Sechs bus, Zwölf pins 1 (SCL) and 2 (SDA) (docs/sechs.md).
I2C writes the bytes, then reads rlen bytes after a repeated start;
neither: an address-only probe. Results: 0 ok, 1 no answer at the
address, 2 a written byte not acknowledged, 3 the bus is stuck, 4 the
clock was held too long. Both need target power (status 6 otherwise) and
switch SWIO off.

Version 6: PAGE_WRITE flag 2: the page is known to differ (VERIFY_PAGES
said so), so the Wolfskralle does not read it before erasing; it still
verifies it afterwards. PROFILE counts, since the last reset: time
running commands (us), commands, GETs answered, of them BUSY, SWIO
packets, of them incomplete, then time (us) reading pages before writing,
erasing, programming, reading pages back, computing CRCs, then flash
controller busy polls, pages erased or written, fast writes given up.

Version 4: programming (docs/programming.md). FLASH_BEGIN unlocks the
flash of a chip that CONNECT stopped and identified as a CH32V003 or
CH32V005, if it is not read-protected; it reports the page and flash size
the Wolfskralle's write gate uses. PAGE_LOAD fills the Wolfskralle's
256-byte page buffer; PAGE_WRITE checks the buffer against the CRC-32
sent, then makes the page hold it (leaving it alone if it already does;
otherwise erase, program, verify, up to three times). FLASH_END and RESUME
lock the flash. New results: 5 not a supported chip, 6 read-protected,
7 the flash would not unlock, 8 FLASH_BEGIN first, 9 refused by the write
gate, 10 did not verify, 11 the page buffer's CRC differs.

Version 3: the debug commands (docs/debug.md). Their result byte: 0 ok,
1 no chip answers, 2 the core would not stop, 3 a memory read failed,
4 not connected (CONNECT first). They need target power (status 6
otherwise) and switch SWIO on themselves. MEM_READ and CRC32 work only on
a chip stopped by CONNECT; RESUME, POWER off and a restart end that.

CONNECT reply:

| Offset | Size | |
|---|---|---|
| 0 | 1 | result |
| 1 | 1 | how: 0 attached, 1 power-cycled, 2 reset (board v1) |
| 2 | 4 | configuration register (0x5AA5xxxx from a chip) |
| 6 | 4 | DMSTATUS |
| 10 | 4 | HARTINFO |
| 14 | 4 | chip ID (debug register 0x7F) |
| 18 | 4 | the word at 0x1FFFF7C4 |
| 22 | 4 | FLASH_OBR (bit 1: read-protected) |
| 26 | 4 | the word at 0x1FFFF7E0 (low 16 bits: flash KB) |
| 30 | 12 | the unique ID at 0x1FFFF7E8 |
| 42 | 1 | program buffer words, from ABSTRACTCS (version 5) |
| 43 | 1 | fast paths in use: 1 reads, 2 writes (version 5) |

Version 5: CONNECT's mode may add 0x80: no fast paths (`--slow`).

New statuses in version 2: 6 no power (SWIO needs target power on),
7 SWIO error (reserved). DMI, SWIO_TEST and SWIO_RAW switch SWIO on when
needed; POWER off switches it off. The timing bytes are, in ticks of
20.8 ns: period, one, one-pp, zero, read, thresh, then mode (0/1) and stop
(us); docs/swio.md explains them. SWIO_TIMING refuses (bad argument) a
timing that cannot work: needs one, one-pp, read < zero <= period - 2,
read < thresh < period, 20 <= period <= 250, mode 0 or 1, 1 <= stop <= 200.

SWIO_TEST reply:

| Offset | Size | |
|---|---|---|
| 0 | 1 | 0 ok, 1 no chip answered |
| 1 | 4 | what the configuration register read |
| 5 | 4 | round trips done |
| 9 | 4 | round trips that read back wrong |
| 13 | 4 | packets that came back incomplete |
| 17 | 4 | the first wrong round trip: the value written |
| 21 | 4 | ... and the value read |
| 25 | 12 | min, max (u8 ticks) of the measured low times of: written 1s, written 0s, 1s and 0s in read headers, read slots that should be 1, and 0 |

INFO reply:

| Offset | Size | |
|---|---|---|
| 0 | 1 | protocol version (1) |
| 1 | 1 | board (0 v0, 1 v1) |
| 2 | 2 | firmware version, major << 8 \| minor |
| 4 | 4 | uptime in ms |
| 8 | 2 | supply voltage in mV |
| 10 | 1 | target power (0/1) |
| 11 | 1 | LED mode |
| 12 | 4 | commands run |
| 16 | 4 | repeated requests answered from the kept reply |
| 20 | 4 | requests dropped for a bad CRC or length |
| 24 | 4 | requests rejected because a command was running |
| 28 | 16 | "Wolfskralle", NUL padded |
| 44 | 1 | SWIO on (version 2) |
| 45 | 1 | why the Wolfskralle last started: 0 power-on, 1 reset pin or debugger, 2 software, 3 watchdog (the firmware hung), 4 other (firmware 0.5) |

POWER reply:

| Offset | Size | |
|---|---|---|
| 0 | 1 | target power after the operation |
| 1 | 2 | supply mV before switching |
| 3 | 2 | lowest supply mV while monitoring |
| 5 | 2 | average supply mV while monitoring |
| 7 | 2 | samples taken while monitoring (about 3 us each) |

The supply voltage is measured against the CH32V003's internal 1.2 V
reference, so it is only as accurate as that reference (a few percent).
It is meant for finding dips, such as the one when target power switches
on (docs/plan.md, H3), not for calibration.

## 6. Tests

`make -C host test` runs `host/test/proto_test`: the real `wkdev.c` in one
thread as the device's main loop, the real `wkclient.c` in another, and a
simulated USB between them that moves 8-byte packets and injects faults:
a lost SETUP, a SET aborted halfway, a corrupted SET, a SET whose status
stage is lost (the device has it, the host sees an error), a SET sent
twice, a lost GET, a truncated GET and a corrupted GET. Commands take
random times, so GETs often find the device busy.

It checks that every command completes with the right reply, that every
request runs exactly once, that every corruption is noticed (as many bad
requests and bad replies as were injected), and that the fixed busy and
no-reply frames are valid. It runs 5,000 commands without faults, 20,000
with 5% and 5,000 with 30%.

The test was checked by breaking the device on purpose: without the
repeat check, requests run twice; with replies allowed while a request
waits to run, the host sees torn and stale replies. Both make it fail.
