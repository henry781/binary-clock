# Firmware

The firmware is a single Arduino sketch, [`firmware/binary_clock/`](../firmware/binary_clock),
for the ATmega328P running from its internal 8 MHz oscillator. It uses
avr-libc only, none of the Arduino library functions, so it also builds
with plain avr-gcc.

- [Building and programming](#building-and-programming)
- [Build options](#build-options)
- [Architecture](#architecture)
- [Debug output](#debug-output)

## Building and programming

The chip is programmed over ISP through `J_ISP1`; there is no bootloader.
Any ISP programmer works. The steps below use an Arduino Uno as the
programmer.

### Programmer

1. Upload *File → Examples → 11.ArduinoISP → ArduinoISP* to the Uno
   (board *Arduino Uno*, normal *Upload*).
2. Put a 10 µF capacitor between RESET (+) and GND of the Uno. Without it
   the Uno resets when the port opens and the programmer does not answer
   ("not in sync"). Remove it to reprogram the Uno itself.
3. Wire the Uno to `J_ISP1`:

   | Uno | `J_ISP1` pin |
   |-----|--------------|
   | D12 | 1 (MISO) |
   | 5V | 2 (VCC) |
   | D13 | 3 (SCK) |
   | D11 | 4 (MOSI) |
   | D10 | 5 (~RESET) |
   | GND | 6 (GND) |

   The Uno powers the board through pin 2. Disconnect the 5 V supply of
   the clock while programming.

### Arduino IDE

1. Install [MiniCore](https://github.com/MCUdude/MiniCore): add
   `https://mcudude.github.io/MiniCore/package_MCUdude_MiniCore_index.json`
   to *Additional boards manager URLs*, then install *MiniCore* from the
   Boards Manager.
2. Open `firmware/binary_clock/binary_clock.ino` and select:

   | Tools menu | Setting |
   |------------|---------|
   | Board | MiniCore → ATmega328 |
   | Variant | 328P / 328PA |
   | Clock | **Internal 8 MHz** |
   | BOD | 2.7V |
   | EEPROM | EEPROM retained |
   | Bootloader | No bootloader |
   | Programmer | Arduino as ISP |

3. *Tools → Burn Bootloader*, once per new chip. With *No bootloader* this
   erases the chip and writes the fuses. A new chip runs at 1 MHz until
   this is done.
   The avrdude command line in the console must contain
   `-Ulfuse:w:0b11100010:m`; if it shows `0b11110111`, the *Clock* menu is
   set to an external crystal: do not proceed (a chip programmed that way
   can be recovered with [`arduino_isp_clock`](../firmware/tools/arduino_isp_clock)).
4. *Sketch → Upload Using Programmer*. The normal *Upload* button expects a
   bootloader and fails.

### Command line

With avr-gcc, avr-libc and avrdude installed:

```sh
make -C firmware                                # build firmware/build/binary_clock.hex
make -C firmware fuses PORT=/dev/ttyACM0        # once per new chip
make -C firmware flash PORT=/dev/ttyACM0
```

`PORT` is the serial port of the Arduino running ArduinoISP. Build
options are passed as make variables, for example
`make -C firmware VERBOSE_TICK=1 DBG_BIT_US=112`.

### Fuses

| Fuse | Value | Meaning |
|------|-------|---------|
| lfuse | `0xE2` | internal 8 MHz RC oscillator, no clock divider, 65 ms start-up |
| hfuse | `0xD7` | SPI programming enabled, EEPROM kept on chip erase, start at address 0 (`0xDF` works too) |
| efuse | `0xFD` | brown-out detection at 2.7 V, the minimum for 8 MHz |

## Build options

Defined at the top of `binary_clock.ino`; each can also be set on the
compiler command line.

| Option | Default | Effect |
|--------|---------|--------|
| `DCF77_ENABLED` | 1 | DCF77 synchronization |
| `DCF77_OUT_ACTIVE_LOW` | 1 | pulses are LOW on PD3, as delivered by the level shifter of the board; 0 for a receiver wired straight to PD3 |
| `DCF77_PD3_PULLUP` | 0 | internal pull-up on PD3, only for an open-collector receiver powered from 5 V |
| `DCF77_DEBUG_PULSES` | 0 | print the decoder measurements of every second |
| `DEBUG_UART_ENABLED` | 1 | debug output on PD1 |
| `VERBOSE_TICK` | 0 | print a status line every second |
| `DBG_BIT_US` | 103 | bit time of the debug output in µs, see [Debug output](#debug-output) |
| `BRIGHT_CURVE` | 1 | brightness law: 1 proportional, 2 quadratic, 0 square root |
| `BRIGHT_MIN` | 2 | minimum LED level, in 1/2048 |
| `LIGHT_FILTER_SHIFT` | 2 | light filter time constant, about 2^n seconds (1 to 6) |

## Architecture

| Source | Rate | Work |
|--------|------|------|
| `TIMER2_OVF` | 1 Hz | advances the counters (seconds → minutes → hours), applies an announced DST change or leap second, closes the current DCF77 second |
| `INT1` | each edge on PD3 | passes the DCF77 edge to the decoder |
| `PCINT2`, `PCINT1` | each button edge | schedules a debounce sample |
| `TIMER2_COMPA` | 20 ms after the last button edge | reads the buttons |
| `loop()` | after any interrupt | decodes a DCF77 frame and loads the time, updates the brightness once per second, handles a button press, prints the log |

Between interrupts the CPU sleeps in Idle mode. Power-save mode would
use less current, but it stops the I/O clock, which drives the PWM and
the edge detection of INT1; the clock runs from a mains supply and the
1.5 mA of Idle mode does not matter.

Timer0 is powered down (so `millis()` and `delay()` are not available),
Timer1 generates the PWM, Timer2 is the real-time clock. The USART, SPI,
TWI and analog comparator are powered down as well.

### Timekeeping

Timer2 runs from the 32.768 kHz crystal with a prescaler of 128: `TCNT2`
counts 256 steps per second and overflows exactly once per second. The
overflow count and `TCNT2` together form a 16-bit timestamp with a
resolution of 1/256 s (3.9 ms), used to time the DCF77 pulses.

The firmware keeps a copy of the three counters (`sw_sec`, `sw_min`,
`sw_hr`). On each tick it pulses the CLK input of the seconds counter; at
60 it resets that counter with MR and clocks the minutes counter, and so
on up to 24 hours. Pulses go directly to the port registers:

- CLK idles HIGH and pulses LOW for 5 µs (the CD4040 counts on the falling edge);
- MR idles LOW and pulses HIGH for 10 µs.

Both are 20 to 40 times longer than the CD4040 minimum. Since the CD4040
cannot be loaded in parallel, setting a counter to N takes an MR pulse
followed by N clock pulses (0.33 ms for 59), with interrupts disabled.

At power-up the firmware turns the LEDs off (`~OE` HIGH), resets the
three counters, waits one second for the 32 kHz oscillator to settle,
then starts Timer2; the first tick comes about two seconds after
power-up. The first light measurement sets the brightness before
interrupts are enabled, so the LEDs come up at the right level.

### Dimming

Timer1 runs in fast PWM mode 14 with `ICR1` = 2047 and a prescaler of 8:
8 MHz / 8 / 2048 = 488 Hz, in 2048 steps. `OC1A` (PB1) drives `~OE` of the
three buffers, so the LEDs are on during the LOW part of the period:
LED level = 2047 − `OCR1A`. The frequency never changes, only the duty
cycle. No interrupt touches Timer1.

Once per second the firmware reads the light sensor on ADC4 (AVCC
reference), smooths the reading with an exponential moving average
(time constant about 4 s, so a passing shadow does not make the display
pump) and maps it to a level between `BRIGHT_MIN` and 2047:

| `BRIGHT_CURVE` | Law | Behavior |
|----------------|-----|----------|
| 1 (default) | level = 2 × reading | constant contrast with the room |
| 2 | quadratic | darker in dim light |
| 0 | square root | brighter in dim light |

`BRIGHT_MIN` = 2 gives 0.1 % in the dark; 1 halves it, 4 to 8 make the
clock more visible at night. The sensor range itself is set by `R_PT1`
([hardware.md](hardware.md#light-sensor)).

### Buttons

The buttons use the internal pull-ups. Any edge triggers a pin-change
interrupt, which schedules a sample 20 ms later with the compare unit of
Timer2; every bounce pushes the sample back. When the sample runs, the
three inputs are read, and a button that went from released to pressed
produces one event. Presses are handled one at a time: of two buttons
pressed within the same 20 ms, only one counts. A press shorter than
20 ms is ignored, and holding a button does not repeat.

| Button | Action |
|--------|--------|
| `SW1` | hours +1 |
| `SW2` | minutes +1 and seconds to 0 |
| `SW3` | seconds to 0 |

Setting the time by hand stops the clock from applying DST changes and
leap seconds on its own until DCF77 provides the time again.

### DCF77

`INT1` timestamps every edge of the DCF77 signal and the tick closes each
second, even when the line is quiet; the decoder
([`dcf77_decoder.h`](../firmware/binary_clock/dcf77_decoder.h), described in
[dcf77-decoder.md](dcf77-decoder.md)) delivers a frame at the end of each
minute. `loop()` decodes it, and when a time is accepted it waits for the
top of the minute, loads the three counters and rewrites `TCNT2` so that
the next tick falls exactly one second after the top. The timestamps
held by the decoder are moved to the new time base at the same moment.

### Timing

The tick interrupt takes about 0.2 ms, at most 0.7 ms when it closes a
DCF77 second and evaluates the phase vote; `INT1` at most 0.5 ms. Decoding
a frame takes a few milliseconds in `loop()`. Loading the time disables
interrupts for about 0.6 ms. The default build uses 8.3 KB of flash and
721 bytes of RAM.

## Debug output

`J_DBG1` pin 3 (PD1) transmits at 9600 baud, 8N1, 5 V levels. The UART is
bit-banged: the USART is powered down, and SoftwareSerial would take over
the pin-change interrupts of the buttons. An interrupt that occurs while a
character is being sent stretches one of its bits and can garble it. Once
the clock is synchronized this happens regularly, because the lines are
printed at the start of each second, when the DCF77 pulse arrives: expect
a few damaged characters. The clock itself is not affected.

The bit time depends on the internal RC oscillator, which is
factory-calibrated to ±10 % only. If the terminal shows garbage, measure
one bit on PD1 with an oscilloscope (104 µs expected) and scale
`DBG_BIT_US` accordingly; a chip whose oscillator runs 9 % fast needs 112.
The [`blink_test`](../firmware/tools/blink_test) sketch gives the actual
oscillator frequency.

| Message | Meaning |
|---------|---------|
| `BOOT binary_clock` | power-up |
| `SYNC 14:05 OK margin 40/40 CET` | time loaded from DCF77; the margins are the score leads of the minutes and hours (40 at most) |
| `SYNC wait 14:05 margin 24/12 CET` | best estimate, not confident yet (minutes need 28, hours 32) |
| `SYNC gate 14:05 margin 40/34 CET` | confident, but different from the clock: loaded only if the next frame confirms it |
| `SYNC align` | frame rejected: two of the three checks failed (bit 0 = 0, bit 20 = 1, exactly one of bits 17 and 18 set) |
| `SYNC phase` | frame rejected: its top is not a whole number of minutes after the previous one |
| `SYNC dup` | second frame within the same minute |
| `SYNC noisy` | frame rejected: 54 of its 59 seconds or more are noisy |
| `SYNC leap` | frame of a leap-second minute ignored; the position in the minute is learned again |
| `SYNC late` | frame closed too late to load the time at the top of the minute |
| suffix `unconfirmed` | frame delivered while the phase vote no longer confirms the grid; it can only re-align a clock that already shows its time |
| `DST CET->CEST`, `DST CEST->CET` | DST change applied at the top of the hour |
| `LEAP 59 held` | leap second applied: second 59 lasted two seconds |
| `SW1 h=15`, `SW2 m=07`, `SW3 sec=0` | button presses |

`CEST` replaces `CET` in summer. With `VERBOSE_TICK=1` the firmware also
prints, every second, `T <n> hh:mm:ss L <reading> <level>`: seconds since
boot, displayed time, filtered light reading (0–1023) and LED level
(0–2047). [`tools/drift_log.py`](../firmware/tools/drift_log.py) uses
these lines to measure the crystal drift. With `DCF77_DEBUG_PULSES=1`,
the decoder measurements of each second are appended to the `T` line, or
printed on a line of their own when `VERBOSE_TICK` is 0
([dcf77-decoder.md](dcf77-decoder.md#debug-output)).
