# Firmware

| Directory | Content |
|-----------|---------|
| [`binary_clock/`](binary_clock) | clock firmware: `binary_clock.ino` and the DCF77 decoder `dcf77_decoder.h` |
| [`test/`](test) | PC simulator and regression tests of the decoder |
| [`tools/`](tools) | bring-up sketches and a crystal drift logger |

The firmware builds with the Arduino IDE and [MiniCore](https://github.com/MCUdude/MiniCore),
or from the command line with avr-gcc:

| Command | Effect |
|---------|--------|
| `make` | build `build/binary_clock.hex` |
| `make VERBOSE_TICK=1 DBG_BIT_US=112` | build with other options (see [docs/firmware.md](../docs/firmware.md#build-options)) |
| `make tools` | build the bring-up sketches `blink_test` and `crystal_test` |
| `make fuses PORT=/dev/ttyACM0` | write the fuses through an Arduino running ArduinoISP |
| `make flash PORT=/dev/ttyACM0` | program the firmware the same way |
| `make test` | run the decoder regression suite on the PC |

Building, programming and the firmware design are documented in
[docs/firmware.md](../docs/firmware.md).
