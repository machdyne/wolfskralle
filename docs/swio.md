# SWIO: the link to the target

**Status: phase 2. Works on hardware: the first `swio-scan` (one module,
2026-10) found wide working ranges and no errors at the chosen timing
(docs/hwtest.md). More modules to measure.**

The CH32V00x chips are programmed through their single-wire debug
interface (SWIO). On the Wolfskralle it is TSWIO, the module's pin 10
(PROG on LS10 and LS11). This document describes how the firmware drives
it (`firmware/swio.c`) and how to find and check the timing.

## 1. The signal

Everything is a low pulse on the line, whose length is the bit (WCH's
*QingKe V2 Microprocessor Debug Manual*; the details as proven on an LS10A
by machdyne/basic, docs/ch32prog.md there):

| Pulse | Rule (the chip's debug clock T is about 83 ns) | Measured on an LS10A (Werkzeug) |
|---|---|---|
| a 1 | low for T to 4T (83-333 ns) | works 120-300 ns |
| a 0 | low for at least 6T (500 ns) | works 500-2000 ns |
| between bits | high for T to 16T (83 ns-1.33 us) | |
| after a packet | high for at least 18T | works from 2 us |

A packet is 41 bits, most significant first: a start bit (1), the 7-bit
debug register address, 1 for a write or 0 for a read, then either 32 data
bits (write) or 32 read slots (read). A read slot starts with a short low
pulse from the programmer; the chip then holds the line low for a 0, or
lets it rise for a 1.

## 2. How the Wolfskralle makes it

The CH32V003 bit-bangs USB in an interrupt, which must never wait for more
than a few microseconds. A SWIO packet takes about 50 us. So the CPU does
not make the pulses: TIM2 and two DMA channels do, and the CPU (and with it
the USB interrupt) stays free during the packet.

| Part | Does |
|---|---|
| TIM2, 48 MHz (1 tick = 20.8 ns), partial remap 1 | one period per bit |
| TIM2 CH2 on PC2 (TSWO in the schematic), PWM, active low | the line low from the start of the period for the bit's low time, then released |
| TIM2 CH4 on PC1 (TSWI in the schematic), input capture, rising edge | when the line was high again, counted from the start of the period |
| DMA1 channel 2 (TIM2 update) | the next bit's low time into CH2, every period |
| DMA1 channel 7 (TIM2 CH4) | each capture into RAM |

PC1 and PC2 meet at TSWIO through 100R each; TSWIO has a 5K1 pull-up to
target power. The output is open drain: the line rises through the pull-up,
which takes time (below). In **mode 1**, write packets are driven push-pull
instead (the chip never drives the line during a write packet), for fast
rising edges; read packets stay open drain, because the chip answers on
the line.

Every bit comes back as a capture: the time from its falling edge to the
line being high again. For a read slot that is the answer (early: 1, late:
0, split at the threshold). For every other bit it is a measurement: the
low time the chip actually saw, rise time included. `wolfctl swio-test`
shows the shortest and longest of each kind, and `wolfctl swio-raw` every
one of a packet. A packet that does not come back with exactly 41 captures
is an error (incomplete), never a value.

The SWIO pins are floating inputs except while SWIO is on, and SWIO is
switched off whenever target power is, so the Wolfskralle cannot feed an
unpowered module through them.

## 3. The timing

| Name | Default | | |
|---|---|---|---|
| period | 60 ticks | 1250 ns | one bit |
| one | 10 | 208 ns | low time of a 1, open drain (about 290 ns on the line) |
| one-pp | 10 | 208 ns | low time of a 1 in push-pull write packets (mode 1) |
| zero | 40 | 833 ns | low time of a 0 |
| read | 6 | 125 ns | low time that starts a read slot |
| thresh | 28 | 583 ns | a read slot that is high again before this is a 1 |
| mode | 0 | | 0: open drain; 1: write packets push-pull |
| stop | 4 | us | line high after each packet |

With a fixed period, a 1 is followed by period - one of high (must stay
below 16T, about 1.33 us) and a 0 by period - zero (at least T, plus the
rise time).

What the first scan on hardware showed (docs/hwtest.md):

- The line is fast. Every pulse measures 4 ticks (83 ns) longer than set,
  at every setting: the rise through the 5K1 pull-up plus the input's
  synchroniser (2-3 ticks). The slow-edge suspicion (H1 in docs/plan.md)
  does not hold for this board: open drain is enough, mode 1 is not
  needed.
- A 1 works from 1 tick to 21 (104-521 ns measured on the line), much
  wider than the 4T the manual implies; from about 540 ns on the line it
  fails (second scan, with the read pulse held). A 0 works from 25 to
  53-55 ticks (521-1146 ns); above that the gap before the next bit is
  too short.
- A read pulse works from 1 tick up to where its read 1 reaches the
  threshold.
- The chip holds a read 0 until 41-44 ticks (about 900 ns) after the
  falling edge; a read 1 rises 4 ticks after the read pulse ends.

The defaults sit in the middle of those ranges: a 1 of 10 ticks, a 0 of
40, a read pulse of 6 (a short one leaves the most room below the
threshold), threshold 28 (read 1s at about 10, 0s from 41).

The timing lives in RAM: it returns to the defaults when the Wolfskralle
restarts. Once measurements show what works for every module, the
defaults will be changed to that.

## 4. Finding the timing: swio-scan

```
$ wolfctl swio-scan            # 300 round trips per step
$ wolfctl swio-scan --apply    # and use the result
```

With a module in the contacts and target power on, `swio-scan`:

1. tries every low time of a 1 from 1 tick up (writes and packet
   headers; the read slots keep their pulse), shows the errors and the
   measured pulses for each, and picks the middle of the longest working
   range;
2. tries every read pulse up to the threshold, with the read 1s and 0s
   seen, and picks one in the lower third of the working range (a short
   pulse leaves the most room below the threshold);
3. sets the read threshold halfway between the latest read 1 and the
   earliest read 0 (and shows the margin);
4. tries every low time of a 0, and picks the middle of its working range;
5. checks the result with 5000 round trips.

"No answer" in a row means the configuration register did not read back
the key with that timing: the timing does not work at all.

(Before firmware 0.4 / this wolfctl, step 1 changed the read pulse along
with the 1, so its upper end mixed two limits.)

In mode 1 (`wolfctl swio-timing mode=1` first) step 1 scans `one-pp`
instead, keeping `one` for the read packets.

The scan writes only the debug module's data register (and its
configuration and control registers, as every connection does); it never
halts the chip on purpose or touches flash. A timing that does not work
can garble a packet into another register, which in the worst case stops
or resets the module's program: harmless for the module, but do not scan
a module whose running program matters.

## 5. Checking the link: swio-test

Long tests run as many commands of at most 1000 round trips, so no single
command keeps the Wolfskralle busy for long. `swio-test` reports the time
taken and the time per round trip.

```
$ wolfctl swio-test 10000          # write and read back 10000 times
$ wolfctl swio-test 10000 --load   # with USB pings in between
```

As `sechsctl swio-test` on Werkzeug: it enables the chip's debug output,
then writes a different value to the data register and reads it back,
without stopping the module's program. It reports wrong round trips,
incomplete packets and the measured pulses. `--load` runs it in short
pieces with a full-size ping after each, with the host polling as fast as
it can, so that USB traffic and SWIO packets overlap all the time: the
phase 2 gate.

## 6. Troubleshooting

| Seen | Likely |
|---|---|
| "no chip answered", configuration register `ffffffff` | nothing pulls the line low: no module, not seated, target power off, or a module whose SWIO pin is not pin 10 |
| configuration register `00000000` | the line is held low |
| errors at every low time of a 1 | measured 1s much longer than the low time set: the line rises slowly; try mode 1 |
| written 1s measured above 300 ns | outside the window: shorten `one` |
| read 1s and 0s overlap | the threshold cannot separate them: check the rise time (written 1s) |
| incomplete packets | a spurious edge or a missing one: noise or a bad contact |
