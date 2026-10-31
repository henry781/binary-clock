# Hardware

The schematic and the PCB are in [`hardware/`](../hardware) (KiCad 10). Reference
designators, pin numbers and net names below are those of the schematic.

- [Block diagram](#block-diagram)
- [Power input](#power-input)
- [Microcontroller](#microcontroller)
- [Timebase](#timebase)
- [Display](#display)
- [Light sensor](#light-sensor)
- [DCF77 receiver](#dcf77-receiver)
- [Buttons and connectors](#buttons-and-connectors)
- [PCB](#pcb)
- [Datasheets](#datasheets)

## Block diagram

```
 5 V ── J_PWR2 ── D_TERM1 ──┬── +5V rail: logic, LEDs
                           │
                           └── U_LDO1 ── 3.3 V ── MOD_DCF1 DCF77 receiver
                                                        │
                                                    Q_DCF1 level shifter
                                                        │ DCF77_CLEAN
 Y1 32.768 kHz crystal ──────────────────────┐          │
 Q_PT1 light sensor ─────────────────────────┤          │
 SW1, SW2, SW3 ──────────────────────────────┤          │
                                     ┌───────┴──────────┴────────┐
                                     │ U_MCU1  ATmega328P        │
                                     └───────┬──────────┬────────┘
                              CLK, MR (×3)   │          │ PWM_DIM
                                     ┌───────┴──────┐   │
                                     │ U3, U4, U5   │   │
                                     │ CD4040       │   │
                                     └───────┬──────┘   │
                                     Q1–Q6   │          │ ~OE
                                     ┌───────┴──────────┴────────┐
                                     │ U11, U12, U13  74HC245    │
                                     └───────┬───────────────────┘
                                             │
                                 17 × (resistor + LED) ── GND
```

The time is held by three CD4040 binary counters (seconds, minutes,
hours) whose outputs light the LEDs through 74HC245 buffers. The
ATmega328P keeps time on its 32.768 kHz crystal, clocks and resets the
counters, decodes DCF77, reads the buttons and dims the display by
switching the buffers' output enable (`~OE`) with a PWM signal. The
microcontroller never drives an LED directly, and there is no
multiplexing: each LED has its own buffer output and resistor.

## Power input

| Ref | Part | Role |
|-----|------|------|
| `J_PWR2` | Phoenix MKDS 1,5/2 screw terminal, 5.00 mm | 5 V input, pin 1 `+`, pin 2 `−` |
| `D_TERM1` | 1N5817 Schottky diode | reverse-polarity protection |
| `C_BULK1` | 100 µF / 16 V electrolytic | bulk capacitor on the `+5V` rail |

The clock is meant to hang on a wall in front of a cable coming out of
it: the cable passes through the hole `H_CABLE1` (Ø 5.5 mm) and is screwed
into `J_PWR2`, mounted on the front with its wire entry facing the hole.
Any regulated 5 V supply of 500 mA or more works (for example a USB
charger with its cable cut).

`D_TERM1` drops 0.15 to 0.3 V depending on the load, so the `+5V` rail
sits at about 4.7 to 4.85 V with a 5.0 V supply. Every part on the board
accepts this. The diode also
keeps the 5 V of an ISP programmer connected to `J_ISP1` from flowing back
into the supply.

Estimated consumption from the 5 V supply:

| Condition | Current |
|-----------|---------|
| Dark room (LEDs at 0.1 %) | about 2 mA |
| Lit room (LEDs at about 50 %) | about 25 mA |
| Daylight, worst time of day (14 LEDs at 100 %) | under 100 mA |

At most 14 LEDs are lit at once (4 for the hours, 5 each for minutes and
seconds, for example at 23:59:59). The microcontroller draws about
1.5 mA, the CMOS logic a few tens of µA, the DCF77 receiver and its
level shifter about 0.2 mA.

A power cut loses the time: the clock restarts at 00:00:00 and sets
itself again from DCF77, usually within 5 to 15 minutes, longer when
reception is poor ([dcf77-decoder.md](dcf77-decoder.md#validation)).

## Microcontroller

`U_MCU1`, ATmega328P-PU (DIP-28), runs from its internal 8 MHz RC
oscillator. The crystal is reserved for Timer2.

| Pin | Port | Net | Function |
|----:|------|-----|----------|
| 1 | PC6 / ~RESET | `MCU_RESET` | reset, `R_RST1` 10 kΩ pull-up, `J_ISP1` |
| 2 | PD0 | `DBG_RX` | debug UART RX (unused, input with pull-up), `J_DBG1` |
| 3 | PD1 | `DBG_TX` | debug UART TX, 9600 baud, `J_DBG1` |
| 4 | PD2 | — | unused, output LOW |
| 5 | PD3 / INT1 | `DCF77_CLEAN` | DCF77 signal (idle HIGH, pulses LOW) |
| 6 | PD4 | `SW1_IN` | button hours +1 |
| 7 | VCC | `+5V` | `C_DEC_MCU_VCC1` 100 nF |
| 8 | GND | `GND` | |
| 9 | PB6 / TOSC1 | — | crystal `Y1`, `C1` 15 pF |
| 10 | PB7 / TOSC2 | — | crystal `Y1`, `C2` 15 pF |
| 11 | PD5 | — | unused, output LOW (next to TOSC2, must not switch) |
| 12 | PD6 | `SW3_IN` | button seconds reset |
| 13 | PD7 | `CLK_1MIN` | clock of the minutes counter `U4` |
| 14 | PB0 | `CLK_1HZ` | clock of the seconds counter `U3` |
| 15 | PB1 / OC1A | `PWM_DIM` | dimming PWM to `~OE` of `U11`–`U13` |
| 16 | PB2 | `CLK_1HR` | clock of the hours counter `U5` |
| 17 | PB3 / MOSI | `MOSI` | `J_ISP1` |
| 18 | PB4 / MISO | `MISO` | `J_ISP1` |
| 19 | PB5 / SCK | `SCK` | `J_ISP1` |
| 20 | AVCC | — | through `L_AVCC1` ferrite bead, `C_DEC_MCU_AVCC1` 100 nF |
| 21 | AREF | — | `C_AREF1` 100 nF |
| 22 | GND | `GND` | |
| 23 | PC0 | `MR_S` | reset of the seconds counter `U3` |
| 24 | PC1 | `MR_M` | reset of the minutes counter `U4` |
| 25 | PC2 | `MR_H` | reset of the hours counter `U5` |
| 26 | PC3 | `SW2_IN` | button minutes +1 |
| 27 | PC4 / ADC4 | `LIGHT` | light sensor |
| 28 | PC5 | — | unused, output LOW |

The ADC uses AVCC as its reference; `L_AVCC1` and `C_DEC_MCU_AVCC1` keep
the LED switching noise of the `+5V` rail out of it. The fuses select the
internal 8 MHz oscillator without the divide-by-8 and a 2.7 V brown-out
level, the minimum for 8 MHz operation (see
[firmware.md](firmware.md#fuses)).

## Timebase

| Ref | Part |
|-----|------|
| `Y1` | 32.768 kHz watch crystal, KDS DT-38, CL = 12.5 pF, ±20 ppm |
| `C1`, `C2` | 15 pF C0G, from each crystal pin to GND |

Timer2 runs in asynchronous mode from `Y1`: 32,768 Hz / 128 / 256 gives
the 1 Hz tick, and it keeps counting while the CPU sleeps. The
oscillator is the Pierce amplifier built into the ATmega328P; no external
feedback resistor is needed.

The crystal sees the two capacitors in series, C1·C2 / (C1 + C2) =
7.5 pF, plus 5 to 7 pF of pin and trace capacitance: 12.5 to 14.5 pF,
close to the 12.5 pF it is specified for. Each crystal sits within ±20 ppm (±1.7 s/day);
the exact error depends on the part and the board, and can be trimmed by
changing `C1` and `C2` after a drift measurement: a clock that runs slow
needs less capacitance (10 pF), one that runs fast more (22 pF); each step
moves the frequency by roughly 15 to 25 ppm. DCF77 corrects the time
every minute anyway when the signal is received.

Layout rules for the 32 kHz oscillator, which is high-impedance and
easily disturbed:

- `Y1`, `C1` and `C2` sit close to pins 9 and 10, over the ground plane.
- The PWM, the only signal that switches continuously, runs more than
  10 mm away from the crystal circuit. The tracks that pass closer carry
  DC or slow signals: counter clocks, buttons, DCF77.
- Nothing switches on the pins next to TOSC1/TOSC2: pin 8 is GND and pin
  11 (PD5) is a static output. A 488 Hz signal on PD5 couples into TOSC2
  strongly enough to lose one Timer2 count in 256 (the clock runs 0.4 %
  slow); this is why the PWM is on PB1.
- Never probe pins 9 and 10: a probe's capacitance stops the oscillator.
  The `crystal_test` sketch outputs the crystal frequency divided by two on
  PB3 instead.

## Display

### Counters

`U3` (seconds), `U4` (minutes) and `U5` (hours) are CD4040BE 12-stage
binary counters. Only the first six stages are used (five for the
hours). A counter advances on the falling edge of CLK (pin 10) and is
cleared while MR (pin 11) is HIGH. It has no parallel load: the firmware
sets a value by pulsing MR and then sending N clock pulses. Unused outputs
are left open.

### Buffers

`U11` (hours), `U12` (minutes) and `U13` (seconds) are SN74HC245N bus
transceivers used as buffers: the CD4040 outputs cannot source the few
milliamps an LED needs, the 74HC245 can.

- `DIR` (pin 1) is tied to `+5V`: direction A → B.
- `~OE` (pin 19) of all three is `PWM_DIM`. The LEDs are on while it is
  LOW. `R_OE1` (10 kΩ) pulls it HIGH, so the LEDs stay off while the
  microcontroller is in reset or being programmed.
- Unused A inputs are tied to GND (`U11` pins 7–9, `U12` and `U13` pins 8
  and 9); the matching B outputs are left open.

### LEDs

| Row | LEDs | Color | Series resistor | Current at 100 % |
|-----|------|-------|-----------------|------------------|
| Hours | `D_R1`–`D_R5` | red, 625 nm | `R_LED_R1`–`R_LED_R5`, 330 Ω | about 7 mA |
| Minutes | `D_G1`–`D_G6` | green, 525 nm | `R_LED_G1`–`R_LED_G6`, 220 Ω | 3.5–7 mA |
| Seconds | `D_B1`–`D_B6` | blue, 470 nm | `R_LED_B1`–`R_LED_B6`, 220 Ω | 3.5–7 mA |

The LEDs are 10 mm diffused types with a 60° viewing angle. The current
is (V_OH − V_F) / R, with the buffer output about 0.3 V below the
4.7 V rail; for the green and blue LEDs it depends on the forward voltage
of the batch (2.9–3.6 V). The cathodes go straight to GND.

Each row reads as a binary number with weights 1, 2, 4, 8, 16, 32 from
left to right, as printed on the board:

| Weight | Counter output | Buffer pins A → B | Hours LED | Minutes LED | Seconds LED |
|-------:|----------------|-------------------|-----------|-------------|-------------|
| 1  | Q1, pin 9 | `U11` 6 → 14; `U12`, `U13` 7 → 13 | `D_R5` | `D_G6` | `D_B6` |
| 2  | Q2, pin 7 | `U11` 5 → 15; `U12`, `U13` 6 → 14 | `D_R4` | `D_G5` | `D_B5` |
| 4  | Q3, pin 6 | `U11` 4 → 16; `U12`, `U13` 5 → 15 | `D_R3` | `D_G4` | `D_B4` |
| 8  | Q4, pin 5 | `U11` 3 → 17; `U12`, `U13` 4 → 16 | `D_R2` | `D_G3` | `D_B3` |
| 16 | Q5, pin 3 | `U11` 2 → 18; `U12`, `U13` 3 → 17 | `D_R1` | `D_G2` | `D_B2` |
| 32 | Q6, pin 2 | `U12`, `U13` 2 → 18 | — | `D_G1` | `D_B1` |

Counter outputs carry the net labels `H0`–`H4`, `M0`–`M5` and `S0`–`S5`
(bit number = log2 of the weight). Output names follow the CD4040 data
sheet, where Q1 is the least significant bit.

## Light sensor

| Ref | Part | Role |
|-----|------|------|
| `Q_PT1` | Vishay TEPT5700 | ambient light phototransistor, collector to `+5V` |
| `R_PT1` | 470 kΩ | load resistor, emitter to GND |
| `C_PT1` | 100 nF | filter |

The TEPT5700 is a current source driven by light; `R_PT1` turns its
current into the voltage `LIGHT`, read on ADC4. `C_PT1` and `R_PT1` form a
47 ms low-pass filter that removes the 100 Hz flicker of lamps and
gives the ADC a low source impedance. The 470 kΩ value puts a lit room
near the middle of the ADC range; the sensor saturates under a nearby
lamp or in daylight. Measured on the bench with this value:

| Light | ADC reading | LED duty cycle |
|-------|------------:|---------------:|
| Dark room | 0 | 0.1 % (floor) |
| Office tube light at 1 m | 29 | 2.8 % |
| Dim evening light | about 100 | 10 % |
| Lightly lit room | 468 | 46 % |
| LED lamp at 10 cm | 1020 | 100 % |

The sensor faces the room from the front of the board, at least 47 mm
from the nearest LED, so that the LEDs do not light it themselves. To make the clock brighter in a lit
room, increase `R_PT1` (1 MΩ); to make it dimmer, decrease it (220 kΩ).
The brightness curve itself is set in the firmware
([firmware.md](firmware.md#dimming)).

## DCF77 receiver

| Ref | Part | Role |
|-----|------|------|
| `MOD_DCF1` | DCF-3850M-800 module with ferrite antenna | 77.5 kHz receiver, 3.3 V |
| `U_LDO1` | MCP1700-3302E | 3.3 V regulator for the module |
| `C_LDO_IN1`, `C_LDO_OUT1` | 1 µF ceramic | regulator input and output capacitors |
| `R_DCF_VCC1`, `C_DCF_VCC1` | 100 Ω, 100 nF | supply filter of the module (16 kHz) |
| `Q_DCF1` | 2N3904 | level shifter (inverter) |
| `R_DCF_OUT1` | 10 kΩ | base resistor |
| `R_DCF_C1` | 10 kΩ | collector pull-up to `+5V` |

```
 +5V ── U_LDO1 ── DCF_3V3 ── R_DCF_VCC1 ──┬── VCC (2)  MOD_DCF1
                                          C_DCF_VCC1   GND (1) ── GND
                                          GND          ~PON (4) ── GND
                                                       OUT (3) ── R_DCF_OUT1 ── base Q_DCF1
                                                                  emitter ── GND
 +5V ── R_DCF_C1 ──┬── collector Q_DCF1
                   └── DCF77_CLEAN ── PD3 (INT1)
```

The module must run from 3.3 V (85 µA maximum). Its output is
push-pull, idle LOW, with HIGH pulses of 100 ms (bit 0) and 200 ms
(bit 1), and no pulse at second 59. A 3.3 V signal would leave almost no
margin over the input threshold of the ATmega328P powered at 4.7 V, so
`Q_DCF1` turns it into a clean 0/5 V signal on PD3, inverted: idle HIGH,
pulses LOW. `~PON` is active LOW and tied to GND; left open, the module
chops its output.

Module pins: 1 GND, 2 VCC, 3 OUT, 4 ~PON. The module is soldered flat
onto the board by its castellated edge. Seen from the front, the pads of
the footprint are, from left to right: VCC, GND (the rectangular pad 1),
OUT, ~PON. Check the labels of your module against this order before
soldering.

Reception is the weak point of any DCF77 clock. The module sits at the
top of the board, as far as possible from the power input, and the
ferrite antenna is held by two cable ties through `MH5`–`MH8` over an area
without copper. On the board:

- a keep-out zone (`DCF77 module keep-out`) forbids tracks, vias and copper
  under the module;
- a second zone (`DCF77 antenna no-copper`) keeps the ground pours away
  from the antenna: a conductive plane close to the ferrite rod damps it;
- the decoder ([dcf77-decoder.md](dcf77-decoder.md)) is designed for the
  spurious pulses that remain.

Orient the clock so that the antenna rod is horizontal and perpendicular
to the direction of the transmitter (Mainflingen, near Frankfurt,
Germany), away from switching power supplies, screens and computers.

## Buttons and connectors

| Ref | Label | Pin | Function |
|-----|-------|-----|----------|
| `SW1` | H+1 | PD4 | hours +1 |
| `SW2` | M+1 | PC3 | minutes +1, seconds to 0 |
| `SW3` | S=0 | PD6 | seconds to 0 |

The buttons short their pin to GND and use the internal pull-ups; the
firmware debounces them.

`J_ISP1`, 2 × 3 header, standard AVR ISP pinout (top view):

| Pin | Signal | Pin | Signal |
|----:|--------|----:|--------|
| 1 | MISO | 2 | VCC (`+5V`) |
| 3 | SCK | 4 | MOSI |
| 5 | ~RESET | 6 | GND |

`J_DBG1`, 1 × 3 header for a USB-serial adapter with 5 V logic levels
(or a 3.3 V adapter whose RX input is 5 V tolerant): 1 GND, 2 RX (PD0,
unused), 3 TX (PD1). Only TX is used: connect it to the RX of the
adapter.

## PCB

- Round, Ø 180 mm, two layers, FR-4 1.6 mm, ground pour on both sides.
- All parts on the front: the circuit is the face of the clock. They are
  through-hole parts, except the DCF77 module, soldered flat by its
  castellated edge. The back is flat, for mounting against a wall.
- Tracks 0.3 mm, 0.6 mm for most of the `+5V` rail, clearance 0.2 mm, vias
  0.8 / 0.4 mm.
- `MH1`–`MH4`: M3 mounting holes on a 120 × 100 mm rectangle centered on the
  board. `MH5`–`MH8`: Ø 2.2 mm holes for the antenna cable ties.
- `H_CABLE1`: Ø 5.5 mm hole for the power cable, 80 mm below and 13 mm to
  the right of the center of the board.
- Silkscreen: row labels, bit weights, button functions, connector names,
  and the values of the parts that are easy to mix up at assembly: the two
  TO-92 parts (2N3904 and MCP1700), the crystal and regulator capacitors,
  the resistor rows of each LED color.

### Design rules

- Every unused CMOS input is tied to a rail; unused outputs are left open.
- Every IC has a 100 nF X7R decoupling capacitor close to its supply pins
  (`C_DEC_U3`–`C_DEC_U5`, `C_DEC_U11`–`C_DEC_U13`, `C_DEC_MCU_VCC1`).

## Datasheets

- [ATmega328P](https://www.microchip.com/en-us/product/ATmega328P) (Microchip), sections
  *Asynchronous Operation of Timer/Counter2*, *16-bit Timer/Counter1*,
  *Analog-to-Digital Converter*, *Power Management and Sleep Modes*
- [CD4040B](https://www.ti.com/product/CD4040B) and [SN74HC245](https://www.ti.com/product/SN74HC245) (Texas Instruments)
- [TEPT5700](https://www.vishay.com/docs/81321/tept5700.pdf) (Vishay)
- [MCP1700](https://www.microchip.com/en-us/product/MCP1700) (Microchip)
- Microchip application notes AVR042, *AVR Hardware Design Considerations*, and AVR134,
  *Real Time Clock (RTC) Using the Asynchronous Timer*
