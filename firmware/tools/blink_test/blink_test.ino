/*
 * blink_test.ino - is the chip running, and at the right clock?
 *
 * Uses only the internal 8 MHz RC oscillator and busy-wait loops, not the
 * 32.768 kHz crystal. Target: bare ATmega328P, MiniCore, Internal 8 MHz.
 *
 *   pin 14 (PB0): 1 Hz square wave (500 ms HIGH / 500 ms LOW), visible on
 *                 an LED with a 330 Ohm resistor
 *   pin 15 (PB1): 1 kHz square wave (500 us / 500 us): its frequency is that
 *                 of the RC oscillator, nominally 1.000 kHz
 *   pin 17 (PB3): copy of the 1 kHz signal, on the same pin as crystal_test
 *                 (J_ISP1 pin 4 on the clock board)
 *
 * Nothing moves: check the supply, RESET (pin 1 must be at 5 V) and the
 * fuses. About 125 Hz instead of 1 kHz: the CKDIV8 fuse is still set, run
 * "Burn Bootloader" again. The internal RC is factory-calibrated to
 * +/-10 %; the measured frequency also gives the correction for DBG_BIT_US
 * in the clock firmware. If this test passes but crystal_test shows
 * nothing, the 32.768 kHz crystal is not oscillating.
 */

#include <avr/io.h>
#include <util/delay.h>

void setup(void)
{
  DDRB  = _BV(PB0) | _BV(PB1) | _BV(PB3);
  PORTB = 0;
}

void loop(void)
{
  // 500 ms = 500 periods of 1 kHz
  for (uint16_t i = 0; i < 500; i++) {
    PORTB |=  _BV(PB1) | _BV(PB3); _delay_us(498);
    PORTB &= ~(_BV(PB1) | _BV(PB3)); _delay_us(498);
  }
  PORTB ^= _BV(PB0);
}
