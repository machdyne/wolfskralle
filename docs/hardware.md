# Wolfskralle: hardware as the firmware sees it

This describes v0. v1 will differ only where noted (TRESET).

Taken from `pcb/wolfskralle_v0` (schematic and PCB netlist). The firmware's
`board.h` must match this table.

## MCU (U1, CH32V003F4U6, QFN-20)

| Pad | Pin | Net | Use |
|---|---|---|---|
| 1 | PD7/NRST | RESET | Wolfskralle's own reset (J2 pin 7) |
| 5 | PD0 | LED | white LED D3 (high = on, 1K5) |
| 8 | PC1 | TSWO | 100R to TSWIO; the firmware's SWIO input (TIM2 CH4 capture, partial remap 1) |
| 9 | PC2 | TSWI | 100R to TSWIO; the firmware's SWIO output (TIM2 CH2 PWM, partial remap 1) |
| 12 | PC5 | ZP1 | Zwölf pin 1 (A, Sechs SCL); I2C1 SCL with full remap; SWCLK for 2-wire chips |
| 13 | PC6 | ZP2 | Zwölf pin 2 (B, Sechs SDA); I2C1 SDA with full remap |
| 15 | PD1 | SWIO | Wolfskralle's own SWIO (J2 pin 5) |
| 16 | PD2 | TPWRON | target power: high = on (BSS138 -> P-FET Q2); R9 10K pulls it high, so the target is powered while the MCU is in reset |
| 17 | PD3 | USB_DP | rv003usb D+ |
| 18 | PD4 | USB_DN | rv003usb D- |
| 19 | PD5 | USB_DN_PU | 1K5 pull-up on D- (low speed) |

Unused: PA1, PA2, PC0, PC3, PC4, PC7, PD6.

PC1 and PC2 are joined at TSWIO through two 100R resistors (200R between the
pins). TSWIO has a 5K1 pull-up to TPWR. The names TSWO/TSWI are only labels:
the network is symmetric, and cnlohr's firmware drives PC2 and reads PC1.

## Target (spring contacts J1, J4)

| Zwölf pin | Name | Wolfskralle net | Connected to MCU |
|---|---|---|---|
| 1 | A (Sechs SCL, PROG on LS11) | ZP1, 10K to TPWR | PC5 |
| 2 | B (Sechs SDA) | ZP2, 10K to TPWR | PC6 |
| 3, 4 | C, D | ZP3, ZP4 | no |
| 5 | GND | GND | |
| 6 | 3V3 | TPWR | switched by PD2 |
| 7, 8, 9 | E, F, G | ZP7-ZP9 | no |
| 10 | H (PROG = SWIO on LS10/LS11) | TSWIO, 5K1 to TPWR | PC1, PC2 |
| 11 | RESETN | TRESET | **v0: no** (hardware bug, fixed in v1); button SW1 to GND, J2 pin 6 |
| 12 | WPN | ZP12 | no (left open, as the spec requires) |

## ISP header J2

```
7 5 3 1
8 6 4 2
```

1 ZP1/SWCLK, 2 TPWR, 3 ZP2, 4 TSWIO, 5 SWIO (own), 6 TRESET, 7 RESET (own), 8 GND.

## LEDs and power

- D2 green: 3V3 present. D1 blue: target power (TPWR). D3 white: firmware (PD0).
- ME6211 3.3V LDO feeds both the MCU and, through Q2, the target. TPWR has
  10 uF (C3) that charges from the same rail when target power switches on.

## Consequences for the firmware

1. On v0 the firmware cannot hold the target in reset; it can only
   power-cycle it (PD2). This is acceptable because v0 programs factory-new
   chips (and modules running Machdyne BASIC, which never turns SWIO off).
   v1 connects TRESET to the MCU; `board.h` selects the board, and the
   reset functions do nothing on v0.
2. SWIO is open-drain with a 5K1 pull-up, so rising edges are slow. The
   width of a "1" bit as the target sees it is the driven low time plus the
   rise time, which must stay below 4T (about 333 ns on a CH32V003).
3. Sechs (I2C) is reachable on PC5/PC6. The firmware bit-bangs it
   (docs/sechs.md) rather than use I2C1's full remap: TIM2's partial
   remap 1, which SWIO uses, also maps TIM2 CH1 to PC5. On an LS11,
   Zwölf pins 1 and 10 are the same signal (SCL and SWIO): SWIO is off
   while the bus is in use.
