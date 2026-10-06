# Programming a module

**Status: phase 4. Works on hardware: a CH32V003 (LS10A), 16 KB, written
and verified in 23.3 s. Tested end to end against a simulated CH32V003
and CH32V005 (including power cuts and damaged packets); the CH32V005 not
yet on hardware.**

```
$ wolfctl flash basic-ls10a.bin          # Machdyne BASIC for an LS10A
$ wolfctl flash my-firmware.bin --force  # anything else
$ wolfctl verify basic-ls10a.bin         # does the module hold it?
```

`flash` programs the module in the spring contacts, verifies it and
restarts it. It writes the whole main flash: the image, then 0xFF to the
end, so nothing of an older firmware is left behind.

## 1. Failsafe rules

The rules of machdyne/basic's Werkzeug programmer (its docs/ch32prog.md),
as they apply to the Wolfskralle. They hold in every version, and the
tests check them (section 4).

1. **Main flash only.** Only the identified chip's main flash is erased
   and written: 0x08000000-0x08003FFF (16 KB) on a CH32V003,
   0x08000000-0x08007FFF (32 KB) on a CH32V005; before the chip is
   identified, nothing. Never the option bytes, the system area, or any
   other address. The **Wolfskralle itself enforces this** (its write
   gate, `firmware/ch32dbg.c`): whatever the computer sends, it writes
   memory only there and to the five flash controller registers that
   programming needs (KEYR, MODEKEYR, STATR, CTLR, ADDR).
2. **Never the option-byte key.** Nothing writes OBKEYR, so read and write
   protection cannot be changed, even by mistake. The gate refuses it.
3. **The right chip.** Before anything is written, the chip is identified
   through its debug module: a CH32V003 (chip ID, hartinfo and the ID word
   in memory, the check proven on hardware) or a CH32V005 (chip ID).
   wolfctl and the Wolfskralle check this independently and must agree on
   the flash and page size. A read-protected chip is never written.
4. **The right image, complete.** The image is read and checked before
   the module is touched: 1 to 32,768 bytes, and the Machdyne BASIC
   identity (`fw=Machdyne BASIC`) with the module it was built for
   (`mod=LS10A` or `mod=LS11A`), unless `--force`. Once the chip is
   identified, and still before anything is written: the image must fit
   its flash, and its module must match the chip, since their pins differ
   (unless `--force`). A refused image leaves the chip running as before.
   Every request to the Wolfskralle is checked by a CRC and runs exactly
   once (docs/protocol.md), and each page's data is checked again by a
   CRC-32 before the page is written.
5. **Stopped before written.** The core is stopped (halted, then reset with
   the halt held, so it waits at its reset vector) before any erase. On
   board v0 there is no reset line: the chip is stopped while running, or
   if it does not answer, after a power cycle (docs/debug.md).
6. **Verified before released.** Every page is read back and compared as
   it is written; on a mismatch the Wolfskralle erases and writes it again
   (up to three times). Then the whole flash is read back once more, as a
   CRC-32 computed on the Wolfskralle, and compared with the image's. If
   that differs, every page is checked and corrected once more. The flash
   is locked and the module restarted only after this passes; otherwise
   wolfctl reports the failure and leaves the module stopped (and its
   flash locked).
7. **Always recoverable.** Nothing above can leave a module that cannot be
   programmed again: the debug interface does not depend on the flash, and
   a power cut or an unplugged cable at any point leaves a module the next
   attempt programs. **Board v0's limit:** a firmware that turns SWIO off
   (or puts the chip to sleep) at once after starting cannot be stopped
   without the reset line; such a module needs board v1 or a WCH-LinkE
   with RESETN. Machdyne BASIC never does this.
8. **Files untouched.** Nothing is sent to the module's F-RAM or EEPROM:
   the Wolfskralle does not drive the Sechs pins.

## 2. How it goes

| Step | Where | |
|---|---|---|
| check the image | wolfctl | rule 4 |
| CONNECT | Wolfskralle | stop the core, read the identity (rules 5, 3); the gate opens for a CH32V003/V005 |
| check chip and image together | wolfctl | rules 3, 4; a refusal restarts the chip |
| FLASH_BEGIN | Wolfskralle | unlock the flash (only through the gate; never if read-protected) |
| VERIFY_PAGES | Wolfskralle | every page's CRC-32 compared with what it must hold, 28 pages a request: which pages differ |
| each page that differs: PAGE_WRITE (and PAGE_LOAD for a CH32V005's larger pages) | both | the page's data and its CRC-32; the Wolfskralle checks the data, compares the page once more, then erases, programs (unless blank) and verifies, up to five times |
| CRC32 of the whole flash | Wolfskralle | rule 6 |
| FLASH_END, RESUME | Wolfskralle | lock, restart |

Pages that already hold the right data are left alone: a factory-new
chip's blank pages beyond the image, or all of them when the same image
is flashed again. Only pages that differ are erased.

A SWIO packet that comes back damaged (docs/swio.md) is repeated when that
is safe: reads, and writes that leave the same state if done twice
(including the start of an erase). A flash key or the start of
programming is never repeated; its step fails and the page is erased and
written again. If the flash stays locked after the keys (a key may have
gone astray), wolfctl stops the chip again and retries.

## 3. Speed (phase 5)

Firmware 0.6 wrote each word through the program buffer as Werkzeug does
(about 30 SWIO packets a word, 7 for each word read) and sent every page
over USB: a full 16 KB CH32V003 took 23.3 s, the same image again 9.4 s.
From the two times: about 90 us per SWIO packet and 8 ms per USB request.

Firmware 0.7 changes three things:

- **Fewer USB requests.** The Wolfskralle first compares all pages with
  their CRCs (VERIFY_PAGES, 28 pages a request); only the pages that
  differ are sent. A CH32V003 page's 64 bytes travel inside PAGE_WRITE
  itself (one request a page instead of two). wolfctl asks a busy
  Wolfskralle every 4 ms at most during a flash, instead of 20.
- **The fast paths.** Reads use a two-word program (load the word at x8
  into x9, step x8) repeated by the debug module's auto-execution: each
  read of DATA0 returns a word and fetches the next, one SWIO packet a
  word. Page programming the same way: each write of DATA0 stores a word
  into the page buffer (x9 to x8, then PAGE_PG | BUF_LOAD to FLASH_CTLR,
  then x8 += 4). These are from the RISC-V debug specification (and what
  minichlink does); they need a program buffer of two words, which the
  chip reports in ABSTRACTCS.
- **One check per access.** The abstract-command status (ABSTRACTCS) is
  read once at the end of each access instead of after each command; an
  error, or a command still busy when the next came, stays set until it
  is read.

The fast paths are careful:

- **Reads are checked when connecting**: 8 words read both ways must be
  equal, or reads stay on the plain method. A fast read that fails is
  repeated the plain way.
- **Writes are checked by the page verify** (every page is read back as it
  is written). If a page written the fast way is wrong without any damaged
  packet, the Wolfskralle drops fast writes for the rest of the session
  and writes the plain way.
- **They stay inside the gate.** The write program stores only inside the
  page being written and to FLASH_CTLR; the gate is asked about the page's
  first and last word and FLASH_CTLR before it runs.
- **Auto-execution is switched off and read back** before anything else
  touches DATA0 (left on, the next access would run the program again).
  If that cannot be confirmed, the session ends: nothing more is written
  until a new CONNECT.
- **They never read past what was asked** (they read two words ahead, and
  stop early).
- `--slow` (on `flash`, `verify`, `id`, `read`, `crc`) switches them off,
  to compare or to rule them out. `wolfctl id` shows which are in use.

On hardware (firmware 0.7): a full 16 KB write took 10.9 s and 10.6 s
(before 23.3 s), with both fast paths in use throughout. `-v` showed
1905 of 2176 GETs answered BUSY: each a full 128-byte report, sixteen
bit-banged USB packets taken from the CPU that was writing the page.

Firmware 0.8 (protocol 6) therefore: sends only the bytes in use (a BUSY
answer is one packet); does not read a page before writing it when the
compare already found it different; and counts where the time goes
(`wolfctl -v flash`, `wolfctl profile`), so the next step is measured
rather than estimated.

In the simulation, a CH32V003 with a 12 KB image takes about 55,000
debug transactions and 270 USB requests (firmware 0.6: 150,000 and 450). On the
hardware's numbers, that is roughly 6-7 s for a full flash and 1-2 s for
the same image again. A shorter SWIO bit (docs/swio.md) would save
another 10-20%, but needs a scan first: the chip holds a read 0 for 41-44
ticks of the 60-tick bit.

## 4. Tests

`make -C host test` runs `host/test/flash_test`: wolfctl's job and USB
client, the Wolfskralle's protocol, command handlers and debug module code,
and a simulated chip whose flash controller counts anything the rules
forbid (writes outside page programming, outside the main flash, to
OBKEYR or the option bytes). It checks:

- a CH32V003 (12 KB and 16 KB images, on a used and a blank chip) and a
  CH32V005 (24 KB): written, verified, locked, running;
- the fast paths in use on a chip that has them; `--slow`; and chips
  without them (a one-word program buffer, no auto-execution, an
  auto-execution that skips words): still written right, the plain way,
  with no violation;
- no read past the end of what was asked (fast reads read ahead);
- the same image again: no page erased; one byte changed: only that page;
- refusals, with nothing written and the chip running on: an LS11A image
  on a CH32V003, an image too large, a non-BASIC image (refused before the
  module is touched; written with --force), a read-protected chip, an
  unsupported chip (also with --force), no module;
- the Wolfskralle's gate on its own, with wolfctl's checks bypassed: no
  unlock and no write on an unsupported chip; writes past the flash, to
  the option bytes or not page-aligned refused and counted;
- 1% of SWIO packets damaged (far more than the hardware has shown): each
  flash either succeeds with the right contents or fails cleanly;
- about 270 power cuts at random points (pages being erased or written
  are torn), each followed by a successful flash;
- no violation in any of it.

The tests were checked by breaking the code on purpose: a page compare
that looks at only the first word, a write to OBKEYR, and a fast read that
reads one word too far all make them fail.
