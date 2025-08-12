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

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
