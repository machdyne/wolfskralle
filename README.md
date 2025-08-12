# Wolfskralle Programmer

Wolfskralle is an ISP + spring contact programmer an for CH32 MCUs and [Zwölf LS10](https://github.com/machdyne/zwolf) modules.

This repo will contain schematics, pinouts and a 3D-printable socket housing for the spring connector.

Find more information on the [Wolfskralle product page](https://machdyne.com/product/zwolf-wolfskralle/).

## Pinout

```
7 5 3 1
8 6 4 2
```

| Pin | Signal |
| --- | ------ |
| 1 | ZP1 (GSCL) / SWCLK | 
| 3 | ZP2 (GSDA) | 
| 5 | SWIO (programmer firmware) | 
| 7 | RESET | 
| 2 | TPWR3V3 (target power) | 
| 4 | TSWIO (target swio) | 
| 6 | TRESET (target reset) | 
| 8 | GND | 

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
