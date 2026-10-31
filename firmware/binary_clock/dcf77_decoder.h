/*
 * dcf77_decoder.h - noise-tolerant DCF77 decoder (docs/dcf77-decoder.md)
 *
 * No hardware dependency: the firmware includes it in the sketch, and the
 * PC simulator in firmware/test/ compiles it as is to replay oscilloscope
 * captures and noisy synthetic signals.
 *
 * Time base: 16-bit timestamps in ticks of 1/256 s (3.9 ms), supplied by
 * the caller. Inputs:
 *   dcf_edge(now, active)  on every change of the line (active = pulse);
 *   dcf_poll(now)          once per second (Timer2 overflow): closes the
 *                          seconds even when the line no longer moves, and
 *                          evaluates the phase vote every 8 s.
 * Output: flag_frame_ready + dcf_frame_done[] (one weight per second) at
 * the top of the minute, and dcf_marker_ticks = start of second 0. The
 * frame is decoded outside the ISR by dcf_accumulate(), then dcf_gate().
 *
 * The receiver on the assembled clock delivers, besides the DCF77 pulses,
 * 2 to 8 spurious pulses per second (5-320 ms), episodes where the line
 * sticks active, a spurious pulse that often precedes the real pulse and
 * merges with it, and a quieter zone after it; "0" pulses last 70-140 ms,
 * "1" pulses 150-290 ms, and a quarter of the bits or more read wrong. Hence:
 *
 *   1. Acquisition by vote: the phase of every pulse start (low byte of the
 *      timestamp) is counted in 32 bins of 8 ticks; a pulse cut by a gap of
 *      up to 23 ms is joined before it votes. The grid only locks on a
 *      clear majority.
 *   2. The grid is kept: every second lasts exactly 256 ticks (crystal) and
 *      starts 39 ms before the mean pulse start. Once locked, it is not
 *      dropped for unreadable seconds: the vote goes on, and while its phase
 *      receives votes (dcf_conf) the signal is present (frames delivered,
 *      phase tracking active). A clear vote 47-94 ms away recenters it;
 *      farther away it moves it, unless second 59 has been found (only the
 *      DCF77 phase has one). After a confident frame, only an overwhelming
 *      vote touches it for 20 min. If no second 59 shows up within 12 min,
 *      the grid has locked on a regular group of spurious pulses: it is
 *      dropped and that phase is excluded from the vote for about 4 min.
 *   3. Measurement in fixed windows: active time in A = -39..+100 ms around
 *      the expected start, in 113..199 ms (second half of a "1"), and after
 *      about 290 ms (noise); length of the pulse that starts in A, joined
 *      across gaps < 23 ms. Each second gets a weight = log-likelihood
 *      ratio "1" versus "0" of these measurements, in half-nats (table
 *      dcf_wtab, measured with the simulator).
 *   4. Position in the minute from history: each second goes into its slot
 *      of a modulo-60 counter (ring); a per-slot score rises when A is empty
 *      and falls on a clear pulse; second 59 is the slot that clearly leads
 *      all the others. Without it no frame is delivered: a missed pulse is
 *      never taken for the minute marker. Once confirmed by a confident
 *      frame, it is kept while the grid is protected, whatever the scores.
 *   5. A frame is only delivered if it contains no run of more than 8
 *      unreadable seconds and if the vote confirms the phase; while the
 *      grid is protected it is also delivered without votes, but then it
 *      only adds to the scores and re-aligns a clock that already shows its
 *      time (no DST change, no announcement, no extended protection).
 *   6. Decoding over several minutes (dcf_accumulate): every frame votes
 *      for the 60 possible minutes and the 48 possible hours (24 x time zone
 *      CET / CEST); the time is only loaded once it leads the runner-up by
 *      about 7 nats (8 for the hours), after two frames at least, and
 *      (dcf_gate) once a time that differs from the clock has been
 *      confirmed by a second frame.
 *   7. DST change: the end of 01 h CET (02 h CEST) leads to both zones,
 *      decided by the announcement (bit 16) and then by the following
 *      frames; when it is clearly announced, the clock changes by itself at
 *      the top of the hour (dcf_dst_due), and the gate accepts the +/-1 h
 *      that follows. Announced leap second (bit 19): second 59 of 00:59 CET
 *      (01:59 CEST) lasts two seconds (dcf_leap_due) and the position is
 *      learned again; a missed announcement shows as a pulse in place of
 *      the gap at the top of 01:00 CET (02:00 CEST).
 */
#ifndef DCF77_DECODER_H
#define DCF77_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#if defined(__AVR__)
#include <avr/pgmspace.h>
#define DCF_TABLE_ATTR    PROGMEM
#define DCF_TABLE_READ(p) ((int8_t)pgm_read_byte(p))
#else
#define DCF_TABLE_ATTR
#define DCF_TABLE_READ(p) (*(p))
#endif

// Debug hooks (empty by default), called in ISR context when a second is
// closed, at the top of the minute, when the grid locks / recenters / is
// dropped (1 / 2 / 0), and on every evaluation of the phase vote.
#ifndef DCF_HOOK_SECOND
#define DCF_HOOK_SECOND(code, pos) ((void)0)
#endif
#ifndef DCF_HOOK_MARKER
#define DCF_HOOK_MARKER() ((void)0)
#endif
#ifndef DCF_HOOK_LOCK
#define DCF_HOOK_LOCK(event) ((void)0)
#endif
#ifndef DCF_HOOK_ACQ
#define DCF_HOOK_ACQ(best, other) ((void)0)
#endif

#define DCF_GUARD      10   // 39 ms: the grid starts this much before the mean pulse start
#define DCF_PULSE_A    26   // 100 ms: minimum pulse length (a "0")
#define DCF_A_END      (DCF_GUARD + DCF_PULSE_A)   // window A: -39..+100 ms
#define DCF_B_LEN      38   // code = pulse length beyond 100 ms, capped at 148 ms
#define DCF_W_END      (DCF_A_END + DCF_B_LEN + DCF_GUARD)  // end of the pulse zone (~290 ms)
#define DCF_C_LEN      (256 - DCF_W_END)         // rest of the second
#define DCF_BW_BEG     (DCF_GUARD + 29)          // window 113..199 ms after the expected start:
#define DCF_BW_END     (DCF_GUARD + 51)          // active for a "1", empty for a "0"
#define DCF_JOIN        6   // gap < 23 ms inside a pulse: same pulse
#define DCF_NONE       0xFE // second without a pulse: less than 6 ticks (23 ms) active in A
#define DCF_BAD        0xFF // unreadable second: 6-12 ticks (23-51 ms) active in A, or stuck line (C > 50 %)
#define DCF_THR        15   // code >= 15 (pulse >= ~160 ms): "1" (debug output, weight table)
#define DCF_LONG       34   // code >= 34 (>= 230 ms): long pulse, often a merged spurious pulse
#define DCF_C_NOISY    43   // C > 43 ticks (25 % of window C): noisy second

#define DCF_ACQ_TICKS  (8 * 256)  // phase vote evaluated every 8 s (by dcf_poll)
#define DCF_ACQ_MIN    6    // >= 6 votes in the 3 adjacent bins with the most votes...
                            // ...and >= 2x the best disjoint triplet
#define DCF_VOTE_MIN   12   // a pulse only votes if it lasts 47-312 ms...
#define DCF_VOTE_MAX   80
#define DCF_VOTE_GAP   6    // ...once joined across gaps < 23 ms
#define DCF_CONF_MIN   7    // votes in a triplet of bins touching the grid phase (~+/-60 ms)...
#define DCF_CONF_K     8    // ...confirmation valid for 8 evaluations (64 s)
#define DCF_RELOCK_TOL 24   // clear vote more than +/-94 ms from the grid: grid moved (else recentered)
#define DCF_TRACK_TOL  12   // +/-47 ms: pulse starts used to track the grid
#define DCF_TRACK_N    8    // phase correction after 8 pulse starts
#define DCF_MAX_BAD    8    // more than 8 unreadable seconds in a row: no frame, tracking paused
#define DCF_WATCHDOG   720  // 12 min locked without a clear second 59: grid dropped... (in heavy
                            // noise, second 59 can take up to 12 min to stand out)
#define DCF_BAN        30   // ...and its phase excluded from the vote for 30 evaluations (4 min)
#define DCF_GRID_OK    1200 // grid confirmed by a confident frame: protected for 20 min...
#define DCF_ACQ_STRONG 12   // ...except against an overwhelming vote (>= 12, >= 3x the next)
#define DCF_G_MAX      15   // slot score (second 59?) bounded to 0..15
#define DCF_G_T1       6    // elected slot: score >= 6 (two clear gaps)...
#define DCF_G_T2       4    // ...and >= 4 more than any other slot
#define DCF_B20_PEN    2    // clear "0" read 21 slots later: bit 20 != 1, the slot loses 2
#define DCF_SOFT_MAX   6    // "0" more than 6 ticks (23 ms) below the threshold: clear (bit 20 penalty)

static volatile uint8_t  dcf_ring[60];         // weight (int8) of the last closed second of each slot
static volatile uint8_t  dcf_ring_nz[60];      // 1: noisy second (no clear pulse)
static volatile uint8_t  dcf_frame_done[59];   // frame read from the ring at the end of second 59
static volatile uint8_t  dcf_noisy_done = 0;   // noisy seconds in that frame
static volatile bool     flag_frame_ready = false;
static volatile uint16_t dcf_marker_ticks = 0; // start of second 0 (top of the minute)
static volatile bool     dcf_marker_pulse = false;  // second 59 of that frame carried a pulse
static volatile bool     dcf_hist_reset   = false;  // (loop -> ISR) position must be learned again

static volatile bool     dcf_locked    = false;  // second grid locked
static volatile bool     dcf_level     = false;  // line active (pulse) since the last edge
static volatile bool     dcf_first     = false;  // partial second (lock, recentering): unreadable
static volatile uint16_t dcf_sec_ticks  = 0;     // start of the current second (grid)
static volatile uint16_t dcf_last_ticks = 0;     // active time integrated up to here
static volatile uint8_t  dcf_a = 0, dcf_b = 0, dcf_c = 0;  // active in A, pulse code, active after the pulse zone
static volatile uint8_t  dcf_bw = 0;             // active in 113..199 ms
static volatile int16_t  dcf_chain_beg = 0;      // start (relative) of the pulse that began in A
static volatile int16_t  dcf_chain = -128;       // end (relative) of that pulse
static volatile uint8_t  dcf_bad_run   = 0;      // consecutive unreadable seconds
static volatile uint8_t  dcf_clean     = 0;      // seconds since the last long unreadable run
static volatile uint8_t  dcf_conf      = 0;      // evaluations left during which the phase is confirmed
static volatile int8_t   dcf_recenter  = 0;      // grid recentering at the next second boundary
static volatile int16_t  dcf_track_sum = 0;      // summed offsets of pulse starts from the grid
static volatile uint8_t  dcf_track_n   = 0;
static volatile uint16_t dcf_nomark    = 0;      // seconds locked without a clear second 59
static volatile uint8_t  dcf_ban_bin   = 0;      // phase bin excluded from the vote (watchdog)...
static volatile uint8_t  dcf_ban_ttl   = 0;      // ...for this many evaluations
static uint16_t          dcf_grid_ok   = 0;      // seconds of protection left (DCF_GRID_OK)

// Acquisition: votes per phase bin (8 ticks) plus the sum of the positions
// within the bin, for a phase finer than the bin.
static volatile uint16_t dcf_acq_ticks  = 0;     // start of the voting period
static volatile uint16_t dcf_pulse_ticks = 0;    // start of the current (joined) pulse
static volatile uint16_t dcf_pulse_end   = 0;    // end of the last piece of pulse
static volatile bool     dcf_vote_pending = false;  // pulse ended, not counted yet
static uint8_t           dcf_acq_cnt[32];
static uint16_t          dcf_acq_off[32];

// Position in the minute: score of each slot of a modulo-60 second counter
// (second 59 is the slot whose window A is most often empty).
static uint8_t           dcf_slot_a[60];
static volatile uint8_t  dcf_slot = 0;           // slot of the current second
static uint8_t           dcf_m59  = 255;         // slot chosen for second 59 (255: none)...
static bool              dcf_m59_ok = false;     // ...confirmed by a frame (confident minutes)
static volatile bool     dcf_frame_good = false; // (loop -> ISR) the last frame confirmed it
static volatile bool     dcf_frame_conf = true;  // (ISR -> loop) frame delivered with votes at the grid phase

// Fixed 0/1 threshold: the weight table is measured against it.
static const uint8_t dcf_thr = DCF_THR;

// Drops the grid; clear: votes are stale (time axis broken).
static void dcf_unlock(bool clear)
{
  dcf_locked  = false;
  dcf_conf    = 0;
  dcf_grid_ok = 0;
  if (clear) {
    dcf_vote_pending = false;
    memset(dcf_acq_cnt, 0, sizeof dcf_acq_cnt);
    memset(dcf_acq_off, 0, sizeof dcf_acq_off);
  }
  DCF_HOOK_LOCK(0);
}

// Slot score, asymmetric: a low A is strong evidence of a gap (a real pulse
// rarely falls there), a high A only weak evidence of a pulse (spurious
// pulses also fill second 59).
static inline void dcf_slot_update(uint8_t *g, uint8_t a)
{
  int8_t d = a < 6 ? 3 : a < 13 ? 2 : a < 16 ? 1 : a < 20 ? 0 : -1;
  int16_t v = (int16_t)*g + d;
  *g = (uint8_t)(v < 0 ? 0 : v > DCF_G_MAX ? DCF_G_MAX : v);
}

// Returns the slot of second 59, or 255. A slot is elected when it stands
// out (score >= 6 and 4 more than any other slot). Once a frame read at
// that position has given confident minutes, the slot is kept
// unconditionally while the grid is protected (dcf_grid_ok, 20 min after
// the last confident frame): second 59 cannot change slot unless the grid
// moves, which forgets the position (dcf_forget_position). The scores are
// only used to find it: in heavy noise, other slots (a second whose pulse is
// often missing, for instance) climb to the ceiling without being second 59.
// When the protection expires, the slot must be elected again; this also
// releases a grid that has drifted out of phase, where all slots sit at the
// ceiling. An unconfirmed, wrongly elected slot (shifted frames that never
// become confident) gives way as usual.
static uint8_t dcf_slot_marker(void)
{
  uint8_t m1 = 0, hi = 0, hi2 = 0;
  for (uint8_t i = 0; i < 60; i++) {
    uint8_t v = dcf_slot_a[i];
    if (v > hi) { hi2 = hi; hi = v; m1 = i; }
    else if (v > hi2) hi2 = v;
  }
  uint8_t m = dcf_m59;
  if (dcf_frame_good) { dcf_frame_good = false; dcf_m59_ok = m != 255; dcf_grid_ok = DCF_GRID_OK; }
  if (m != 255 && dcf_m59_ok && dcf_grid_ok) return m;
  m = (hi >= DCF_G_T1 && (uint8_t)(hi - hi2) >= DCF_G_T2) ? m1 : 255;
  if (m != dcf_m59) dcf_m59_ok = false;
  dcf_m59 = m;
  return m;
}

// Position in the minute unknown: ring and scores cleared.
static void dcf_forget_position(void)
{
  dcf_slot   = 0;
  dcf_clean  = 0;
  dcf_nomark = 0;
  dcf_m59    = 255;
  dcf_m59_ok = false;
  memset((void *)dcf_ring, 0, sizeof dcf_ring);
  memset((void *)dcf_ring_nz, 1, sizeof dcf_ring_nz);
  memset(dcf_slot_a, 0, sizeof dcf_slot_a);
}

// Weight of a second = log-likelihood ratio ln P(measurement | 1) /
// P(measurement | 0) in half-nats, truncated towards 0: the smallest value
// measured with the simulator over six conditions (synthetic model at 1.8,
// 3 and 5 spurious pulses/s, board model at severity 0.5, 0.75 and 1; 80
// signals of 40 min each), and 0 where the sign changes from one condition
// to another. Indexed by class of the second x code group x activity in
// 113..199 ms (in quarters): a "1" cut by a gap keeps its second half
// active, a "0" stretched by a merged spurious pulse leaves it partly empty.
//                              activity 113..199 ms:   0    1    2    3    4
static const int8_t dcf_wtab[4][5][5] DCF_TABLE_ATTR = {
  { /* clean: 13 <= A < 36, C <= 25 % */
    /* code - 15 <= -7 */                              { -5,  -2,   0,   0,   0 },
    /* -6..-1 */                                       { -4,   0,   0,   0,   0 },
    /* 0..5 */                                         { -3,   0,   0,   1,   2 },
    /* >= 6, code < 34 */                              {  0,   0,   0,   2,   3 },
    /* code >= 34 */                                   {  0,   0,   0,   1,   3 } },
  { /* noisy: C > 25 % */
                                                       { -5,  -3,   0,   0,   0 },
                                                       { -4,   0,   0,   0,   0 },
                                                       { -4,   0,   0,   2,   2 },
                                                       {  0,  -1,   0,   2,   3 },
                                                       {  0,   0,   0,   0,   2 } },
  { /* window A full (spurious pulse just before the pulse) */
                                                       {  0,   0,   0,   0,   0 },
                                                       { -3,  -1,   0,   0,   0 },
                                                       { -2,   0,   0,   0,   0 },
                                                       {  0,   0,   0,   0,   1 },
                                                       {  0,   0,   0,   0,   1 } },
  { /* A low (6 <= A < 13: start clipped or delayed) */
                                                       { -1,  -1,   0,   0,   0 },
                                                       {  0,   0,   0,   0,   1 },
                                                       {  0,   0,   0,   0,   2 },
                                                       {  0,   0,   0,   0,   3 },
                                                       {  0,   0,   0,   0,   2 } } };

static int8_t dcf_weight(uint8_t a, uint8_t code, uint8_t c, uint8_t bw)
{
  if (a < DCF_PULSE_A / 4) return 0;                      // no pulse
  uint8_t k = a < DCF_PULSE_A / 2 ? 3 : a >= DCF_A_END ? 2 : c > DCF_C_NOISY ? 1 : 0;
  int8_t  d = (int8_t)(code - DCF_THR);
  uint8_t g = code >= DCF_LONG ? 4 : d <= -7 ? 0 : d <= -1 ? 1 : d <= 5 ? 2 : 3;
  // activity quarter, rounded over 22 ticks, without division (ISR): 0..2, 3..8, 9..13, 14..19, 20..22
  uint8_t q = bw < 3 ? 0 : bw < 9 ? 1 : bw < 14 ? 2 : bw < 20 ? 3 : 4;
  int8_t w = DCF_TABLE_READ(&dcf_wtab[k][g][q]);
  // Two corrections from recordings of the real receiver (true time
  // known), more cautious than the simulator: a long pulse in a second where
  // the line is active more than 50 % of the time after 290 ms was a "0" in
  // every case; a "1" read with the line already active at the start of A
  // (A >= 33) was right only 10 times out of 17.
  if (g == 4 && c > DCF_C_LEN / 2) return 0;
  return a >= DCF_A_END - 3 && w > 2 ? 2 : w;
}

// Closes the current second: classifies it, stores it in its slot, moves
// the grid on by one second and, at the end of second 59, delivers the frame.
static void dcf_close_second(void)
{
  uint8_t a = dcf_a, code;
  if (a < DCF_PULSE_A / 4)                               code = DCF_NONE;
  else if (a < DCF_PULSE_A / 2 || dcf_c > DCF_C_LEN / 2) code = DCF_BAD;
  else                                                   code = dcf_b;
  int8_t w = dcf_weight(a, dcf_b, dcf_c, dcf_bw);
  bool noisy = a < DCF_PULSE_A / 2 || a >= DCF_A_END || dcf_c > DCF_C_NOISY;
  bool first = dcf_first;
  if (first) {                      // second already under way (lock, recentering): partial measurement
    dcf_first = false;
    code = DCF_BAD;
    w = 0;
    noisy = true;
  } else if (code > DCF_B_LEN) {
    if (dcf_bad_run < 255) dcf_bad_run++;
  } else {
    dcf_bad_run = 0;
  }
  if (first || dcf_bad_run > DCF_MAX_BAD) dcf_clean = 0;
  else if (dcf_clean < 255)              dcf_clean++;
  if (dcf_hist_reset) { dcf_hist_reset = false; dcf_forget_position(); }

  uint8_t slot = dcf_slot;
  dcf_slot = (uint8_t)(slot == 59 ? 0 : slot + 1);
  dcf_ring[slot]    = (uint8_t)w;
  dcf_ring_nz[slot] = noisy;
  if (!first) {
    dcf_slot_update(&dcf_slot_a[slot], a);
    // Bit 20 is always 1: a clear "0" read here contradicts the hypothesis
    // "second 59 is 21 slots earlier" (a "1" is rarely read as "0").
    if (code <= DCF_B_LEN && (uint8_t)(code + DCF_SOFT_MAX) < dcf_thr) {
      uint8_t *g = &dcf_slot_a[slot >= 21 ? slot - 21 : slot + 39];
      *g = *g > DCF_B20_PEN ? (uint8_t)(*g - DCF_B20_PEN) : 0;
    }
  }
  uint8_t m59 = dcf_slot_marker();
  uint8_t pos = m59 != 255 ? (uint8_t)((slot + 119 - m59) % 60) : 255;
  // Watchdog: a grid locked on something other than the DCF77 seconds (a
  // regular group of spurious pulses) never finds a second 59.
  if (dcf_grid_ok) dcf_grid_ok--;
  if (m59 != 255 || dcf_grid_ok) dcf_nomark = 0;
  else if (++dcf_nomark > DCF_WATCHDOG) {
    dcf_ban_bin = (uint8_t)((uint8_t)(dcf_sec_ticks + DCF_GUARD) >> 3);
    dcf_ban_ttl = DCF_BAN;
    dcf_nomark  = 0;
    dcf_unlock(false);
    return;
  }
  DCF_HOOK_SECOND(code, pos);

  int8_t step = 0;                  // slow phase tracking
  if (dcf_recenter) {               // clear vote out of tracking range: recenter at once
    step = dcf_recenter;
    dcf_recenter  = 0;
    dcf_first     = true;           // next second already under way or clipped: partial
    dcf_track_sum = 0;
    dcf_track_n   = 0;
  } else if (dcf_bad_run > DCF_MAX_BAD || !dcf_conf) {  // no signal: noise alone would make the phase wander
    dcf_track_sum = 0;
    dcf_track_n   = 0;
  } else if (dcf_track_n >= DCF_TRACK_N) {
    int16_t m = dcf_track_sum / (int16_t)dcf_track_n;
    step = (int8_t)(m > 2 ? 2 : m < -2 ? -2 : m);
    dcf_track_sum = 0;
    dcf_track_n   = 0;
  }
  dcf_sec_ticks += (uint16_t)(int16_t)(256 + step);
  dcf_a = dcf_b = dcf_c = dcf_bw = 0;
  dcf_chain_beg = 0;
  dcf_chain = -128;

  if (pos == 59) {                  // end of the minute: frame = the 59 preceding slots
    // While the grid is protected the frame is delivered even if its phase
    // no longer receives votes (in heavy noise they can stay below the
    // threshold for whole minutes with the grid right). Without votes it
    // only adds to the scores and re-aligns a clock that already shows its
    // time.
    if ((dcf_conf || dcf_grid_ok) && dcf_clean >= 59) {
      dcf_frame_conf = dcf_conf != 0;
      uint8_t j = slot, nz = 0;
      for (uint8_t i = 0; i < 59; i++) {
        if (++j == 60) j = 0;
        dcf_frame_done[i] = dcf_ring[j];
        nz += dcf_ring_nz[j];
      }
      dcf_noisy_done   = nz;
      dcf_marker_pulse = a >= DCF_PULSE_A / 2;   // >= 50 ms active in A: not the marker gap
      dcf_marker_ticks = (uint16_t)(dcf_sec_ticks + DCF_GUARD);
      flag_frame_ready = true;
    }
    DCF_HOOK_MARKER();
  }
}

static inline uint8_t dcf_overlap(int16_t r1, int16_t r2, int16_t lo, int16_t hi)
{
  if (r1 < lo) r1 = lo;
  if (r2 > hi) r2 = hi;
  return r2 > r1 ? (uint8_t)(r2 - r1) : 0;
}

// Integrates the current level from dcf_last_ticks up to until, window by
// window, closing the seconds crossed on the way.
static void dcf_integrate(uint16_t until)
{
  int16_t dt = (int16_t)(until - dcf_last_ticks);
  if (dt < 0) return;               // timestamp of an earlier instant: nothing to integrate
  if (dt > 16 * 256) { dcf_unlock(true); return; }   // time axis broken
  for (;;) {
    uint16_t end     = (uint16_t)(dcf_sec_ticks + 256);
    bool     crosses = (int16_t)(until - end) >= 0;
    uint16_t seg_end = crosses ? end : until;
    if (dcf_level) {
      int16_t r1 = (int16_t)(dcf_last_ticks - dcf_sec_ticks);
      int16_t r2 = (int16_t)(seg_end - dcf_sec_ticks);
      dcf_a  += dcf_overlap(r1, r2, 0, DCF_A_END);
      dcf_bw += dcf_overlap(r1, r2, DCF_BW_BEG, DCF_BW_END);
      dcf_c  += dcf_overlap(r1, r2, DCF_W_END, 256);
      if (r1 <= dcf_chain + DCF_JOIN) {               // continuation of the pulse
        if (r2 > dcf_chain) dcf_chain = r2;
      } else if (r1 < DCF_A_END) {                    // new pulse in A
        dcf_chain_beg = r1 < 0 ? 0 : r1;
        dcf_chain     = r2;
      }                                               // otherwise: spurious pulse after a gap
      int16_t len = (int16_t)(dcf_chain - dcf_chain_beg - DCF_PULSE_A);
      if (len > DCF_B_LEN) len = DCF_B_LEN;
      if (len > (int16_t)dcf_b) dcf_b = (uint8_t)len; // longest pulse that started in A
    }
    if (!crosses) break;
    dcf_last_ticks = seg_end;
    dcf_close_second();
    if (!dcf_locked) return;
  }
  dcf_last_ticks = until;
}

static void dcf_vote(void)
{
  dcf_vote_pending = false;
  uint16_t len = (uint16_t)(dcf_pulse_end - dcf_pulse_ticks);
  if (len < DCF_VOTE_MIN || len > DCF_VOTE_MAX) return;
  uint8_t ph = (uint8_t)dcf_pulse_ticks;
  uint8_t k  = ph >> 3;
  if (dcf_acq_cnt[k] < 255) { dcf_acq_cnt[k]++; dcf_acq_off[k] += ph & 7; }
}

static inline uint8_t dcf_acq_triple(uint8_t i)
{
  uint16_t s = dcf_acq_cnt[i] + dcf_acq_cnt[(i + 1) & 31] + dcf_acq_cnt[(i + 2) & 31];
  return s > 255 ? 255 : (uint8_t)s;
}

// End of a voting period (every 8 s, grid locked or not): locks the grid on
// the 3 adjacent bins with the most votes if they clearly dominate; with
// the grid locked, confirms it if its phase receives votes, and recenters
// or moves it if the vote is clearly elsewhere. Votes are halved at each
// evaluation (sliding memory). A phase just rejected by the watchdog does
// not count for DCF_BAN evaluations.
static void dcf_try_lock(uint16_t now)
{
  if (dcf_ban_ttl) {
    dcf_ban_ttl--;
    for (uint8_t d = 0; d < 5; d++) {
      uint8_t i = (uint8_t)((dcf_ban_bin + 30 + d) & 31);
      dcf_acq_cnt[i] = 0;
      dcf_acq_off[i] = 0;
    }
  }
  const uint8_t *c = dcf_acq_cnt;   // running sums (fewer cycles in the ISR)
  uint8_t best = 0, bi = 0;
  uint16_t t = c[0] + c[1] + c[2];  // triplet of bins i, i+1, i+2
  for (uint8_t i = 0; i < 32; i++) {
    uint8_t s = t > 255 ? 255 : (uint8_t)t;
    if (s > best) { best = s; bi = i; }
    t += c[(i + 3) & 31];
    t -= c[i];
  }
  uint8_t other = 0;                // best triplet sharing no bin with it
  uint8_t j = (uint8_t)((bi + 3) & 31);
  t = c[j] + c[(j + 1) & 31] + c[(j + 2) & 31];
  for (uint8_t d = 3; d <= 29; d++) {
    uint8_t s = t > 255 ? 255 : (uint8_t)t;
    if (s > other) other = s;
    t += c[(j + 3) & 31];
    t -= c[j];
    j = (uint8_t)((j + 1) & 31);
  }
  DCF_HOOK_ACQ(best, other);
  bool pass = best >= DCF_ACQ_MIN && best >= 2u * other;
  uint8_t phase = 0;
  if (pass) {
    uint16_t sum = 0;               // phase = mean of the votes of the triplet
    for (uint8_t d = 0; d < 3; d++) {
      uint8_t i = (uint8_t)((bi + d) & 31);
      sum += dcf_acq_off[i] + 8u * d * dcf_acq_cnt[i];
    }
    phase = (uint8_t)(bi * 8 + (sum + best / 2) / best - DCF_GUARD);
  }
  if (dcf_locked) {                 // does the grid phase receive its votes?
    uint8_t k = (uint8_t)((uint8_t)(dcf_sec_ticks + DCF_GUARD) >> 3), g = 0;
    for (uint8_t d = 30; d <= 32; d++) {
      uint8_t s = dcf_acq_triple((uint8_t)((k + d) & 31));
      if (s > g) g = s;
    }
    if (g >= DCF_CONF_MIN) dcf_conf = DCF_CONF_K;
    else if (dcf_conf)     dcf_conf--;
  }
  for (uint8_t i = 0; i < 32; i++) { dcf_acq_cnt[i] >>= 1; dcf_acq_off[i] >>= 1; }
  if (!pass) return;
  if (dcf_locked) {
    int8_t d = (int8_t)(uint8_t)(phase - (uint8_t)dcf_sec_ticks);
    if (d >= -DCF_TRACK_TOL && d <= DCF_TRACK_TOL) return;     // within tracking range
    // Grid confirmed by a confident frame less than 20 min ago: this is the
    // DCF77 phase. With a weak signal, a group of spurious or delayed
    // pulses can win a vote 6 to 3; recentering on it would take the grid
    // out of phase, and a second recentering would be needed to bring it
    // back, each pair costing minutes of frames. Only an overwhelming vote
    // (a real phase jump on a clean signal) moves it.
    if (dcf_grid_ok && (best < DCF_ACQ_STRONG || best < 3u * other)) return;
    if (d >= -DCF_RELOCK_TOL && d <= DCF_RELOCK_TOL) {         // recenter, position kept
      dcf_recenter = d;
      DCF_HOOK_LOCK(2);
      return;
    }
    // A clear second 59 only exists at the phase of the DCF77 seconds: a
    // grid that has found it is the right one, and a regular group of
    // spurious pulses that wins a vote does not move it.
    if (dcf_slot_marker() != 255) return;
    DCF_HOOK_LOCK(0);               // wrong phase (noise, long drift): grid moved
  }
  dcf_conf       = DCF_CONF_K;
  dcf_recenter   = 0;
  dcf_grid_ok    = 0;
  dcf_sec_ticks  = (uint16_t)(now - (uint8_t)((uint8_t)now - phase));
  dcf_last_ticks = now;
  dcf_a = dcf_b = dcf_c = dcf_bw = 0;
  dcf_chain_beg  = 0;
  dcf_chain      = -128;
  dcf_first      = true;
  dcf_bad_run    = 0;
  dcf_track_sum  = 0;
  dcf_track_n    = 0;
  dcf_forget_position();
  dcf_locked     = true;
  DCF_HOOK_LOCK(1);
}

// Call (in ISR context) on every change of the DCF77 line.
static void dcf_edge(uint16_t now, bool active)
{
  if (dcf_locked) dcf_integrate(now);
  bool starts = active && !dcf_level;
  bool ends   = !active && dcf_level;
  dcf_level = active;

  if (dcf_locked && starts) {
    int16_t r = (int16_t)(now - dcf_sec_ticks - DCF_GUARD);  // offset from the expected start
    if (r >= 128) r -= 256;         // pulse early, belongs to the next second
    if (r >= -DCF_TRACK_TOL && r <= DCF_TRACK_TOL) { dcf_track_sum += r; dcf_track_n++; }
  }
  if (starts) {                     // vote (grid locked or not): wait for a possible next piece
    if (dcf_vote_pending && (uint16_t)(now - dcf_pulse_end) < DCF_VOTE_GAP) {
      dcf_vote_pending = false;     // short gap: same pulse
    } else {
      if (dcf_vote_pending) dcf_vote();
      dcf_pulse_ticks = now;
    }
  } else if (ends) {
    dcf_pulse_end    = now;
    dcf_vote_pending = true;
  }
}

// Call (in ISR context) once per second, edge or no edge: closes the
// seconds of the grid and evaluates the vote every 8 s. The vote runs here
// rather than on an edge: just after the tick, before loop() prints its
// line, it does not distort the bit-banged debug UART.
static void dcf_poll(uint16_t now)
{
  if (dcf_locked) dcf_integrate(now);
  if ((uint16_t)(now - dcf_acq_ticks) >= DCF_ACQ_TICKS - 128) {
    if (dcf_vote_pending && !dcf_level && (uint16_t)(now - dcf_pulse_end) >= DCF_VOTE_GAP)
      dcf_vote();
    dcf_acq_ticks = now;
    dcf_try_lock(now);
  }
}

// The caller's time axis has just jumped by delta ticks (TCNT2 rewritten):
// express all timestamps on the new axis and rotate the votes by as much
// (their phase is the low byte of the timestamps).
static void dcf_shift_time(uint16_t delta)
{
  dcf_sec_ticks    += delta;
  dcf_last_ticks   += delta;
  dcf_marker_ticks += delta;
  dcf_acq_ticks    += delta;
  dcf_pulse_ticks  += delta;
  dcf_pulse_end    += delta;
  uint8_t k = (uint8_t)(((uint8_t)delta + 4) >> 3) & 31;  // bins of 8 ticks, rounded
  dcf_ban_bin = (uint8_t)((dcf_ban_bin + k) & 31);
  for (; k; k--) {
    uint8_t  c = dcf_acq_cnt[31];
    uint16_t o = dcf_acq_off[31];
    for (uint8_t i = 31; i; i--) { dcf_acq_cnt[i] = dcf_acq_cnt[i - 1]; dcf_acq_off[i] = dcf_acq_off[i - 1]; }
    dcf_acq_cnt[0] = c;
    dcf_acq_off[0] = o;
  }
}

// ---------------------------------------------------------------------
// Frame decoding (outside the ISR)
// ---------------------------------------------------------------------

// Integration over several minutes. The time moves on by one minute per
// frame: instead of deciding each frame on its own (a quarter of the bits
// or more can be wrong), every frame adds a score to each of the 60 possible minute
// values and the 48 possible hour values (24 hours x 2 zones, CET and
// CEST): the weight of each second if the value has that bit in its BCD
// code + parity (zone: bits 17 and 18), its opposite otherwise. The scores
// are shifted by the minutes elapsed between two frames; a value is only
// retained once it clearly leads the runner-up, after two frames at least.
// Weights are in half-nats (2 x log-likelihood in nats): the score gap
// between two values is 4 x their log-likelihood ratio, so a lead of 28
// corresponds to about 7 nats. Scores are relative: the best value is 0,
// the others are negative and bounded, so that a false history fades out.
#define DCF_SCORE_MAX  40   // maximum gap behind the best value
#define DCF_CONFIDENT  28   // lead required over the runner-up (minutes)
#define DCF_CONFIDENT_H 32  // hours: they only change once per hour, so a systematic
                            // error accumulates there; a lead of 28 lets rare wrong hours
                            // through in heavy noise
#define DCF_RESET_MIN  120  // more than 2 h without a frame: start from scratch
// DST change announcement (bit 16): A1 = sum of its weights, bounded to
// 0..60. In the simulator, noise alone never takes it above 14; a real
// announcement reaches 13 to 60 by the end of the hour (30 to 60 in most
// cases).
#define DCF_A1_BIAS    12   // prior rarity of a DST change, in units of A1
#define DCF_A1_CAP     60
#define DCF_A1_ARM     24   // clear announcement: the clock changes by itself at the top (dcf_dst_due)
#define DCF_ANN        6    // leap second announced: bit 19 read as a clear "1" 6 times more than "0"
#define DCF_GATE_N     2    // time different from the clock: confirmed by 2 frames
// Noisy frame (seconds without a clear pulse, out of 59): <= 15 -> weights
// x 3/2, >= 46 -> x 3/4, >= 50 -> x 1/2, >= 54 -> frame ignored
// (likelihood ratios measured with the simulator).
#define DCF_NS0        15
#define DCF_NS1        46
#define DCF_NS2        50
#define DCF_NS3        54

static int8_t   dcf_score_m[60];
static int8_t   dcf_score_h[48];         // local hour 0..23 in CET, then in CEST
static uint8_t  dcf_n_frames    = 0;     // frames accumulated (saturates at 255)
static uint32_t dcf_frame_sec   = 0;     // time of the last frame (seconds)
static uint32_t dcf_alt_sec     = 0;     // last out-of-phase frame
static bool     dcf_alt_valid   = false;
static uint8_t  dcf_margin_m    = 0, dcf_margin_h = 0;
static uint8_t  dcf_wscale      = 4;     // weights x dcf_wscale / 4 for the current frame
static volatile uint8_t dcf_zone = 0;   // zone of the best hour: 0 CET (bit 18), 1 CEST (bit 17)
static int8_t   dcf_a1 = 0;              // accumulated DST change announcement
static volatile uint8_t dcf_gate_zone = 0xFF;  // zone of the clock: last accepted time (gate)
static volatile int8_t  dcf_dst_arm = 0;   // +/-1: DST change announced (loop -> ISR, dcf_dst_due)
static uint8_t  dcf_cand_n = 0;          // gate: confident frames that agree but differ from the clock
static int16_t  dcf_cand_v = 0;
static uint32_t dcf_cand_sec = 0;
static uint8_t  dcf_b19 = 0;             // leap second announcement (0..8)
static bool     dcf_leap_armed = false;  // frame of the leap-second minute expected...
static uint32_t dcf_leap_sec = 0;        // ...around this time (seconds)
static volatile bool dcf_leap_arm = false;  // leap second at the end of this hour (loop -> ISR)

// Rotation by k positions upwards: value v becomes v + k.
static void dcf_rotate(int8_t *s, uint8_t n, uint8_t k)
{
  k %= n;
  for (; k; k--) {
    int8_t last = s[n - 1];
    for (uint8_t i = (uint8_t)(n - 1); i; i--) s[i] = s[i - 1];
    s[0] = last;
  }
}

static inline uint8_t dcf_bcd(uint8_t v) { return (uint8_t)((v / 10) << 4 | v % 10); }

// Adds the frame to the score of each value 0..n-1; rx = received weights
// of the field: BCD code (len-1 bits) of the value modulo p, then parity;
// zb is added to the values < p and subtracted from the others (hours:
// zone). Returns the lead of the best value over the best value that
// differs modulo p, and the best value in *best.
static uint8_t dcf_score_field(int8_t *s, uint8_t n, uint8_t p, const int8_t *rx, uint8_t len,
                               int8_t zb, uint8_t *best)
{
  int16_t top = -32768;
  for (uint8_t v = 0; v < n; v++) {
    uint8_t code = dcf_bcd(v < p ? v : (uint8_t)(v - p)), par = 0;
    int16_t acc = s[v] + (v < p ? zb : -zb);   // >= -40 - 11 - 8 x 7: fits in an int8
    for (uint8_t i = 0; i < len; i++) {
      uint8_t bit;
      if (i < len - 1) { bit = code >> i & 1; par ^= bit; }
      else             bit = par;
      acc += bit ? rx[i] : -rx[i];
    }
    s[v] = (int8_t)acc;
    if (acc > top) { top = acc; *best = v; }
  }
  int16_t second = -DCF_SCORE_MAX;
  uint8_t bu = *best < p ? *best : (uint8_t)(*best - p);
  for (uint8_t v = 0; v < n; v++) {
    int16_t r = s[v] - top;
    if (r < -DCF_SCORE_MAX) r = -DCF_SCORE_MAX;
    s[v] = (int8_t)r;
    if ((v < p ? v : v - p) != bu && r > second) second = r;
  }
  return (uint8_t)(-second);
}

static inline void dcf_hmax(int8_t *t, int16_t v) { if (v > *t) *t = (int8_t)v; }

// Weight of second i of the frame, scaled for the frame.
static inline int8_t dcf_soft(const volatile uint8_t *f, uint8_t i)
{
  return (int8_t)((int8_t)f[i] * (int8_t)dcf_wscale / 4);
}

static inline bool dcf_in_phase(uint32_t dt)
{
  uint8_t ph = (uint8_t)(dt % 60);
  return ph <= 1 || ph >= 59;          // +/-1 s: jitter of tick_total at the top
}

// Integrates a frame received at now_sec (seconds, free-running clock).
// Returns NULL and the time if both fields are confident, otherwise the
// reason: leap second ("leap", position to be learned again), frame too
// noisy ("noisy"), misaligned ("align"), out of phase ("phase"), duplicate
// ("dup") or not confident yet ("wait"). Also arms the DST change and the
// leap second of the clock (dcf_dst_due, dcf_leap_due).
static const char *dcf_accumulate(const volatile uint8_t *f, uint32_t now_sec,
                                  uint8_t *hours, uint8_t *minutes)
{
  // Frame of the minute that contains the leap second: its top arrives one
  // second early and the learned position slips by one second. Whatever it
  // contains, do not use it and learn the position again (a few minutes);
  // the hour history remains valid.
  if (dcf_leap_armed && (int32_t)(now_sec - dcf_leap_sec) >= -30) {
    dcf_leap_armed = false;
    dcf_b19 = 0;
    dcf_hist_reset = true;
    return "leap";
  }
  // The more noisy seconds a frame has, the less reliable its readings:
  // weights reduced, or frame ignored.
  uint8_t nz = dcf_noisy_done;
  if (nz >= DCF_NS3) return "noisy";
  dcf_wscale = 4;
  // Alignment checks: bit 0 = 0, bit 20 = 1, exactly one of 17/18. Each is
  // a single bit, often wrong in noise: the frame is only rejected when two
  // of them fail.
  uint8_t fails = 0;
  int8_t s0 = dcf_soft(f, 0), s17 = dcf_soft(f, 17), s18 = dcf_soft(f, 18), s20 = dcf_soft(f, 20);
  if (s20 < 0)                                       fails++;
  if (s0 > 0)                                        fails++;
  if ((s17 > 0 && s18 > 0) || (s17 < 0 && s18 < 0)) fails++;
  if (fails >= 2)                                    return "align";
  dcf_wscale = nz >= DCF_NS2 ? 2 : nz >= DCF_NS1 ? 3 : nz <= DCF_NS0 ? 6 : 4;
  // Leap second announcement (bit 19, during the preceding hour): counter
  // raised by a clear "1", lowered by a "0". The announcements (bits 16 and
  // 19) are only read in frames with votes: a frame delivered without them
  // may be nothing but noise (transmitter lost).
  if (dcf_frame_conf) {
    int8_t s19 = (int8_t)f[19];
    if (s19 >= 2) { if (dcf_b19 < 8) dcf_b19++; }
    else if (s19 < 0 && dcf_b19) dcf_b19--;
  }

  uint32_t k = dcf_n_frames ? (now_sec - dcf_frame_sec + 30) / 60 : 0;
  // Phase: a false top shifts the frame by a whole number of seconds; its
  // time bits are then other fields. The 2-out-of-3 alignment check lets
  // many of them through in heavy noise; the top itself does not lie. Two
  // out-of-phase frames that agree with each other start a new history.
  if (dcf_n_frames && k <= DCF_RESET_MIN && !dcf_in_phase(now_sec - dcf_frame_sec)) {
    if (dcf_b19 >= DCF_ANN) {                // leap second announced: it is what shifted
      dcf_b19 = 0;                           // the top; position to be learned again
      dcf_hist_reset = true;
      return "leap";
    }
    bool again = dcf_alt_valid && dcf_in_phase(now_sec - dcf_alt_sec);
    dcf_alt_sec = now_sec; dcf_alt_valid = true;
    if (!again) return "phase";
    dcf_n_frames = 0;
    k = 0;
  }
  dcf_alt_valid = false;
  if (dcf_n_frames && k == 0) return "dup";
  if (!dcf_n_frames || k > DCF_RESET_MIN) {
    memset(dcf_score_m, 0, sizeof dcf_score_m);
    memset(dcf_score_h, 0, sizeof dcf_score_h);
    dcf_n_frames = 0;
    dcf_a1 = 0;                              // an announcement from before the gap is void
  } else {
    // Hours: for each possible carry r = (v + k) / 60, M[r] = best score of
    // the minutes v that lead to it; hour y of each zone moves to y + r with
    // M[r] + score[y], each hour keeping its best path. With a confident
    // minute this is a plain rotation; with an uncertain one, neighboring
    // hours stay in competition instead of betting on the best minute
    // (which, if wrong at the turn of the hour, would shift the whole
    // history).
    // DST change (02:00 CET -> 03:00 CEST, 03:00 CEST -> 02:00 CET): passing
    // the end of 01 h CET (02 h CEST) also leads to the other zone, one hour
    // later (earlier). A1, accumulated during the preceding hour, decides
    // between the two branches like a bit of weight A1 against the rarity of
    // a change (gap 2 x (A1 - DCF_A1_BIAS)); if the announcement was poorly
    // received, the following frames (hour and zone bits) decide. Neither
    // the minute of the change nor the zone is ever decided alone.
    uint8_t kk = (uint8_t)k;                 // k <= 120 here
    int8_t M[3] = {-128, -128, -128};
    for (uint8_t v = 0; v < 60; v++) {
      uint8_t s8 = (uint8_t)(v + kk), r = s8 >= 120 ? 2 : s8 >= 60 ? 1 : 0;
      if (dcf_score_m[v] > M[r]) M[r] = dcf_score_m[v];
    }
    int16_t sw = 2 * ((int16_t)dcf_a1 - DCF_A1_BIAS);   // > 0: change expected
    int16_t c_chg = sw < 0 ? sw : 0, c_stay = sw > 0 ? -sw : 0;
    int8_t tmp[48];
    memset(tmp, 0x80, sizeof tmp);           // -128: hour unreachable
    for (uint8_t i = 0; i < 48; i++) {
      uint8_t z = i >= 24, y = (uint8_t)(z ? i - 24 : i), hc = (uint8_t)(1 + z);
      for (uint8_t r = 0; r < 3; r++) {
        if (M[r] == -128) continue;
        int16_t b = (int16_t)M[r] + dcf_score_h[i];
        uint8_t x = (uint8_t)(y + r);
        if (x >= 24) x -= 24;
        if (r && y <= hc && hc < y + r) {    // end of the hour of the change passed
          uint8_t x2 = (uint8_t)(z ? (x ? x - 1 : 23) : (x < 23 ? x + 1 : 0));
          dcf_hmax(&tmp[z ? x2 : 24 + x2], b + c_chg);
          b += c_stay;
        }
        dcf_hmax(&tmp[z ? 24 + x : x], b);
      }
    }
    for (uint8_t i = 0; i < 48; i++)
      dcf_score_h[i] = tmp[i] < -DCF_SCORE_MAX ? (int8_t)-DCF_SCORE_MAX : tmp[i];
    dcf_rotate(dcf_score_m, 60, (uint8_t)(k % 60));
  }
  if (dcf_frame_conf) {
    int16_t na = (int16_t)dcf_a1 + dcf_soft(f, 16);
    if (na > DCF_A1_CAP) na = DCF_A1_CAP;
    if (na < 0) na = 0;
    dcf_a1 = (int8_t)na;
  }

  if (dcf_n_frames < 255) dcf_n_frames++;
  dcf_frame_sec = now_sec;

  int8_t rx[8];
  for (uint8_t i = 0; i < 8; i++) rx[i] = dcf_soft(f, (uint8_t)(21 + i));
  dcf_margin_m = dcf_score_field(dcf_score_m, 60, 60, rx, 8, 0, minutes);
  for (uint8_t i = 0; i < 7; i++) rx[i] = dcf_soft(f, (uint8_t)(29 + i));
  uint8_t hz;
  dcf_margin_h = dcf_score_field(dcf_score_h, 48, 24, rx, 7,
                                 (int8_t)(dcf_soft(f, 18) - dcf_soft(f, 17)), &hz);
  // Confirms the position and protects the grid (dcf_slot_marker), but
  // never from a frame without votes, which history alone can lift above
  // the threshold.
  if (dcf_margin_m >= DCF_CONFIDENT && dcf_frame_conf) dcf_frame_good = true;
  uint8_t zone = hz >= 24;
  dcf_zone = zone;
  *hours   = (uint8_t)(zone ? hz - 24 : hz);
  // Hour before a DST change (01 h CET, 02 h CEST): armed if the
  // announcement is clear. Other frames leave it alone: the frame of the
  // change itself, often read just before the tick, must not disarm it.
  if (*hours == 1 + zone && dcf_frame_conf) dcf_dst_arm = dcf_a1 >= DCF_A1_ARM ? (int8_t)(zone ? -1 : 1) : 0;

  // Announced leap second (bit 19 during the preceding hour): inserted
  // after 00:59:59 CET (01:59:59 CEST), it brings the frame of 01:00
  // (02:00) one second early. Every frame of that hour records when to
  // expect it, in case the last ones are rejected.
  if (dcf_b19 >= DCF_ANN && (*hours == 0 || *hours == 1)) {
    dcf_leap_armed = true;
    dcf_leap_sec   = now_sec + 60u * (uint8_t)(60 - *minutes);
  }
  if (*hours == zone) dcf_leap_arm = dcf_b19 >= DCF_ANN;   // clock: dcf_leap_due
  // Leap second whose announcement was only glimpsed (start-up shortly
  // before): the frame of 01:00 CET (02:00 CEST) arrives one second early,
  // and its second 59 carries a pulse (a "0") instead of the marker gap.
  // Its bits are right but its top is not: position to be learned again.
  // Without any announcement, a spurious pulse there would be enough (one
  // day in four at 3 spurious pulses/s).
  if (dcf_marker_pulse && dcf_b19 >= DCF_ANN / 2 && *minutes == 0 && *hours == 1 + zone) {
    dcf_b19 = 0;
    dcf_hist_reset = true;
    return "leap";
  }
  if (dcf_n_frames < 2 || dcf_margin_m < DCF_CONFIDENT || dcf_margin_h < DCF_CONFIDENT_H)
    return "wait";
  return 0;
}

// Minute of the day shown by the clock at the top of a frame, for the
// gate: the frame is read just before the tick (s = 59, top imminent) or
// just after it (0); 1: the clock runs one second fast (missed leap
// second) and is re-aligned without waiting for a second frame. -1: not at
// the top.
static inline int dcf_clock_hm(uint8_t h, uint8_t m, uint8_t s)
{
  if (s == 59) return (h * 60 + m + 1) % 1440;
  if (s <= 1)  return h * 60 + m;
  return -1;
}

// Gate on the current clock (clock_hm = dcf_clock_hm, -1 if the clock is
// not at the top of its minute). A confident time equal to the clock is
// loaded (phase re-alignment); a different one must come back DCF_GATE_N
// times in a row, each confident frame equal to the previous one advanced
// by the minutes elapsed (seconds of tick_total: before the first
// synchronization the phase of the clock is arbitrary). A single wrong
// decode never overwrites a correct clock. Exception, the DST change: the
// clock, still on the old time, shows 02:xx CET (03:xx CEST), the decoder
// has moved to the other zone and gives exactly one hour more (less); it is
// loaded at once. A frame delivered without votes (grid protected,
// dcf_frame_conf false) only re-aligns a clock that already shows its time:
// it changes neither the time, nor the zone, nor the candidate time.
static const char *dcf_gate(uint8_t h, uint8_t m, int clock_hm)
{
  int16_t v = (int16_t)(h * 60 + m);
  uint8_t zone = dcf_zone, cz = dcf_gate_zone;
  if (!dcf_frame_conf) return clock_hm >= 0 && v == clock_hm ? 0 : "gate";
  if (clock_hm >= 0 && (v == clock_hm
      || (cz <= 1 && zone != cz && clock_hm / 60 == (zone ? 2 : 3) && v == clock_hm + (zone ? 60 : -60)))) {
    dcf_cand_n = 0;
    dcf_gate_zone = zone;
    return 0;
  }
  uint32_t dk = (dcf_frame_sec - dcf_cand_sec + 30) / 60;
  if (dcf_cand_n && (int16_t)((dcf_cand_v + dk) % 1440) == v) dcf_cand_n++;
  else dcf_cand_n = 1;
  dcf_cand_v = v; dcf_cand_sec = dcf_frame_sec;
  if (dcf_cand_n >= DCF_GATE_N) { dcf_cand_n = 0; dcf_gate_zone = zone; return 0; }
  return "gate";
}

// DST change applied by the clock itself at the top of the hour, even if
// the frame of that moment is unreadable: the tick ISR calls it at every
// change of hour (new_hour = the hour that begins). Returns +1 at 02:00 CET
// (-> 03:00 CEST), -1 at 03:00 CEST (-> 02:00 CET), otherwise 0; requires a
// clear announcement (A1 >= DCF_A1_ARM) in the last frame and a clock in
// step in the zone the decoder sees. Arming only holds for the next change
// of hour. A wrong change is undone by the gate at the first confident
// frame (different zone, +/-1 h).
static int8_t dcf_dst_due(uint8_t new_hour)
{
  int8_t d = dcf_dst_arm;
  dcf_dst_arm = 0;
  if (!d || dcf_gate_zone != (d > 0 ? 0 : 1) || new_hour != (d > 0 ? 2 : 3)) return 0;
  dcf_gate_zone = (uint8_t)(d > 0);
  return d;
}

// Leap second applied by the clock itself: the tick ISR calls it when
// second 59 of minute 59 ends (h = current hour). True at 00:59:59 CET
// (01:59:59 CEST) if the announcement (bit 19) is clear and the clock is in
// step in the decoder's zone: second 59 then lasts two seconds. Arming only
// holds for that end of hour.
static bool dcf_leap_due(uint8_t h)
{
  bool a = dcf_leap_arm;
  dcf_leap_arm = false;
  return a && dcf_gate_zone == dcf_zone && h == dcf_gate_zone;
}

#endif  // DCF77_DECODER_H
