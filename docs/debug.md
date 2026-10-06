# Debug access: connect, stop, identify, read

**Status: phase 3. Works on hardware (`wolfctl id` on a CH32V003);
tested against a simulated CH32V003 and CH32V005.**

Above the link (docs/swio.md), `firmware/ch32dbg.c` talks to the target's
RISC-V debug module. It is hardware-independent: the firmware gives it
SWIO, target power and (on board v1) the reset line; the host tests give
it a simulated chip.

Connecting, identifying and reading write nothing to the target's memory:
only debug module registers and, through abstract commands, the core
registers x8 and x9 of a stopped core. The tests count every memory write
the simulated chip sees, and require none for these. Programming (phase 4,
docs/programming.md) writes memory only through the write gate.

## 1. Connecting

| Mode | wolfctl | Does |
|---|---|---|
| auto (default) | | attach; if nothing answers, restart the target and catch it |
| attach | `--attach` | talk to the running chip only |
| restart | `--restart` | restart first, then catch it |

**Attach**: enable the chip's debug output (the key 0x5AA50400 in its
configuration registers 0x7E and 0x7D), request a halt, and read the
configuration register back: only a chip returns the key. A line that
nobody pulls low reads as `ffffffff`.

**Restart on board v0**: TRESET is not connected (docs/hardware.md), so
the Wolfskralle switches target power off for 100 ms (the 10 uF on TPWR
discharge through the blue LED's 1K5) and on again, then knocks (key and
halt request) every 250 us until the chip answers, for up to about 0.1 s.
This catches a chip within the first moments of its program. SWIO is off
while the power is.

**Restart on board v1**: hold RESETN low for 1 ms, release it, knock.

Then, as on Werkzeug: halt, reset the core with the halt request held (so
it waits at its reset vector, before its program runs again), wait until
`allhalted`, clear the request and any abstract command error.

Factory-new chips and modules running Machdyne BASIC answer to attach.
Restart is for a chip whose program turns SWIO off or puts the chip to
sleep: on v0 it works as long as the program does not do that within the
first moments after power-up.

## 2. Identifying

| Read | From | |
|---|---|---|
| chip ID | debug register 0x7F | 0x005xxxxx: CH32V005; 0x003x05xx or 0: CH32V003 (checked further) |
| ID word | 0x1FFFF7C4 | CH32V003: 0x003xxxxx |
| hartinfo | debug register 0x12 | CH32V003: low bits 0x0f4 |
| flash size | 0x1FFFF7E0, low 16 bits | KB |
| read protection | FLASH_OBR (0x4002201C), bit 1 | |
| unique ID | 0x1FFFF7E8, 96 bits | |

The rules for the CH32V003 and CH32V005 are the ones machdyne/basic
proved (CH32V003 on hardware). `wolfctl` names the chip from them; the
device only reports what it read. Other CH32V00x chips are named but are
not Zwölf module chips: phase 4 will refuse to program them.

## 3. Reading

Memory is read a word at a time through the program buffer (`c.lw x8,
0(x8)`): DATA0 = address, copy it to x8 and run the buffer, copy x8 back,
read DATA0, with every abstract command checked for errors. That is six
SWIO packets per word, about 0.4 ms: a 16 KB flash takes about 1.6 s.
Phase 5 will make this faster.

Since firmware 0.7 (phase 5), reads use auto-execution where the chip
allows it: one packet a word (docs/programming.md, speed).

A read either returns the right word or fails: a damaged packet is
reported, never taken as data (the tests check this with 2% of the
packets damaged). On top of that, `wolfctl read` reads everything twice:
once word by word to the computer, and once more on the Wolfskralle,
which returns only the CRC-32. The two must agree.

## 4. Restarting

`resume`: reset the core without the halt request, acknowledge the reset,
request resume (on v1 also a short pulse on RESETN). The chip runs its
program from the start.

`id`, `read` and `crc` restart the chip when they are done, unless given
`--halt`; `wolfctl resume` restarts a chip left stopped.

## 5. Commands

| wolfctl | Protocol |
|---|---|
| `id` | CONNECT, RESUME |
| `read [ADDR [LEN]] [-o FILE]` | CONNECT, MEM_READ (29 words each), CRC32, RESUME |
| `crc [ADDR [LEN]]` | CONNECT, CRC32, RESUME |
| `resume` | RESUME |

Without ADDR and LEN, `read` and `crc` cover the chip's whole flash
(0x08000000, its size as the chip reports it).
