# Wolfskralle Programmer

Wolfskralle is an ISP + spring contact programmer for CH32 MCUs and [Zwölf](https://github.com/machdyne/zwolf) [LS10](https://machdyne.com/product/zwolf-ls10/) modules.

![Wolfskralle](https://github.com/machdyne/wolfskralle/blob/38c62a5283cb2000488aab3b664369fcaf6a570f/wolfskralle.png)

This repo will contain schematics, pinouts and a 3D-printable socket housing for the spring connector.

Find more information on the [Wolfskralle product page](https://machdyne.com/product/wolfskralle-programmer/).

## Pinout

```
7 5 3 1
8 6 4 2
```

| Pin | Signal |
| --- | ------ |
| 1 | ZP1 (Zwölf GSCL) / SWCLK | 
| 3 | ZP2 (Zwölf GSDA) | 
| 5 | SWIO (programmer swio) | 
| 7 | RESET (programmer reset) | 
| 2 | TPWR3V3 (target power) | 
| 4 | TSWIO (target swio) | 
| 6 | TRESET (target reset) | 
| 8 | GND | 

## Firmware

The Wolfskralle's firmware and its command-line tool, `wolfctl`, are being
written in phases; see [docs/plan.md](docs/plan.md) for the plan and the
current state.

```
$ make -C firmware deps && make -C firmware      # firmware/wolfskralle.bin
$ make -C firmware flash                         # with a WCH-LinkE on J2
$ make -C host && make -C host test              # host/wolfctl, protocol tests
$ host/wolfctl info
$ host/wolfctl flash basic-ls10a.bin             # program the module
```

| Document | Contents |
|---|---|
| [docs/plan.md](docs/plan.md) | development plan and history |
| [docs/hardware.md](docs/hardware.md) | the board as the firmware sees it (pins, v0/v1) |
| [docs/firmware.md](docs/firmware.md) | building and flashing the firmware |
| [docs/protocol.md](docs/protocol.md) | the USB protocol |
| [docs/swio.md](docs/swio.md) | the link to the target: SWIO, its timing, the scan |
| [docs/debug.md](docs/debug.md) | connecting to the chip, identifying and reading it |
| [docs/programming.md](docs/programming.md) | programming a module, and the failsafe rules |
| [docs/wolfctl.md](docs/wolfctl.md) | the command-line tool |
| [docs/hwtest.md](docs/hwtest.md) | hardware tests and their results |

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
