# Building the clock

- [What you need](#what-you-need)
- [Ordering the PCB](#ordering-the-pcb)
- [Assembly](#assembly)
- [First power-up](#first-power-up)
- [Trimming the crystal](#trimming-the-crystal)
- [Mounting](#mounting)
- [Troubleshooting](#troubleshooting)

## What you need

- The PCB, made from the KiCad project (see below).
- The parts listed in [`hardware/bom.csv`](../hardware/bom.csv): 76 parts plus
  seven IC sockets, all through-hole except the DCF77 module.
- A regulated 5 V supply of 500 mA or more with a two-wire cable, for
  example a USB charger whose cable is cut.
- Four M3 standoffs about 10 mm long, for mounting, and two small cable
  ties (2 mm wide) for the antenna.
- To program the microcontroller: an Arduino Uno (or any AVR ISP
  programmer), a 10 µF capacitor and six jumper wires.
- Optional: a USB-serial adapter for the debug output, a multimeter, an
  oscilloscope.

## Ordering the PCB

Generate the Gerber and drill files from `hardware/binary_clock.kicad_pcb`
with KiCad 10: in the PCB editor, *File → Fabrication Outputs → Gerbers*
(the plot settings are stored in the board) and *Generate Drill Files*.
From the command line:

```sh
kicad-cli pcb export gerbers --board-plot-params -o hardware/production/ hardware/binary_clock.kicad_pcb
kicad-cli pcb export drill -o hardware/production/ hardware/binary_clock.kicad_pcb
```

Board parameters: 2 layers, FR-4, 1.6 mm, 1 oz copper, round Ø 180 mm,
minimum track 0.3 mm, minimum clearance 0.2 mm, smallest hole 0.4 mm.
Choose the solder mask color with care: the board is the face of the
clock.

## Assembly

Solder from the lowest parts to the highest. Every part goes on the
front side, the one with the LEDs and the labels.

1. **Resistors.** 330 Ω for the red row, 220 Ω for the green and blue
   rows (the value is printed above each row), 470 kΩ for `R_PT1`, 10 kΩ
   for `R_OE1`, `R_RST1`, `R_DCF_OUT1` and `R_DCF_C1`, 100 Ω for
   `R_DCF_VCC1`.
2. **`D_TERM1`** (band on the side marked K) and the ferrite bead
   `L_AVCC1`.
3. **Ceramic capacitors.** 15 pF for `C1` and `C2` next to the crystal,
   1 µF for `C_LDO_IN1` and `C_LDO_OUT1` next to the regulator, 100 nF
   everywhere else.
4. **`Y1`**, lying flat on its footprint. Solder it quickly: watch
   crystals do not like heat.
5. **IC sockets**, notch as printed on the board. Do not insert the ICs
   yet.
6. **`Q_DCF1` and `U_LDO1`.** The 2N3904 and the MCP1700 come in the same
   TO-92 package: read the marking before soldering. Swapping them puts
   5 V on the 3.3 V receiver. Spread the leads to the 2.54 mm pitch of the
   footprint.
7. **Headers and buttons**: `J_ISP1`, `J_DBG1`, `SW1`–`SW3`.
8. **`C_BULK1`**, positive (longer) lead in the pad marked +.
9. **`J_PWR2`**, with the wire entry facing the cable hole.
10. **LEDs.** The cathode (short lead, flat side of the rim) goes into the
    square pad. Red in the top row, green in the middle, blue at the
    bottom. A strip of cardboard under the rims keeps them all at the same
    height.
11. **`Q_PT1`.** The TEPT5700 is mounted like an LED: short lead (the
    collector) and flat side to the square pad, facing the room.
12. **`MOD_DCF1`.** Seen from the front, the footprint pads are, from left
    to right: VCC, GND (rectangular pad), OUT, PON. Check that the labels
    of your module match this order, then solder it flat by its
    castellated edge. Hold the ferrite antenna in its rectangle with two
    cable ties through `MH5`–`MH8`. The antenna wires are thin: do not bend
    them back and forth.

## First power-up

Proceed step by step, so that a fault shows up where it is easy to find.

1. **No ICs.** Connect the 5 V supply. The rail should read about 4.8 V
   between pins 2 and 6 of `J_ISP1`, and the same on every socket: pins 16
   and 8 of `U3`–`U5`, pins 20 and 10 of `U11`–`U13`, pins 7, 20 and 8, 22
   of `U_MCU1`. Pin 3 of `U_LDO1` reads 3.3 V. The board draws less than
   1 mA.
2. **Program the microcontroller** in its socket through `J_ISP1`, with
   the 5 V supply disconnected ([firmware.md](firmware.md#building-and-programming)).
   Two small sketches help when something does not work:
   [`blink_test`](../firmware/tools/blink_test) checks that the chip runs
   at 8 MHz and [`crystal_test`](../firmware/tools/crystal_test) that the
   crystal oscillates. Both output their signal on `J_ISP1` pin 4.
3. **Microcontroller only.** Flash the clock firmware built with
   `VERBOSE_TICK=1`, connect a USB-serial adapter to `J_DBG1` (9600 baud)
   and power the clock: `BOOT binary_clock` appears, then one `T` line per
   second. Pin 10 of the `U3` socket shows a 5 µs low pulse every second,
   and pin 19 of the 74HC245 sockets a 488 Hz signal whose duty cycle
   changes when you cover the light sensor.
4. **Display.** Power off, insert `U3`–`U5` (CD4040) and `U11`–`U13`
   (74HC245), notch as printed, and power on: the blue row counts the
   seconds. Set the time with the buttons.
5. **DCF77.** The receiver needs a minute or two after power-up. Then
   `SYNC wait` lines appear once per minute, followed by `SYNC hh:mm OK`,
   typically 5 to 15 minutes after power-up with good reception: the
   clock is synchronized. Keep it away from
   computers, screens and switching power supplies while testing.
6. Flash the default build (without `VERBOSE_TICK`) for everyday use, or
   keep the debug output: it does not affect the clock.

## Trimming the crystal

Optional: DCF77 corrects the time every minute, but without reception the
clock relies on its crystal (±20 ppm, up to 1.7 s per day).

1. Build the firmware with `DCF77_ENABLED=0 VERBOSE_TICK=1`, so that DCF77
   does not correct the clock during the measurement.
2. Connect a USB-serial adapter to `J_DBG1` and run, on a computer whose
   clock is synchronized over NTP and which has pyserial installed
   (`pip install pyserial`):

   ```sh
   python3 firmware/tools/drift_log.py /dev/ttyUSB0
   ```

3. After at least 6 hours, stop it with Ctrl-C. Within ±10 ppm (±0.9 s
   per day), keep 15 pF. A larger negative result means the clock runs
   slow: replace `C1` and `C2` with 10 pF. A larger positive result means
   it runs fast: use 22 pF. Each step moves the frequency by roughly 15 to
   25 ppm; measure again after the change.
4. Flash the default firmware again.

## Mounting

The power cable comes out of the wall behind the clock and passes through
`H_CABLE1`, 80 mm below and 13 mm to the right of the center of the
board: drill the wall there. Strip about 25 mm of the outer sheath, so
that only the two insulated wires make the 90° turn into `J_PWR2`, and
mind the polarity printed next to the terminal. Mount the board on four
M3 standoffs through `MH1`–`MH4`.

For the best DCF77 reception, the antenna rod should be horizontal and
perpendicular to the direction of the transmitter (Mainflingen, near
Frankfurt), away from metal and electronic devices.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Programmer: "not in sync" or "programmer is not responding" | 10 µF capacitor missing on the Uno RESET, wrong port, or programmer not set to *Arduino as ISP* |
| "Device signature = 0x000000" or "0xFFFFFF" | ISP wiring (a loose wire, MOSI and MISO swapped) or no power on the target; or fuses set for an external crystal: recover with [`arduino_isp_clock`](../firmware/tools/arduino_isp_clock) |
| The signature of another chip, such as 0x1E9514 | not an ATmega328P (0x1E9514 is an ATmega328): select the matching *Variant* in MiniCore |
| Boot takes 8 s, the LEDs flicker, the debug output is unreadable | fuses not written, the chip still runs at 1 MHz: run *Burn Bootloader* |
| Every character of the debug output is wrong | bit time: adjust `DBG_BIT_US` ([firmware.md](firmware.md#debug-output)) |
| A few wrong characters on the debug output | interrupts during transmission: harmless ([firmware.md](firmware.md#debug-output)) |
| No `BOOT` line and dark LEDs, with the firmware programmed | the firmware waits for the 32 kHz crystal at start-up: check `Y1`, `C1`, `C2` and their solder joints, run `crystal_test`; or check the adapter wiring (TX to its RX, common GND) |
| `T` lines appear but the LEDs never light | a buffer missing or reversed, or `PWM_DIM` stuck HIGH |
| LEDs count wrongly | a solder bridge or open joint on a counter output, MR or CLK line |
| Never any `SYNC` line | the grid cannot lock: check the receiver supply (3.3 V), `~PON` at GND, the orientation of the antenna and nearby noise sources; build with `DCF77_DEBUG_PULSES=1` ([dcf77-decoder.md](dcf77-decoder.md#debug-output)) |
| `SYNC wait` for a long time | weak or noisy signal: turn the clock, move it away from noise sources; in heavy noise the first time can take 30 minutes or more |
| Too bright at night, or too dim by day | `BRIGHT_MIN`, `BRIGHT_CURVE` or `R_PT1` ([firmware.md](firmware.md#dimming)) |
