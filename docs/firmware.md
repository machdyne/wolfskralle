# Wolfskralle firmware

**Status: phase 6 (USB, protocol, target power, LED, supply measurement;
SWIO; debug access; programming behind a write gate; the fast paths; the
Sechs bus). Phases 1-5 work on hardware; the Sechs bus is tested in
simulation.** See docs/plan.md.

## Building

You need a RISC-V GCC and newlib headers. On Debian or Ubuntu:

```
$ sudo apt install gcc-riscv64-unknown-elf libnewlib-dev picolibc-riscv64-unknown-elf
```

Then:

```
$ cd firmware
$ make deps            # ch32fun and rv003usb at the pinned versions
$ make                 # wolfskralle.bin
```

`make deps` clones into `firmware/ch32fun` and `firmware/rv003usb` (ignored
by git). The pinned commits are in the Makefile; update them deliberately.

| Option | |
|---|---|
| `BOARD=v1` | build for board v1 (TRESET connected; set `PIN_TRESET` in `board.h` first) |
| `DEBUG=1` | printf over the Wolfskralle's own SWIO, for a WCH-LinkE |

Size (phase 6, firmware 0.9): about 12.8 KB of 16 KB flash, 908 bytes of
2 KB RAM (256 of them the page buffer).

## Flashing the Wolfskralle

With a WCH-LinkE on the ISP header J2 and the Wolfskralle plugged into USB:

| J2 pin | Signal | WCH-LinkE |
|---|---|---|
| 5 | SWIO (the Wolfskralle's own) | SWDIO |
| 8 | GND | GND |

```
$ make flash           # builds minichlink from ch32fun if needed
                       # (which needs libusb-1.0-0-dev and libudev-dev)
```

J2 pins 2, 4 and 6 are the *target* side (TPWR, TSWIO, TRESET): not these.

After flashing, the Wolfskralle restarts and enumerates as "Machdyne
Wolfskralle"; `wolfctl info` should answer (docs/wolfctl.md).

## Files

| File | |
|---|---|
| `firmware/wolfskralle.c` | start-up, commands, rv003usb callbacks, LED, supply measurement |
| `firmware/wkdev.c`, `wkdev.h` | the device side of the protocol (hardware-independent, also built by the host tests) |
| `firmware/swio.c`, `swio.h` | SWIO with TIM2 and DMA, the link test (docs/swio.md) |
| `firmware/ch32dbg.c`, `ch32dbg.h` | the target's debug module: connect, stop, identify, read, restart; the write gate and page programming (hardware-independent, also built by the host tests; docs/debug.md, docs/programming.md) |
| `firmware/dbgcmds.c`, `dbgcmds.h` | the protocol commands for the debug module and the flash (hardware-independent) |
| `firmware/i2cm.c`, `i2cm.h` | a bit-banged I2C master for the Sechs bus (hardware-independent: pin hooks in `wolfskralle.c`; docs/sechs.md) |
| `firmware/i2ccmds.c`, `i2ccmds.h` | the protocol commands for the Sechs bus (hardware-independent) |
| `firmware/swio_bits.h` | SWIO packets as pulse widths and back (also built by the host tests) |
| `firmware/board.h` | pins, board v0/v1 |
| `firmware/usb_config.h` | rv003usb settings and the USB descriptors |
| `firmware/funconfig.h` | ch32fun settings |
| `common/wkproto.h` | the protocol, shared with `wolfctl` |

## Behaviour

- **Start:** target power on (R9 already holds it on while the MCU
  starts), LED on, then USB.
- **LED (auto):** blinks until the host has configured the device, then
  stays lit and goes dark for 40 ms on each command. `wolfctl led` changes
  the mode.
- **USB:** the interrupt only copies bytes and picks which buffer to send;
  it never waits for the main loop (docs/protocol.md).
- **Target pins:** PC1, PC2 (SWIO) and PC5, PC6 (Sechs) are floating
  inputs until used. PC5 and PC6 are driven only low (open drain), only
  during a Sechs transaction, and only with target power on. SWIO switches on only with target power on, and off
  whenever target power goes off, so the Wolfskralle cannot feed current
  into an unpowered target through them.
- **Watchdog:** 4 s. The main loop, every SWIO packet and every long
  wait feed it, so only a hang lets it run out; it then restarts the
  Wolfskralle (USB enumerates again). `wolfctl info` shows why the
  Wolfskralle last started ("started by"); a watchdog restart is a bug
  worth reporting. It stops while a debugger (WCH-LinkE) halts the CPU.
- **SWIO:** TIM2 and DMA make the pulses, so the USB interrupt runs during
  packets; a command that runs SWIO (such as a long `swio-test`) answers
  BUSY to the host meanwhile (docs/swio.md).
