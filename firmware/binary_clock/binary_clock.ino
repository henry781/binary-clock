/*
 * binary_clock.ino - firmware for the ATmega328P of the binary clock
 *
 * Build: Arduino IDE with MiniCore
 *   Board ATmega328, Variant 328P / 328PA, Clock Internal 8 MHz, BOD 2.7 V,
 *   Bootloader: No bootloader, Programmer: Arduino as ISP
 *   ("Burn Bootloader" once to write the fuses: lfuse 0xE2, hfuse 0xD7,
 *   efuse 0xFD; then "Upload Using Programmer").
 * or: make -C firmware   (avr-gcc only, see firmware/Makefile)
 *
 * The MCU does four things (docs/firmware.md):
 *   1. 1 Hz timebase: Timer2 in asynchronous mode on the 32.768 kHz crystal,
 *      driving the CLK and MR inputs of the three CD4040 display counters;
 *   2. dimming: ambient light measured once per second on ADC4 (TEPT5700 +
 *      R_PT1), 488 Hz hardware PWM on PB1/OC1A driving ~OE of the 74HC245
 *      LED buffers;
 *   3. DCF77 decoding on INT1 (dcf77_decoder.h, docs/dcf77-decoder.md);
 *   4. three buttons, debounced by deferred sampling.
 *
 * The CPU sleeps in IDLE, not POWER_SAVE: power-save stops the I/O clock,
 * which stops the Timer1 PWM and leaves INT1 able to wake on low level only.
 *
 * Timer0 is stopped and powered down, so millis(), delay() and tone() of the
 * Arduino core are unavailable; only the busy-wait _delay_us()/_delay_ms()
 * (F_CPU = 8 MHz) are used.
 *
 * Pin numbers in comments are DIP-28 package pins.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include <util/atomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------
// Build options (each can be overridden on the compiler command line)
// ---------------------------------------------------------------------
#ifndef DCF77_ENABLED
#define DCF77_ENABLED        1  // DCF77 synchronization
#endif
#ifndef DCF77_OUT_ACTIVE_LOW
#define DCF77_OUT_ACTIVE_LOW 1  // 1: PD3 idles HIGH and pulses LOW. The receiver outputs HIGH
                                //    pulses at 3.3 V; the Q_DCF1 inverter turns them into
                                //    0/5 V LOW pulses on PD3.
                                // 0: receiver output wired straight to PD3, no inverter
#endif
#ifndef DCF77_PD3_PULLUP
#define DCF77_PD3_PULLUP     0  // 1 only for an open-collector receiver powered from 5 V
                                // (a pull-up to 5 V would drive current into a 3.3 V output)
#endif
#ifndef DCF77_DEBUG_PULSES
#define DCF77_DEBUG_PULSES   0  // 1: print each decoded DCF77 second (docs/dcf77-decoder.md)
#endif
#ifndef DEBUG_UART_ENABLED
#define DEBUG_UART_ENABLED   1  // bit-banged TX on PD1, 9600 baud
#endif
#ifndef VERBOSE_TICK
#define VERBOSE_TICK         0  // 1: print "T <n> hh:mm:ss L <adc> <level>" every second
                                // (drift measurement, dimming curve)
#endif

// Dimming (docs/hardware.md): the LED level is the number of steps, out of
// 2048, during which the LEDs are enabled (duty cycle = level / 2048).
#define PWM_TOP              2047 // Timer1 mode 14 (TOP = ICR1), /8: 8 MHz / 8 / 2048 = 488 Hz
#ifndef BRIGHT_CURVE
#define BRIGHT_CURVE         1    // 1: level proportional to light (constant contrast with
                                  //    the room); 2: quadratic, darker at dusk;
                                  // 0: square root, brighter at dusk
#endif
#ifndef BRIGHT_MIN
#define BRIGHT_MIN           2    // floor in the dark: 2/2048 = 0.1 %
#endif
#ifndef LIGHT_FILTER_SHIFT
#define LIGHT_FILTER_SHIFT   2    // moving average: new = old + (sample - old) / 4,
                                  // time constant about 4 s at one sample per second
#endif
#if LIGHT_FILTER_SHIFT < 1 || LIGHT_FILTER_SHIFT > 6
#error "LIGHT_FILTER_SHIFT must be between 1 and 6"
#endif

// ---------------------------------------------------------------------
// Pin mapping (docs/hardware.md)
// ---------------------------------------------------------------------
// Port B
#define PIN_CLK_1HZ   PB0  // 14 -> U3 CLK (seconds counter)
#define PIN_PWM       PB1  // 15 OC1A -> PWM_DIM: ~OE of the 74HC245 buffers
#define PIN_CLK_1HR   PB2  // 16 -> U5 CLK (hours counter)
// PB3/4/5 MOSI/MISO/SCK: ISP only, inputs with pull-up while running
// PB6/7 TOSC1/TOSC2: crystal Y1, owned by asynchronous Timer2

// Port C
#define PIN_MR_S      PC0  // 23 -> U3 MR
#define PIN_MR_M      PC1  // 24 -> U4 MR
#define PIN_MR_H      PC2  // 25 -> U5 MR
#define PIN_SW2       PC3  // 26 button minutes +1 (internal pull-up, PCINT11)
#define PIN_LIGHT     PC4  // 27 ADC4 <- TEPT5700 / R_PT1 (LIGHT)
// PC5: unused, output LOW
#define ADC_CH_LIGHT  4

// Port D
#define PIN_DBG_RX    PD0  //  2 debug RX (unused: input with pull-up)
#define PIN_DBG_TX    PD1  //  3 debug TX (bit-banged, 9600 baud)
// PD2: unused, output LOW
#define PIN_DCF_IN    PD3  //  5 INT1 <- DCF77_CLEAN
#define PIN_SW1       PD4  //  6 button hours +1 (internal pull-up, PCINT20)
// PD5: unused, output LOW. Next to TOSC2 (pin 10): nothing may switch here.
#define PIN_SW3       PD6  // 12 button seconds reset (internal pull-up, PCINT22)
#define PIN_CLK_1MIN  PD7  // 13 -> U4 CLK (minutes counter)

// ---------------------------------------------------------------------
// Direct port access
// ---------------------------------------------------------------------
#define SET_B(p)  (PORTB |=  _BV(p))
#define CLR_B(p)  (PORTB &= ~_BV(p))
#define SET_C(p)  (PORTC |=  _BV(p))
#define CLR_C(p)  (PORTC &= ~_BV(p))
#define SET_D(p)  (PORTD |=  _BV(p))
#define CLR_D(p)  (PORTD &= ~_BV(p))
#define GET_D(p)  ((PIND & _BV(p)) != 0)

// ---------------------------------------------------------------------
// Clock state: software mirror of the three CD4040 counters
// ---------------------------------------------------------------------
static volatile uint8_t sw_sec = 0;
static volatile uint8_t sw_min = 0;
static volatile uint8_t sw_hr  = 0;

// Free-running count of Timer2 overflows (never reset): high byte of the
// 16-bit timestamps in ticks of 1/256 s. Unlike sw_sec, it does not jump
// at the top of the minute or when the time is set.
static volatile uint8_t ovf_count = 0;

// 16-bit timestamp in ticks of 1/256 s (3.9 ms). Call with interrupts
// disabled (ISR or ATOMIC block). If an overflow is pending (TOV2 set and
// TCNT2 just wrapped), ovf_count is not updated yet and is corrected here.
// TCNT2 and TOV2 go through two different synchronizers (datasheet section
// 17.9): TCNT2 can already read 0 while TOV2 is not visible yet, which would
// move the timestamp back by one turn and make the decoder lose its grid.
// Time never goes backwards: a step back is that turn not yet counted.
static volatile uint16_t now_last = 0;     // last timestamp returned
static inline uint16_t now_ticks(void)
{
  uint8_t hi = ovf_count;
  uint8_t lo = TCNT2;
  if ((TIFR2 & _BV(TOV2)) && lo < 2) hi++;
  uint16_t t = ((uint16_t)hi << 8) | lo;
  if ((int16_t)(t - now_last) < 0) t += 256;
  now_last = t;
  return t;
}

// Second tick -> loop() (light measurement, log)
static volatile bool     flag_tick  = false;
// Seconds since boot: "T" log line and minutes elapsed between DCF77 frames
static volatile uint32_t tick_total = 0;

// Dimming
static uint16_t light_acc  = 0;     // 4 x filtered sample: two fraction bits, no dead band
static uint16_t light_filt = 0;     // filtered ADC sample (0..1023)
static uint16_t bright     = 0;     // current level, 0 (off) .. PWM_TOP

// Buttons: debounce by deferred sampling. Every edge on a button (PCINT)
// schedules a Timer2 compare match (OCR2A) DEBOUNCE_TICKS later; further
// bounces push the sample back. When it fires, the three pins are read
// again and a button that went HIGH -> LOW produces an event. The software
// state always equals the actual pin state; a single event is pending at a
// time, so of two buttons pressed within the same sample only one counts.
#define DEBOUNCE_TICKS 5  // about 20 ms at 256 Hz

static volatile bool     flag_button_event = false;
static volatile uint8_t  pending_button    = 0;
static volatile bool     btn_state[3]      = { false, false, false };

#if DCF77_ENABLED
// DCF77: noise-tolerant decoder in dcf77_decoder.h, shared with the PC
// simulator in firmware/test/.
#if DCF77_DEBUG_PULSES
// Last closed second and decoder events, captured in ISR context by the
// decoder hooks and printed outside the ISR by dcf_debug_print().
static volatile bool     dcf_dbg_sec    = false;  // a second has just been closed
static volatile uint8_t  dcf_dbg_a = 0, dcf_dbg_b = 0, dcf_dbg_bw = 0, dcf_dbg_c = 0, dcf_dbg_code = 0;
static volatile uint8_t  dcf_dbg_pos    = 255;    // 255: position unknown
static volatile bool     dcf_dbg_marker = false;  // top of the minute seen
static volatile int8_t   dcf_dbg_lock   = -1;     // 1 grid locked, 2 recentered, 0 dropped, -1 none
static volatile bool     dcf_dbg_acq    = false;  // phase vote evaluated
static volatile uint8_t  dcf_dbg_votes[2];        // best triplet / best disjoint triplet
#define DCF_HOOK_SECOND(code, pos) do { dcf_dbg_a = dcf_a; dcf_dbg_b = dcf_b;         \
                                         dcf_dbg_bw = dcf_bw; dcf_dbg_c = dcf_c;       \
                                         dcf_dbg_code = (code); dcf_dbg_pos = (pos); \
                                         dcf_dbg_sec = true; } while (0)
#define DCF_HOOK_MARKER()          (dcf_dbg_marker = true)
#define DCF_HOOK_LOCK(l)           (dcf_dbg_lock = (l))
#define DCF_HOOK_ACQ(b, o)         do { dcf_dbg_votes[0] = (b); dcf_dbg_votes[1] = (o); \
                                         dcf_dbg_acq = true; } while (0)
#endif
#include "dcf77_decoder.h"

// DST change or leap second applied by the tick ISR (logged by loop()):
// 1 CET -> CEST, 2 CEST -> CET, 3 leap second, 0 none
static volatile uint8_t  clock_event       = 0;
#endif

// ---------------------------------------------------------------------
// CLK / MR pulses to the CD4040 counters
// ---------------------------------------------------------------------
// CLK: idle HIGH -> falling edge (counts) -> 5 us LOW -> back HIGH.
//      The CD4040 counts on the falling edge; t_w >= 250 ns, 20x margin.
static inline void pulse_clk_s(void) { CLR_B(PIN_CLK_1HZ);  _delay_us(5); SET_B(PIN_CLK_1HZ);  }
static inline void pulse_clk_m(void) { CLR_D(PIN_CLK_1MIN); _delay_us(5); SET_D(PIN_CLK_1MIN); }
static inline void pulse_clk_h(void) { CLR_B(PIN_CLK_1HR);  _delay_us(5); SET_B(PIN_CLK_1HR);  }

// MR: idle LOW -> 10 us HIGH -> LOW. The CD4040 reset is active HIGH.
static inline void pulse_mr_s(void)  { SET_C(PIN_MR_S); _delay_us(10); CLR_C(PIN_MR_S); }
static inline void pulse_mr_m(void)  { SET_C(PIN_MR_M); _delay_us(10); CLR_C(PIN_MR_M); }
static inline void pulse_mr_h(void)  { SET_C(PIN_MR_H); _delay_us(10); CLR_C(PIN_MR_H); }

// Load a CD4040 with the value N: MR, then N CLK pulses (about 330 us for
// N = 59). The counter has no parallel load input.
static void load_counter_m(uint8_t n) { pulse_mr_m(); for (uint8_t i = 0; i < n; i++) pulse_clk_m(); }
static void load_counter_h(uint8_t n) { pulse_mr_h(); for (uint8_t i = 0; i < n; i++) pulse_clk_h(); }

// ---------------------------------------------------------------------
// Debug UART: TX only, bit-banged at 9600 baud (F_CPU = 8 MHz)
// ---------------------------------------------------------------------
// Not SoftwareSerial: its receiver would take the PCINT vectors used by
// the buttons.
#if DEBUG_UART_ENABLED
// One bit lasts 104.2 us at 9600 baud; DBG_BIT_US also absorbs the loop
// overhead (about 1 us). The internal RC oscillator is factory-calibrated
// to +/-10 % only: measure one bit on PD1 and scale this value if the
// terminal shows garbage (a chip running 9 % fast needs 112).
#ifndef DBG_BIT_US
#define DBG_BIT_US 103
#endif

static void dbg_putc(char c)
{
  // Start bit (LOW)
  CLR_D(PIN_DBG_TX); _delay_us(DBG_BIT_US);
  // 8 data bits, LSB first
  for (uint8_t i = 0; i < 8; i++) {
    if (c & 1) SET_D(PIN_DBG_TX); else CLR_D(PIN_DBG_TX);
    _delay_us(DBG_BIT_US);
    c >>= 1;
  }
  // Stop bit (HIGH)
  SET_D(PIN_DBG_TX); _delay_us(DBG_BIT_US);
}

static void dbg_puts(const char *s) { while (*s) dbg_putc(*s++); }

// Two digits with a leading zero ("07"), for hh:mm:ss
static void dbg_put_2d(uint8_t v)
{
  dbg_putc('0' + (v / 10) % 10);
  dbg_putc('0' + v % 10);
}

#if VERBOSE_TICK || (DCF77_ENABLED && DCF77_DEBUG_PULSES)
static void dbg_put_u32(uint32_t v)
{
  char buf[11]; uint8_t i = 10; buf[10] = 0;
  do { buf[--i] = '0' + (v % 10); v /= 10; } while (v && i);
  dbg_puts(buf + i);
}
#endif

#if VERBOSE_TICK
static void dbg_put_hms(uint8_t h, uint8_t m, uint8_t s)
{
  dbg_put_2d(h); dbg_putc(':'); dbg_put_2d(m); dbg_putc(':'); dbg_put_2d(s);
}
#endif
#else
static inline void dbg_putc(char)                          {}
static inline void dbg_puts(const char *)                  {}
static inline void dbg_put_2d(uint8_t)                     {}
static inline void dbg_put_u32(uint32_t)                   {}
static inline void dbg_put_hms(uint8_t, uint8_t, uint8_t)  {}
#endif

// ---------------------------------------------------------------------
// Initialization: GPIO, peripherals, ADC, Timer1 PWM, Timer2, interrupts
// ---------------------------------------------------------------------
static void init_gpio(void)
{
  // Port B: CLK lines are outputs idling HIGH (the CD4040 counts on the
  // falling edge); PB1 (PWM) is an output driven HIGH until Timer1 takes
  // over (~OE HIGH = LEDs off during start-up).
  // PB3/4/5 (ISP): inputs with pull-up, never floating; the pull-ups do not
  // disturb the programmer.
  // PB6/7 (TOSC): owned by asynchronous Timer2, DDR/PORT left alone.
  DDRB  = _BV(PIN_CLK_1HZ) | _BV(PIN_PWM) | _BV(PIN_CLK_1HR);
  PORTB = _BV(PIN_CLK_1HZ) | _BV(PIN_PWM) | _BV(PIN_CLK_1HR)
        | _BV(PB3) | _BV(PB4) | _BV(PB5);

  // Port C: MR lines are outputs idling LOW; PC3 = SW2 input with pull-up;
  // PC4 = LIGHT analog input (no pull-up); PC5 unused, output LOW.
  DDRC  = _BV(PIN_MR_S) | _BV(PIN_MR_M) | _BV(PIN_MR_H) | _BV(PC5);
  PORTC = _BV(PIN_SW2);

  // Port D:
  //   PD0 RX       : input with pull-up (unused, must not float)
  //   PD1 TX       : output, idle HIGH (UART idle level)
  //   PD2          : unused, output LOW
  //   PD3 DCF_IN   : input, pull-up per DCF77_PD3_PULLUP
  //   PD4/PD6 SW*  : inputs, internal pull-up
  //   PD5          : unused, output LOW (next to TOSC2: never switches)
  //   PD7 CLK_1MIN : output, idle HIGH (the CD4040 counts on the falling edge)
  DDRD  = _BV(PIN_DBG_TX) | _BV(PIN_CLK_1MIN) | _BV(PD2) | _BV(PD5);
  PORTD = _BV(PIN_DBG_TX) | _BV(PIN_CLK_1MIN) | _BV(PIN_DBG_RX)
        | _BV(PIN_SW1) | _BV(PIN_SW3)
#if DCF77_PD3_PULLUP
        | _BV(PIN_DCF_IN)
#endif
        ;
}

static void init_peripherals(void)
{
  // Pull-ups globally allowed (PUD = 0); they are chosen pin by pin.
  MCUCR &= ~_BV(PUD);

  // Digital input buffers off on the analog input (ADC4) and on the unused
  // PC5. PC3 (SW2) stays digital.
  DIDR0 = _BV(ADC4D) | _BV(ADC5D);

  ACSR |= _BV(ACD);       // analog comparator off

  // Timer0: started by the Arduino core for millis(); stop it, then PRR.
  TIMSK0 = 0;
  TCCR0A = 0;
  TCCR0B = 0;
  // PRR: power down USART, SPI, TWI and Timer0. Timer1 (PWM), Timer2
  // (timebase) and the ADC (light sensor) stay powered.
  PRR = _BV(PRUSART0) | _BV(PRSPI) | _BV(PRTWI) | _BV(PRTIM0);

  // ADC: AVCC reference, channel ADC4, clock 8 MHz / 64 = 125 kHz
  ADMUX  = _BV(REFS0) | ADC_CH_LIGHT;
  ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1);
  ADCSRB = 0;

  // Timer1: Fast PWM, TOP = ICR1 = 2047 (mode 14), prescaler /8
  //   f = 8 MHz / 8 / 2048 = 488 Hz, 2048 steps (0.05 % each).
  //   OC1A (PB1) non-inverting: HIGH from 0 to OCR1A, LOW after.
  //   ~OE is active LOW, so the LEDs are enabled during the LOW part, i.e.
  //   (PWM_TOP - OCR1A) steps out of 2048. OCR1A = PWM_TOP keeps the output
  //   HIGH: LEDs off. No ISR touches Timer1, so the 16-bit writes from
  //   loop() need no ATOMIC block.
  TIMSK1 = 0;
  ICR1   = PWM_TOP;
  OCR1A  = PWM_TOP;
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS11);
}

static void reset_counters_hw(void)
{
  // Explicit MR pulse on the three CD4040 so that they start at 0 whatever
  // state they powered up in.
  pulse_mr_s();
  pulse_mr_m();
  pulse_mr_h();
}

static void init_timer2_async(void)
{
  // Sequence from the datasheet, "Asynchronous Operation of Timer/Counter2"
  TIMSK2 = 0;

  ASSR = _BV(AS2);                  // Timer2 clock = TOSC1/TOSC2 (Y1)

  // The 32 kHz oscillator needs up to 1 s to start and settle ("wait at
  // least one second before using Timer/Counter2 after power-up"). Wait
  // before clearing the counter, otherwise the first tick lands anywhere.
  _delay_ms(1000);

  TCNT2  = 0;
  OCR2A  = 0;
  TCCR2A = 0;                        // normal mode
  TCCR2B = _BV(CS22) | _BV(CS20);   // prescaler /128

  // 32 768 Hz / 128 / 256 (overflow) = exactly 1 Hz

  // Wait for the asynchronous update to complete
  while (ASSR & (_BV(TCN2UB) | _BV(OCR2AUB) | _BV(OCR2BUB)
               | _BV(TCR2AUB) | _BV(TCR2BUB)))
    ;

  TIFR2  = _BV(TOV2) | _BV(OCF2A);
  TIMSK2 = _BV(TOIE2);              // overflow interrupt (OCIE2A is armed on demand)
}

static void init_interrupts(void)
{
#if DCF77_ENABLED
  // INT1 on any logical change of PD3 (DCF77_CLEAN). Edge detection needs
  // the I/O clock, hence IDLE sleep only.
  EICRA = (EICRA & ~(_BV(ISC11) | _BV(ISC10))) | _BV(ISC10);
  EIFR  = _BV(INTF1);
  EIMSK = _BV(INT1);
#else
  EIMSK = 0;
#endif

  // Buttons: PD4 (SW1) = PCINT20, PD6 (SW3) = PCINT22 -> PCINT2_vect;
  //          PC3 (SW2) = PCINT11 -> PCINT1_vect
  PCMSK2 = _BV(PCINT20) | _BV(PCINT22);
  PCMSK1 = _BV(PCINT11);
  PCIFR  = _BV(PCIF2) | _BV(PCIF1);
  PCICR  = _BV(PCIE2) | _BV(PCIE1);
}

// ---------------------------------------------------------------------
// Dimming: light measurement and PWM level
// ---------------------------------------------------------------------
static uint16_t adc_read_light(void)
{
  ADCSRA |= _BV(ADSC);
  while (ADCSRA & _BV(ADSC))        // 13 ADC cycles, about 104 us at 125 kHz
    ;
  return ADC;
}

#if BRIGHT_CURVE == 0
// Integer square root (0..65535 -> 0..255), 8 iterations
static uint8_t isqrt16(uint16_t v)
{
  uint16_t res = 0, bit = 1u << 14;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
    else                { res >>= 1; }
    bit >>= 2;
  }
  return (uint8_t)res;
}
#endif

// Level BRIGHT_MIN..PWM_TOP from the filtered sample 0..1023. The sample is
// proportional to the room light up to saturation (daylight); a
// proportional level keeps the LEDs in the same ratio to their
// surroundings. With R_PT1 = 470 kOhm: darkness 0 -> 0.1 %, office tube at
// 1 m 29 -> 2.8 %, lit room 468 -> 46 %, lamp nearby 1020 -> 100 %.
// To shift the curve: R_PT1, BRIGHT_CURVE, BRIGHT_MIN.
static uint16_t brightness_from_light(uint16_t adc)
{
#if BRIGHT_CURVE == 0
  uint16_t lvl = (uint16_t)isqrt16((uint16_t)(adc * 64u)) << 3;   // 255 * 8 = 2040
#elif BRIGHT_CURVE == 2
  uint16_t lvl = (uint16_t)(((uint32_t)adc * adc + 256u) >> 9);   // 1023^2 / 512 = 2044
#else
  uint16_t lvl = (uint16_t)((adc << 1) | (adc >> 9));             // 1023 -> 2047
#endif
  if (lvl < BRIGHT_MIN) lvl = BRIGHT_MIN;
  if (lvl > PWM_TOP)    lvl = PWM_TOP;
  return lvl;
}

static void update_dimming(void)
{
  uint16_t adc = adc_read_light();
  // Exponential moving average (time constant about 4 s): passing shadows
  // do not make the brightness pump. The average is kept at four times the
  // sample: with plain integers, differences below 4 would be lost and the
  // average could stay up to 3 below the true value, in dim light where
  // the level follows the sample closely.
  light_acc += adc - ((light_acc + (1u << (LIGHT_FILTER_SHIFT - 1))) >> LIGHT_FILTER_SHIFT);
  light_filt = (light_acc + (1u << (LIGHT_FILTER_SHIFT - 1))) >> LIGHT_FILTER_SHIFT;
  bright = brightness_from_light(light_filt);
  OCR1A  = PWM_TOP - bright;
}

// ---------------------------------------------------------------------
// Interrupt service routines
// ---------------------------------------------------------------------

// Timer2 overflow: 1 Hz tick, cascade seconds -> minutes -> hours
ISR(TIMER2_OVF_vect)
{
  ovf_count++;
  flag_tick = true;
  tick_total++;
#if DCF77_ENABLED
  now_last = (uint16_t)ovf_count << 8;  // exact time of the overflow (TCNT2 = 0)
  dcf_poll(now_last);             // closes the DCF77 second even when the line is quiet
  // Announced leap second (dcf_leap_due): second 59 of 00:59 CET
  // (01:59 CEST) lasts two seconds.
  if (sw_sec == 59 && sw_min == 59 && dcf_leap_due(sw_hr)) { clock_event = 3; return; }
#endif

  sw_sec++;
  if (sw_sec < 60) {
    pulse_clk_s();
    return;
  }
  sw_sec = 0;
  pulse_mr_s();

  sw_min++;
  if (sw_min < 60) {
    pulse_clk_m();
    return;
  }
  sw_min = 0;
  pulse_mr_m();

  sw_hr++;
#if DCF77_ENABLED
  // Announced DST change (dcf_dst_due), applied at the top of the hour even
  // without a readable frame: 02:00 CET becomes 03:00 CEST (one more hour
  // pulse), 03:00 CEST goes back to 02:00 CET (hours counter unchanged).
  {
    int8_t dst = dcf_dst_due(sw_hr);
    if (dst < 0) { sw_hr--; clock_event = 2; return; }
    if (dst > 0) { sw_hr++; pulse_clk_h(); clock_event = 1; }
  }
#endif
  if (sw_hr < 24) {
    pulse_clk_h();
    return;
  }
  sw_hr = 0;
  pulse_mr_h();
}

#if DCF77_ENABLED
// INT1: every edge of the DCF77 line goes to the decoder.
ISR(INT1_vect)
{
  uint16_t now         = now_ticks();
  bool     pin_is_high = GET_D(PIN_DCF_IN);
#if DCF77_OUT_ACTIVE_LOW
  dcf_edge(now, !pin_is_high);         // LOW = pulse
#else
  dcf_edge(now,  pin_is_high);
#endif
}
#endif  // DCF77_ENABLED

// Edge on a button: (re)schedule the deferred sample
static inline void arm_button_sample(void)
{
  while (ASSR & _BV(OCR2AUB))      // previous write still in flight (<= 2 TOSC cycles)
    ;
  OCR2A  = (uint8_t)(TCNT2 + DEBOUNCE_TICKS);   // modulo 256
  TIFR2  = _BV(OCF2A);
  TIMSK2 |= _BV(OCIE2A);
}
ISR(PCINT2_vect) { arm_button_sample(); }   // SW1 (PD4), SW3 (PD6)
ISR(PCINT1_vect) { arm_button_sample(); }   // SW2 (PC3)

// Timer2 compare A: 20 ms after the last edge, read the buttons again
ISR(TIMER2_COMPA_vect)
{
  TIMSK2 &= ~_BV(OCIE2A);
  uint8_t pd = PIND, pc = PINC;
  bool pressed[3] = {
    (pd & _BV(PIN_SW1)) == 0,    // pull-up: pressed = LOW
    (pc & _BV(PIN_SW2)) == 0,
    (pd & _BV(PIN_SW3)) == 0,
  };
  for (uint8_t b = 0; b < 3; b++) {
    if (pressed[b] != btn_state[b]) {
      btn_state[b] = pressed[b];
      if (pressed[b]) {
        flag_button_event = true;
        pending_button    = b;
      }
    }
  }
}

// ---------------------------------------------------------------------
// Handlers run from loop()
// ---------------------------------------------------------------------
static void handle_buttons(void)
{
  uint8_t b;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    b = pending_button;
    flag_button_event = false;
  }

  // Critical section: software mirror and CMOS counters updated together.
  // A Timer2 overflow in the middle of an MR + N x CLK sequence would add
  // a stray clock pulse to a counter being reloaded.
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    switch (b) {
      case 0:  // SW1: hours +1
        sw_hr = (uint8_t)((sw_hr + 1) % 24);
        load_counter_h(sw_hr);
        break;
      case 1:  // SW2: minutes +1 (also resets the seconds)
        sw_min = (uint8_t)((sw_min + 1) % 60);
        sw_sec = 0;
        load_counter_m(sw_min);
        pulse_mr_s();
        break;
      case 2:  // SW3: seconds reset
        sw_sec = 0;
        pulse_mr_s();
        break;
    }
#if DCF77_ENABLED
    // Time set by hand: no longer in step with DCF77. No autonomous DST
    // change or leap second, and no gate exception until the next accepted
    // DCF77 time.
    dcf_gate_zone = 0xFF;
    dcf_dst_arm   = 0;
    dcf_leap_arm  = false;
#endif
  }

  // Logged outside the atomic block (about 1 ms per character)
  switch (b) {
    case 0: dbg_puts("SW1 h=");  dbg_put_2d(sw_hr);  dbg_putc('\n'); break;
    case 1: dbg_puts("SW2 m=");  dbg_put_2d(sw_min); dbg_putc('\n'); break;
    case 2: dbg_puts("SW3 sec=0\n");                                 break;
  }
}

#if DCF77_ENABLED
// docs/dcf77-decoder.md: every frame adds to the scores of the 60 possible
// minutes and 48 possible hours (24 x time zone); the time is only loaded
// once both fields stand out clearly.
static void decode_and_load_dcf77_frame(void)
{
  const volatile uint8_t *f = dcf_frame_done;
  uint32_t now_sec;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { now_sec = tick_total; }
  uint8_t hours = 0, minutes = 0;
  const char *why = dcf_accumulate(f, now_sec, &hours, &minutes);
  if (!why) {
    // Gate: a time different from the clock is only loaded once a second
    // confident frame confirms it (dcf_gate). At the top of a well-placed
    // frame, a clock already in step reads sw_sec = 59 (top imminent) or 0,
    // or 1 if it runs one second fast (dcf_clock_hm); otherwise the frame
    // cannot be "equal" to it.
    int clock_hm;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { clock_hm = dcf_clock_hm(sw_hr, sw_min, sw_sec); }
    why = dcf_gate(hours, minutes, clock_hm);
  }
  if (why) {
    dbg_puts("SYNC "); dbg_puts(why);
    if (!strcmp(why, "wait") || !strcmp(why, "gate")) {
      dbg_putc(' '); dbg_put_2d(hours); dbg_putc(':'); dbg_put_2d(minutes);
      dbg_puts(" margin "); dbg_put_2d(dcf_margin_m); dbg_putc('/'); dbg_put_2d(dcf_margin_h);
      dbg_puts(dcf_zone ? " CEST" : " CET");
    }
    if (!dcf_frame_conf) dbg_puts(" unconfirmed");   // delivered thanks to grid protection
    dbg_putc('\n');
    return;
  }

  // Atomic load: software mirror, CMOS counters and the PHASE of Timer2.
  // The start of second 0 on the grid (dcf_marker_ticks) is the top of the
  // minute, give or take the receiver latency (a few tens of ms). TCNT2 is
  // rewritten so that the next overflow falls exactly 1 s after that top;
  // otherwise second :01 would come anywhere in the following second.
  // The grid starts 39 ms before the nominal top (DCF_GUARD), so the frame
  // may be closed slightly before it: wait for the top (<= 10 ticks).
  for (;;) {
    int16_t early;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { early = (int16_t)(dcf_marker_ticks - now_ticks()); }
    if (early <= 0) break;
  }
  bool late;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    uint16_t elapsed = (uint16_t)(now_ticks() - dcf_marker_ticks);  // ticks since the top
    // The frame is closed at most 1 s after the start of second 0
    // (dcf_poll); beyond that the current second would no longer be :00.
    // 6-tick margin: the new counter must not overflow before the
    // TIFR2 = TOV2 below (at most 0.6 ms later).
    late = elapsed >= 250;
    if (!late) {
      sw_hr  = hours;
      sw_min = minutes;
      sw_sec = 0;
      pulse_mr_s();                     // U3 to 0 (no CLK)
      load_counter_m(minutes);          // MR, then N CLK pulses
      load_counter_h(hours);
      while (ASSR & _BV(TCN2UB)) ;
      TCNT2 = (uint8_t)elapsed;         // overflow in (256 - elapsed) ticks = 1 s after the top
      // The value only reaches TCNT2 on the 2nd rising edge of TOSC1
      // (<= 61 us, section 17.9): until then the old counter keeps running
      // (and may overflow), and an ISR following this block would still see
      // the old time axis.
      while (ASSR & _BV(TCN2UB)) ;
      _delay_us(40);                    // a TOV2 from the old counter has had time to arrive
      TIFR2 = _BV(TOV2);                // an overflow of the old time axis is void
      if (TIMSK2 & _BV(OCIE2A)) {       // button sample armed on the old axis: the jump of
        while (ASSR & _BV(OCR2AUB)) ;   // TCNT2 may skip the compare match (section 17.5.2),
        OCR2A = (uint8_t)(elapsed + DEBOUNCE_TICKS);   // so schedule it again
        TIFR2 = _BV(OCF2A);
      }
      // The time axis has just jumped (TCNT2 rewritten): express the DCF77
      // timestamps on the new axis, or the second grid would shift by as much.
      uint16_t new_now = ((uint16_t)ovf_count << 8) | (uint8_t)elapsed;
      dcf_shift_time((uint16_t)(new_now - elapsed - dcf_marker_ticks));
      now_last = new_now;
    }
  }
  if (late) { dbg_puts("SYNC late\n"); return; }

  dbg_puts("SYNC "); dbg_put_2d(hours); dbg_putc(':'); dbg_put_2d(minutes);
  dbg_puts(" OK margin "); dbg_put_2d(dcf_margin_m); dbg_putc('/'); dbg_put_2d(dcf_margin_h);
  dbg_puts(dcf_zone ? " CEST" : " CET");
  if (!dcf_frame_conf) dbg_puts(" unconfirmed");
  dbg_putc('\n');
}

#if DCF77_DEBUG_PULSES
// Prints the last closed DCF77 second outside the ISR (bit-banged UART,
// about 1 ms per character), without a newline: it is appended to the "T"
// line of the second.
static void dcf_debug_print(void)
{
  bool sec, marker, acq;
  uint8_t a, b, bw, c, code, pos, v[2];
  int8_t lock;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    sec    = dcf_dbg_sec;    dcf_dbg_sec    = false;
    marker = dcf_dbg_marker; dcf_dbg_marker = false;
    lock   = dcf_dbg_lock;   dcf_dbg_lock   = -1;
    acq    = dcf_dbg_acq;    dcf_dbg_acq    = false;
    a = dcf_dbg_a; b = dcf_dbg_b; bw = dcf_dbg_bw; c = dcf_dbg_c; code = dcf_dbg_code; pos = dcf_dbg_pos;
    v[0] = dcf_dbg_votes[0]; v[1] = dcf_dbg_votes[1];
  }
  dbg_puts(" D ");
  if (sec) {
    dbg_put_u32(a);  dbg_putc(' '); dbg_put_u32(b); dbg_putc(' ');
    dbg_put_u32(bw); dbg_putc(' '); dbg_put_u32(c); dbg_putc(' ');
    dbg_putc(code == DCF_NONE ? '-' : code == DCF_BAD ? '?' : code >= dcf_thr ? '1' : '0');
  } else {
    dbg_puts("- - - - .");          // grid not locked: no second closed
  }
  dbg_puts(" s");
  if (pos == 255) dbg_putc('-'); else dbg_put_u32(pos);
  if (acq) {                        // phase vote: best / disjoint triplet
    dbg_puts(" vote "); dbg_put_u32(v[0]); dbg_putc('/'); dbg_put_u32(v[1]);
  }
  if (marker)    dbg_puts(" MIN");
  if (lock == 1) dbg_puts(" LOCK");
  if (lock == 2) dbg_puts(" RECENTER");
  if (lock == 0) dbg_puts(" UNLOCK");
}
#endif
#endif  // DCF77_ENABLED

// ---------------------------------------------------------------------
// Sleep: IDLE (the I/O clock keeps running, so the Timer1 PWM and INT1
// edge detection work; asynchronous Timer2 runs). Called with interrupts
// DISABLED.
// ---------------------------------------------------------------------
static inline void sleep_idle(void)
{
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  // The AVR guarantees that no interrupt can run between sei() and the
  // following sleep instruction.
  sei();
  sleep_cpu();
  sleep_disable();
}

// ---------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------
void setup(void)
{
  cli();
  init_gpio();
  init_peripherals();
  reset_counters_hw();
  init_timer2_async();
  init_interrupts();
  // First light measurement without filtering: the LEDs come up at the
  // right level at boot instead of rising from dark over 4 s.
  light_acc = (uint16_t)(adc_read_light() << LIGHT_FILTER_SHIFT);
  update_dimming();
  sei();

  dbg_puts("BOOT binary_clock\n");
}

void loop(void)
{
  // Canonical AVR pattern: cli() -> test the flags -> sei() or sleep.
  // Without cli(), an ISR could set a flag between the test and
  // sleep_cpu(), and the CPU would sleep with the event pending.
  cli();

  bool tick     = flag_tick;
  flag_tick     = false;
  bool has_work = tick || flag_button_event;
#if DCF77_ENABLED
  bool frame_ready = flag_frame_ready;
  flag_frame_ready = false;          // consumed under cli(): no frame is lost
  has_work = has_work || frame_ready;
#endif

  if (has_work) {
    sei();
#if DCF77_ENABLED
    if (frame_ready) decode_and_load_dcf77_frame();   // first, to re-align the phase as early as possible
#endif
    if (tick) {
      update_dimming();              // one ADC conversion per second (about 110 us)
#if DCF77_ENABLED
      uint8_t ev = clock_event;      // at most one per hour: no ATOMIC block needed
      if (ev) {
        clock_event = 0;
        dbg_puts(ev == 1 ? "DST CET->CEST\n" : ev == 2 ? "DST CEST->CET\n" : "LEAP 59 held\n");
      }
#endif
#if VERBOSE_TICK
      uint8_t h, m, s;               // "T <seconds since boot> hh:mm:ss L <adc> <level>"
      ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { h = sw_hr; m = sw_min; s = sw_sec; }
      dbg_puts("T "); dbg_put_u32(tick_total); dbg_putc(' '); dbg_put_hms(h, m, s);
      dbg_puts(" L "); dbg_put_u32(light_filt); dbg_putc(' '); dbg_put_u32(bright);
#endif
#if DCF77_ENABLED && DCF77_DEBUG_PULSES
      dcf_debug_print();
#endif
#if VERBOSE_TICK || (DCF77_ENABLED && DCF77_DEBUG_PULSES)
      dbg_putc('\n');
#endif
    }
    if (flag_button_event) handle_buttons();
  } else {
    sleep_idle();   // enables interrupts (sei) before sleeping
  }
}
