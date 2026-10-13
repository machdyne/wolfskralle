# Sechs: checking a module over its bus

**Status: phase 6 (round 6). Builds; tested end to end against a
simulated Sechs module at the level of the bus lines; not yet on
hardware.**

Sechs is the module bus of Zwölf modules: pins 1 (A, SCL) and 2 (B, SDA)
carry I2C, and every Sechs module answers there with a signature, its
status and an identity text (the specification is machdyne/basic's
`docs/sechs.md`). Machdyne BASIC on an LS10A or LS11A is a Sechs module
at address 0x0C.

The Wolfskralle has both pins in its spring contacts, wired to PC5 and
PC6 with 10K pull-ups to target power (docs/hardware.md). So after
flashing, it can ask the module itself whether BASIC starts and runs:
flash verification proves the bytes are right; this proves the firmware
works.

```
$ wolfctl flash basic-ls10a.bin --check   # flash, then check
$ wolfctl sechs check                     # is BASIC installed and running?
$ wolfctl sechs check basic-ls10a.bin     # ... and is it this image?
$ wolfctl sechs scan                      # what answers on the bus
$ wolfctl sechs info                      # a module's registers and INFO
```

## 1. What `sechs check` does

1. **Restarts the module**: target power off for 300 ms and on again, so it
   starts exactly as after being plugged in (`--no-restart` to skip). A
   power-on also keeps an LS11 out of its programming mode.
2. **Waits for it** (up to 12 s): at 0x0C, or at `-a ADDR`, or else the
   one Sechs module a bus scan finds. BASIC answers in its boot window,
   which starts about as soon as it has power; an LS11 that does start in
   programming mode answers after that mode's 10 s.
3. **Reads it once its boot window is over** (so that its status says what
   it settled on): the signature `S6`, the Sechs version, CAPS, STATUS,
   OK, FAULT and INFO.
4. **Checks**, in this order:
   - INFO says `fw=Machdyne BASIC...`;
   - its `mod=` is the module the image was built for (LS10A or LS11A),
     when an image is given;
   - its whole INFO text appears in the image, when an image is given: the
     module runs firmware that identifies itself exactly as this image
     does (INFO carries no version, so two BASIC builds for the same
     module are not told apart);
   - no fault (STATUS bit 5, FAULT), not degraded, alive (OK bits 0-2);
   - **the console computes**: if the module has an I2C console and no
     program is running, it types `PRINT 1234+4321` and expects `5555`
     in the answer. This runs BASIC's interpreter, not just its Sechs
     code. If a program is running (a `BOOT.BAS` on the module's storage
     starts after the boot window), the console test is skipped rather
     than interrupt it (`--no-console` skips it always).

`flash --check` runs the same after a successful flash, with the image
just written. `-v` shows each step.

| Result | Meaning |
|---|---|
| Machdyne BASIC is installed and runs | everything above passed |
| no Sechs module answered | no BASIC (or another firmware without Sechs), the module did not start, or it is not seated |
| not Machdyne BASIC | a Sechs module with another firmware |
| for another module | BASIC for LS11A on an LS10A or the other way round (only with `--force`) |
| its identity is not the image's | it runs some other BASIC build |
| reports a fault or is degraded | for example a storage check that failed |
| the console did not answer ... with 5555 | the interpreter does not work |
| the Sechs bus fails | a line stays low, or the clock is held too long |

## 2. How the Wolfskralle drives the bus

`firmware/i2cm.c` is a bit-banged I2C master at about 35-40 kHz (the
specification asks modules for 100 kHz; slower is always allowed).
PC5 and PC6 are open drain: a line is pulled low, or released to its
pull-up; between transactions both pins are floating inputs, and the bus
is used only with target power on (the pull-ups are on target power, as
the specification requires of a carrier).

- **Clock stretching** is honoured, up to 50 ms.
- **The USB interrupt** may pause the master at any point; I2C has no
  lowest clock rate, so that only makes a transfer slower.
- **A module left halfway through a read** holds SDA low; before each
  transaction the master clocks up to nine times until SDA is free, then
  sends a stop. A line that stays low anyway is reported as a stuck bus.
- **SWIO is switched off** for every Sechs transaction: on an LS11, pin 1
  is also the chip's SWIO (Zwölf pins 1 and 10 are the same signal there),
  and the module must be running to answer. A debug session ends with it.

Firmware and `firmware/i2ccmds.c` add two commands (protocol version 7,
docs/protocol.md): I2C (one transaction: write, then read with a repeated
start) and I2C_SCAN (which addresses acknowledge). Everything Sechs
itself means (registers, INFO, the console) is in `host/sechs.c`.

## 3. Tests

`make -C host test` runs `host/test/sechs_test`: wolfctl's check, the USB
client, the Wolfskralle's protocol, its I2C commands and bit-banged master,
and a simulated Sechs module that follows the two open-drain lines: it
detects start, repeated start and stop, acknowledges its address and
bytes, stretches the clock at random, and has the core registers, INFO
(a port register), the program registers and an I2C console that runs
`PRINT a+b`, after the behaviour of machdyne/basic's `sechs.c` and LS1x
module code. It checks:

- 2000 register reads of every length, with random clock stretching;
  INFO; address-only probes, answered and not;
- a module left halfway through a read, holding SDA low: recovered;
- a clock held too long, and a shorted SDA: reported, not hung;
- the check: a good module (cold start, read after the boot window,
  console answering 5555); with the right image, the wrong module, another
  image; not BASIC; a fault; a program running (console not touched); a
  wrong console answer; a module at another address; no module (gives up
  after 12 s); a module that answers only after 10 s (an LS11 in
  programming mode); no target power; a shorted line.

The tests were checked by breaking the code on purpose: no wait for a
stretched clock, no recovery clocks, no firmware check, and typing at a
running program all make them fail.

## 4. Not yet

- Putting an LS11 into programming mode over Sechs (CONTROL 6, PROGRAM)
  before flashing, for a module whose program keeps SWIO busy. Not needed
  for factory-new chips or for BASIC, which leaves SWIO free.
- Sechs broadcasts, address changes, labels: `sechsctl` with a Werkzeug
  does those.
