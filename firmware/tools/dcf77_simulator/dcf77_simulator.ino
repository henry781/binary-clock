/*
 * dcf77_simulator.ino - Arduino Uno sketch that emulates a DCF77 receiver
 * for testing the clock firmware without a radio signal.
 *
 *  1. DCF77 simulator: complete frames (59 bits, BCD, parities, no pulse in
 *     second 59) on D8, to be wired to PD3 (INT1, pin 5) of the clock MCU.
 *     The start time is set below; the time moves on by one minute per frame.
 *  2. Optional serial bridge (BRIDGE 1), useful without a USB-serial
 *     adapter: everything the target sends on its debug TX (PD1, pin 3,
 *     9600 baud) arrives on Uno D7 (SoftwareSerial RX) and is copied to the
 *     USB port (115200 baud).
 *
 * Commands from the serial monitor (115200 baud):
 *   'p'  toggles the pulse polarity. Default: idle HIGH, pulse LOW, which is
 *        what PD3 sees behind the Q_DCF1 inverter of the clock board; 'p'
 *        gives the raw receiver output (idle LOW, pulse HIGH).
 *   'n'  noise on/off: one pulse in ten is cut to 40 ms; the decoder reads
 *        that second as unreadable and keeps synchronizing.
 *   'f'  fault on/off: bit 21 (minutes, weight 1) is inverted without
 *        fixing the parity; the decoder keeps the right time thanks to the
 *        previous minutes, with a lower minutes margin.
 *   'h'  help.
 *
 * Wiring (common ground; the target can be powered from the Uno 5V pin):
 *   Uno D8  -> target pin 5 (PD3, INT1)
 *   Uno D7  <- target pin 3 (PD1, debug TX)   only with BRIDGE 1
 *
 * BRIDGE 0 by default: the target's debug output is read with a USB-serial
 * adapter. With BRIDGE 1, SoftwareSerial blocks interrupts for about 1 ms
 * per received byte, which can disturb the DCF77 timing while the target
 * prints.
 */

#define BRIDGE            0

#define PIN_TARGET_TX_IN  7   // SoftwareSerial RX (the target transmits on it)
#define PIN_DCF_OUT       8

#define DCF_START_HOUR    13
#define DCF_START_MINUTE  37

#if BRIDGE
#include <SoftwareSerial.h>
SoftwareSerial target(PIN_TARGET_TX_IN, 6);  // RX = D7, TX = D6 (unused)
#endif

static bool    dcf_active_low = true;    // as PD3 behind the Q_DCF1 inverter: idle HIGH, pulse LOW
static bool    dcf_noise      = false;
static bool    dcf_fault      = false;
static uint8_t frame[60];
static uint8_t cur_hour = DCF_START_HOUR, cur_min = DCF_START_MINUTE;

static void put_bcd(uint8_t start, uint8_t value, uint8_t units_bits, uint8_t tens_bits)
{
  uint8_t u = value % 10, t = value / 10;
  for (uint8_t i = 0; i < units_bits; i++) frame[start + i]              = (u >> i) & 1;
  for (uint8_t i = 0; i < tens_bits;  i++) frame[start + units_bits + i] = (t >> i) & 1;
}

static uint8_t parity(uint8_t start, uint8_t len)
{
  uint8_t p = 0;
  for (uint8_t i = 0; i < len; i++) p ^= frame[start + i];
  return p;
}

// DCF77 frame sent during the simulated minute cur_hour:cur_min. As with
// the real signal, it encodes the time of the NEXT minute, which becomes
// valid at the end of the frame (minute marker). Once synchronized, the
// target therefore shows the simulated time printed by this sketch. The
// date is fixed.
static void build_frame(void)
{
  uint8_t m = cur_min + 1, h = cur_hour;
  if (m >= 60) { m = 0; if (++h >= 24) h = 0; }
  memset(frame, 0, sizeof(frame));
  frame[17] = 0; frame[18] = 1;           // CET (winter time)
  frame[20] = 1;                          // start of time information
  put_bcd(21, m, 4, 3);  frame[28] = parity(21, 7);
  if (dcf_fault) frame[21] ^= 1;          // deliberately wrong parity
  put_bcd(29, h, 4, 2);  frame[35] = parity(29, 6);
  put_bcd(36, 4, 4, 2);                   // day of the month: 4
  put_bcd(42, 7, 3, 0);                   // Sunday
  put_bcd(45, 10, 4, 1);                  // October
  put_bcd(50, 26, 4, 4);                  // year 26
  frame[58] = parity(36, 22);
}

static inline void dcf_level(bool pulse_on)
{
  digitalWrite(PIN_DCF_OUT, (pulse_on ^ dcf_active_low) ? HIGH : LOW);
}

void setup(void)
{
  Serial.begin(115200);
#if BRIDGE
  target.begin(9600);
#endif
  pinMode(PIN_DCF_OUT, OUTPUT);
  dcf_level(false);
  build_frame();
  Serial.println(F("dcf77_simulator: DCF77 frames on D8 | serial bridge on D7 if BRIDGE"));
  Serial.print(F("simulated start minute: ")); Serial.print(cur_hour); Serial.print(':'); Serial.println(cur_min);
  Serial.println(F("  'p' polarity, 'n' noise on/off, 'f' parity fault on/off, 'h' help"));
}

void loop(void)
{
#if BRIDGE
  // --- serial bridge (non-blocking) ---
  while (target.available()) Serial.write(target.read());
#endif

  // --- commands ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') { dcf_active_low = !dcf_active_low; dcf_level(false);
                    Serial.println(dcf_active_low ? F("DCF: idle HIGH, pulse LOW") : F("DCF: idle LOW, pulse HIGH")); }
    if (c == 'n') { dcf_noise = !dcf_noise; Serial.println(dcf_noise ? F("noise ON") : F("noise OFF")); }
    if (c == 'f') { dcf_fault = !dcf_fault; build_frame(); Serial.println(dcf_fault ? F("parity fault ON") : F("parity fault OFF")); }
    if (c == 'h') { Serial.println(F("p: polarity | n: noise | f: parity fault | current minute printed below")); }
  }

  // --- DCF77 simulator, timed by millis() ---
  static uint32_t minute_start = millis();
  static uint8_t  bit_idx      = 0;
  static bool     pulse_on     = false;
  static bool     pulse_done   = false;

  uint32_t t_in_minute = millis() - minute_start;
  uint8_t  sec         = t_in_minute / 1000;
  uint16_t ms_in_sec   = t_in_minute % 1000;

  if (sec >= 60) {                        // new minute
    minute_start += 60000UL;
    if (++cur_min >= 60) { cur_min = 0; if (++cur_hour >= 24) cur_hour = 0; }
    build_frame();
    Serial.print(F("DCF: minute ")); Serial.print(cur_hour); Serial.print(':'); Serial.print(cur_min);
    Serial.println(F(" begins"));
    bit_idx = 0; pulse_done = false;
    return;
  }
  if (sec != bit_idx) { bit_idx = sec; pulse_done = false; }

  if (sec == 59) { if (pulse_on) { pulse_on = false; dcf_level(false); } return; }  // minute marker: no pulse

  uint16_t width = frame[sec] ? 200 : 100;
  if (dcf_noise && (sec % 10 == 3)) width = 40;      // cut pulse: unreadable second for the target

  if (!pulse_done && !pulse_on && ms_in_sec < width) { pulse_on = true; dcf_level(true); }
  if (pulse_on && ms_in_sec >= width)                { pulse_on = false; pulse_done = true; dcf_level(false); }
}
