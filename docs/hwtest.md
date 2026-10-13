# Wolfskralle hardware tests

Each phase of docs/plan.md ends with tests on real hardware. Record the
results here (date, board, host computer and USB port, module, numbers).

## Phase 0: the old firmware (optional)

What failed with `rvswdio_programmer`, to compare against later:

- `dmesg -w` while it fails: USB errors (`-71`, `-110`, "device not
  accepting address", resets) point to USB; none point to SWIO.
- With a scope on TSWIO (J2 pin 4): the width of a "1" bit, measured at
  half the supply, and the rise time. The CH32V003 accepts a 1 that is low
  for 1T-4T (about 83-333 ns).
- With a scope on 3V3 (the green LED's supply) while target power switches
  on: the depth and length of the dip. Phase 1's `wolfctl power on -m` can
  measure this without a scope (below).

Results: *(not yet done)*

## Phase 1: USB and protocol

Firmware: `firmware/wolfskralle.bin` (phase 1). No module needed unless
noted.

1. **Enumeration.** Plug in: the white LED blinks, then stays lit;
   `dmesg` shows "Machdyne Wolfskralle" with no errors; `wolfctl list`
   shows it with a serial number. Repeat 10 times, also on a USB hub.
2. **Info.** `wolfctl info`: board v0, firmware 0.2 (or later), supply about
   3300 mV, target power on.
3. **LED.** `wolfctl led off`, `on`, `auto`; in auto the LED flickers
   during `wolfctl ping 1000`.
4. **Target power.** With a module in the contacts: `wolfctl power off`
   (blue LED off), `wolfctl power on -m 100`. Note the lowest supply
   voltage during switch-on, with and without a module (hypothesis H3 in
   docs/plan.md). Below about 2.9 V the dip may disturb USB or the MCU.
5. **The gate: 100,000 pings without an error.**
   `wolfctl ping 100000`: 0 failed. Also note the counters (resends, set
   and get errors, bad replies): they show how often the protocol had to
   recover.
6. **With target power cycling.** With a module in the contacts:
   `wolfctl ping 10000 -p 100`: 0 failed, and the Wolfskralle is still
   there afterwards.
7. **Long commands.** `wolfctl ping 20 -w 1000`: 0 failed, many busy
   answers, no resends.
8. **Unplug during a run.** Start `wolfctl ping 100000`, unplug: wolfctl
   stops with "unplugged". Plug in again: `wolfctl info` works.
9. **Round trip time and throughput.** From test 5: the average round trip
   and payload bytes per second (this decides the report size in phase 5).

Results: *(not yet done)*

## Phase 2: SWIO

Firmware: phase 2 (`wolfctl info` shows firmware 0.2). A factory-new
module (LS10A or LS11A, or a bare CH32V003/CH32V005 on an adapter), and a
module running Machdyne BASIC.

1. **Something there.** `wolfctl swio-test 10`: no "no chip answered";
   0 wrong. Without a module: "no chip answered", configuration register
   `ffffffff`.
2. **The pulses.** `wolfctl swio-raw write 0x04 0x12345678` and
   `swio-raw read 0x7d`: 41 captures each. Note the measured low time of
   the 1s (open drain: the programmed 125 ns plus the rise time). Over
   300 ns, a 1 can be taken for something else (hypothesis H1).
3. **The scan.** `wolfctl swio-scan` on each module: the working range of
   a 1 and a 0, the read 1s and 0s and the margin. Then
   `wolfctl swio-timing mode=1` and `swio-scan` again: does push-pull
   widen the range? Keep the output of both.
4. **The gate: 10,000 round trips without an error, with USB under load,**
   on an LS10A and an LS11A in the spring contacts, with the default
   timing and with the scanned one:
   `wolfctl swio-test 10000 --load`: 0 wrong, 0 incomplete, 0 pings
   failed. Then `wolfctl swio-test 200000`: 0 wrong.
5. **Power.** `wolfctl power off`, then `wolfctl swio on`: refused (no
   power). `wolfctl power on`, `wolfctl swio-test 100`: works again.
6. **The module keeps running.** A module running BASIC, with a program
   that blinks an LED: during `swio-test 100000` it keeps blinking.

From the results, the defaults in `firmware/swio.c` will be set to a
timing that works for every module (with margin on both sides).

Results:

- **2026-10, first `swio-scan`** (firmware 0.3, open drain, default timing
  before the scan; module: not recorded). SWIO works.
  - A 1: 0 errors from 1 to 21 ticks (set); measured on the line 104-521
    ns, always 4 ticks (83 ns) more than set. At 22 ticks 135 errors, from
    23 no answer; but that scan also lengthened the read pulse with the
    1, so the upper limit is the read pulse's or the 1's (fixed in the
    scan since).
  - Read 1s rose 4 ticks after the read pulse; read 0s at 41-44 ticks,
    whatever the pulse. Threshold 28: 13 ticks margin each side (with an
    11-tick read pulse).
  - A 0: 0 errors from 25 to 55 ticks (521-1146 ns); 24 and below and 57
    and above no answer, 56: 260 errors.
  - The scan's result (1: 11, 0: 40, read 11, threshold 28): 5000 round
    trips, 0 errors.
  - Conclusions: the rise time is small (H1 does not hold); defaults
    changed to 1: 10 ticks, 0: 40, read 6, threshold 28 (firmware 0.4).
- **2026-10, second `swio-scan`** (firmware 0.4, the new scan; module:
  not recorded).
  - A 1 (read pulse held at 6): 0 errors from 1 to 21 ticks (104-521 ns
    on the line), 24 errors at 22, no answer from 23. So the upper limit
    of a 1 is real: about 520-540 ns on the line.
  - The read pulse (threshold 28): 0 errors from 1 to 23 ticks; at 24 a
    read 1 would rise at 28, the threshold itself, and nothing works. The
    limit is the threshold, as expected.
  - Read 1s 4 ticks after the read pulse; read 0s at 41-43 ticks.
  - A 0: 0 errors from 25 to 53 ticks (521-1104 ns), errors at 24 and at
    54-56, no answer below 24 and above 56.
  - Its result (1: 11, 0: 39, read 8, threshold 27) checked with 5000
    round trips: 0 errors. The defaults (1: 10, 0: 40, read 6, threshold
    28) sit as well inside every range, so they stay.
- **`swio-scan 10000`: a timeout at the 10th step**, and then `wolfctl id`
  timed out twice. Most likely not a SWIO problem: wolfctl gave a test of
  n round trips 3 s + n/4 ms (5.5 s for 10000), and with the host polling
  every 5 ms, the USB interrupt takes much of the Wolfskralle's time, so
  10000 round trips took about that long. After the timeout, the
  Wolfskralle went on with the rest of the test while the next wolfctl
  waited, again too briefly. Changed (round 3c): link tests run in
  commands of at most 1000 round trips with 2 ms allowed for each; the
  host polls a busy Wolfskralle at most every 20 ms (less load); a timeout
  now says why (still busy, USB failing, or no matching reply); and a
  watchdog restarts the Wolfskralle if its firmware ever hangs, which
  `wolfctl info` then reports ("started by: WATCHDOG"). If it happens
  again: `wolfctl -v info`, `dmesg | tail -30`, and whether the white LED
  was dark (main loop busy) or blinking (USB not configured).
- Still to do: the scan on the other module type (and record which is
  which), `swio-test 10000 --load` (the gate), `swio-test 200000` (now
  shows the time per round trip).

## Phase 3: debug access (read-only)

Firmware: phase 3 (firmware 0.3). Modules: a factory-new CH32V003 and
CH32V005 (LS10A, LS11A), a module running BASIC, and one flashed with a
known image by a WCH-LinkE.

1. **Identify.** `wolfctl id` on each: the right chip, flash 16 KB
   (CH32V003) or 32 KB (CH32V005), not read-protected, "attached". The
   module's program restarts afterwards (a BASIC module answers on Sechs
   again).
2. **Unique IDs.** Two modules of the same kind show different unique IDs.
3. **Read.** `wolfctl read -o dump.bin` on the module flashed with a
   WCH-LinkE: "read twice and equal", and `cmp dump.bin image.bin` (the
   image padded with what the chip has after it) shows no difference in
   the image's bytes. Note the time it took.
4. **CRC.** `wolfctl crc` equals `crc32` of the dump (Python:
   `zlib.crc32(open('dump.bin','rb').read())`).
5. **The gate: the dump matches**, 10 times in a row, on both chips.
6. **Restart.** `wolfctl id --restart`: "power-cycled and caught at
   start". Note the supply dip with `wolfctl power on -m 100` first.
7. **Recovery (v0, best effort).** A module whose program turns SWIO off
   at once cannot be caught on v0 (that needs board v1). One that does so
   after a delay (say 100 ms): `wolfctl id --attach` fails,
   `wolfctl id` succeeds by power-cycling.
8. **Stopped.** `wolfctl id --halt`: the module's program stops (a
   blinking LED stops); `wolfctl resume`: it runs again.

Results:

- **2026-10, `wolfctl id`** (firmware 0.5): CH32V003, 16 KB, attached to
  the running chip; chip ID 00310510, hartinfo 002120f4, ID word 00310510,
  OBR 0000035c (not read-protected); unique ID 9c5fabcd059bbd6f, then
  ffffffff (the CH32V003 has 64 unique bits). `wolfctl info` afterwards:
  started by power-on, 270 commands, no repeats, bad requests or
  rejections.
- Still to do: `read` against a known image, `crc`, `--restart`.

## Phase 4: programming

Firmware: phase 4 (firmware 0.6). Nothing here should leave a module that
needs rescuing, but for the first flashes use modules you can reprogram
some other way too.

1. **Refusals first** (nothing written; the module keeps running): an
   LS11A image on an LS10A (CH32V003): "for the other module"; a file that
   is not Machdyne BASIC: refused before the module is touched; no module:
   "no chip answers".
2. **The first flash.** `wolfctl flash` a BASIC image on a module whose
   contents you have a copy of (`wolfctl read -o before.bin` first): "written
   and verified", the time it took, the module restarts and runs BASIC
   (answers on Sechs). `wolfctl verify` the image: holds it.
3. **Again.** The same flash again: every page "already right", much
   faster.
4. **The gate: 10 flashes in a row** on an LS10A, alternating two
   different images, each verified, and the module running the right one
   after each.
5. **The CH32V005.** The same on an LS11A (programming the CH32V005 has
   only been tested in simulation anywhere so far, also in machdyne/basic).
6. **Interruptions.** During a flash, unplug the USB cable; plug it back
   in and flash again: written and verified. The same with the module
   lifted out of the contacts for a moment during a flash.
7. **`--force`** with non-BASIC firmware (for example a blink program built
   with ch32fun for the module's chip): written, runs.

Results:

- **2026-10, the first flash** (firmware 0.6, LS10A / CH32V003): a BASIC
  image of 16,376 bytes, "written and verified" in 23.3 s, 256 pages
  written (none already right), CRC-32 fdb5f18b of the whole 16 KB.
  `wolfctl verify` afterwards: the chip holds it.
- **The same image again**: 9.4 s, 0 pages written, 256 already right.
  So reading costs about 9 s of 23 (8192 word reads of about 7 packets,
  plus 512 USB requests), writing the rest: about 90 us per SWIO packet
  and 8 ms per USB request. This set the targets of phase 5.
- Still to do: the refusals, 10 in a row, the LS11A, interruptions,
  `--force`.

## Phase 5: speed

Firmware 0.7 (protocol 5). Same modules as phase 4.

1. **Fast paths found.** `wolfctl id`: "fast paths: reads and writes",
   and the program buffer's size. (If it says "none" or only "reads",
   note it: the slow way still works.)
2. **Times.** `wolfctl flash` of the 16 KB image on a module holding
   something else (flash another image first), then the same image again.
   Compare with 23.3 s and 9.4 s. `wolfctl -v flash` also shows the fast
   paths used.
3. **Slow for comparison.** The same with `--slow`: about the old times.
4. **Every page right.** After each, `wolfctl verify`, and the module runs.
5. **Phase 4's tests** again with firmware 0.7: refusals, 10 in a row,
   the LS11A, interruptions.

Results:

- **2026-10, firmware 0.7** (LS10A / CH32V003): `wolfctl id`: fast paths
  reads and writes, program buffer 8 words. A full 16 KB write (an image
  of zeros, `--force`): 10.9 s; the BASIC image after it: 10.6 s
  (firmware 0.6: 23.3 s). `-v`: fast paths still both in use at the end,
  no retries, 271 commands, 2176 GETs of which 1905 BUSY, no errors.
  Each busy GET was a full 128-byte report: the reason for firmware 0.8's
  short replies.
- **2026-10, firmware 0.8**: a full 16 KB write of the BASIC image over
  another: **8.4 s**, all 256 pages written, verified, no retries, fast
  paths in use throughout. Profile: 6.64 s running commands (erasing
  1.88 s, programming 2.70 s, reading back 0.66 s, CRCs 1.27 s, no page
  read before writing), 69,730 SWIO packets (none incomplete, 95 us each
  including everything around them), 3117 flash controller busy polls,
  2616 GETs (2344 BUSY, now one USB packet each). About 1.8 s is USB
  round trips. Phase 5 closed at this: 23.3 s -> 8.4 s.

## Phase 6: the Sechs check

Firmware 0.9 (protocol 7). An LS10A with BASIC; if possible an LS11A; a
module with another firmware (or a factory-new chip).

1. **Scan and info.** `wolfctl sechs scan`: 0x0C, LS10A, Machdyne BASIC.
   `wolfctl sechs info`: Sechs 0.5, status idle (or running a program),
   ok 0f, INFO lines.
2. **The check.** `wolfctl -v sechs check`: answered within about 1 s of
   power-on, console `PRINT 1234+4321 -> 5555`, "installed and runs".
3. **With the image.** `wolfctl sechs check basic-ls10a.bin`: "its INFO
   is the image's". The LS11A image on the LS10A: "for another module".
4. **Flash and check.** `wolfctl flash basic-ls10a.bin --check`.
5. **Not BASIC.** A factory-new chip or another firmware: "no Sechs module
   answered" (after 12 s).
6. **A running program.** A module with a BOOT.BAS: the console test is
   skipped, the program keeps running.
7. **SWIO afterwards.** `wolfctl id` after a check: still works (SWIO
   switches back on by itself).
8. **The LS11A**, where pin 1 is SCL and SWIO at once: steps 2-4 and 7.

Results: *(not yet done)*
