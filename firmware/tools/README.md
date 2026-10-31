# Bring-up tools

| Tool | Runs on | Purpose |
|------|---------|---------|
| [`blink_test`](blink_test) | clock MCU | checks that the chip runs from its 8 MHz internal oscillator: 1 Hz on PB0, 1 kHz on PB1 and PB3 (`J_ISP1` pin 4) |
| [`crystal_test`](crystal_test) | clock MCU | checks the 32.768 kHz crystal: 16.384 kHz on PB3 (`J_ISP1` pin 4) |
| [`arduino_isp_clock`](arduino_isp_clock) | Arduino Uno | ArduinoISP with an 8 MHz clock on D9, to recover a chip whose fuses select an external crystal |
| [`dcf77_simulator`](dcf77_simulator) | Arduino Uno | emulates a DCF77 receiver on D8, to test the firmware on a breadboard without radio reception |
| [`drift_log.py`](drift_log.py) | PC | measures the crystal drift from the debug output of the clock |

The sketches for the clock MCU are built and uploaded like the firmware:
with MiniCore and *Upload Using Programmer*, or from the command line:

```sh
make -C firmware tools
avrdude -c stk500v1 -b 19200 -P /dev/ttyACM0 -p m328p -U flash:w:firmware/build/blink_test.hex:i
```

The Uno sketches are uploaded with the standard *Arduino Uno* board. Each
file starts with its wiring and expected results. Their use during
bring-up is described in [docs/assembly.md](../../docs/assembly.md#first-power-up).

`dcf77_simulator` drives PD3 directly: use it with an ATmega328P on a
breadboard. On the assembled clock, the receiver's level shifter would
pull the same line against the Uno.
