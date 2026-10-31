/*
 * dcf77_sim.c - PC test bench for the firmware's DCF77 decoder
 *
 * Compiles ../binary_clock/dcf77_decoder.h unchanged (or another header:
 * -DDECODER_H='"path"') and feeds it the way the firmware does: line edges
 * (INT1 ISR), a Timer2 overflow every second (dcf_poll, then the clock with
 * its DST change and leap second, as in the tick ISR), frame decoding
 * outside the ISR, the gate, the wait for the top of the minute and the
 * rewrite of TCNT2.
 *
 *   dcf77_sim synth [minutes] [seed] [spurious pulses/s]
 *       generates a DCF77 signal whose time is known, adds noise, decodes it
 *       like the firmware and checks every loaded time to the second (hour,
 *       minute and position of the top), then, in the middle of every second
 *       after the first load, the displayed time. Exit status 2 if a wrong
 *       time was loaded. Last line:
 *         result: loaded=N wrong=M wait=W rejected=R wrong_display_s=D first_load_s=T
 *       (T = -1: never loaded). See sweep.sh, run_tests.sh and bench.sh.
 *
 *   dcf77_sim board [minutes] [seed] [severity]
 *       same, with the "board" noise model, fitted to recordings of the
 *       receiver on the assembled clock (debug log and oscilloscope captures
 *       of PD3); severity 1 reproduces those recordings: spurious pulses tied
 *       to the DCF77 second (a precursor about 123 ms before the pulse, a
 *       quiet zone after it, a group around +560 ms), episodes of stuck
 *       line, gaps inside the pulses. Parameters BM_* (see board_params()).
 *
 *   dcf77_sim csv <capture.csv> [active-high]
 *       replays an oscilloscope capture of PD3 (Siglent SDS800X HD format:
 *       12 header lines, then "time,volts"). By default the pulse is LOW
 *       (behind the Q_DCF1 inverter). Prints every second.
 *
 * Synthetic model: receiver latency 20 ms, jitter sigma 20 ms; "0" 85-135 ms,
 * "1" 155-270 ms; missed pulses (DROP, 3 %), 3 % clipped by 40-70 ms at the
 * start, 30 % cut by an 8-20 ms gap; Poisson spurious pulses
 * (<spurious pulses/s>) of 5 ms plus an exponential of mean 60 ms, capped at
 * 320 ms. The Timer2 overflow (dcf_poll) has a random phase; on every
 * loaded time the bench does what the firmware does: waits for the top,
 * rewrites TCNT2 (dcf_shift_time), and the overflow then follows the top.
 *
 * Options (environment variables, synth and board modes):
 *   TRACE=1            every second, every vote, every load
 *   FRAMES=1           one line per delivered frame
 *   STAT=1             a "stat" line (positions, frames) before the result
 *   START=hhmm         time of minute 0 (default 2157); DST=1: summer time
 *   LEAP=<m>           (synth) minute m has 61 s (leap second; bit 19
 *                      announced during the 60 preceding minutes)
 *   DSTCHG=<m>         DST change at minute m (+/-1 h depending on DST,
 *                      bit 16 sent during the preceding hour; NOA1=1:
 *                      without announcement)
 *   PPM=<x>            crystal offset of x ppm relative to DCF77
 *   WEAK=<k>:<p>       the pulse of second k is missing with probability p
 *   LOSS=<t>:<s>       no DCF77 pulses from t to t+s seconds, the spurious
 *                      pulses remain (transmitter off, strong interferer)
 *   synth only:
 *   DROP=<p>           probability of a missed pulse (default 0.03)
 *   OUTAGE=<t>:<s>     no signal at all from t to t+s seconds (line idle)
 *   BURST=<t>:<s>:<r>  extra spurious pulses at r/s from t to t+s
 *   JUMP=<t>:<ms>      the pulse phase jumps by <ms> at time t
 *
 * Build: cc -O2 -Wall -Wextra -o dcf77_sim dcf77_sim.c -lm   (or make)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static double   g_t;           // current simulated time (s), for traces
static int      g_trace = 1;   // print every second
static int      g_print_frames;  // one line per frame

#define DCF_HOOK_SECOND(code, pos) hook_second(code, pos)
#define DCF_HOOK_MARKER()          hook_marker()
#define DCF_HOOK_LOCK(l)           hook_lock(l)
#define DCF_HOOK_ACQ(b, o)         do { if (g_trace) printf("  %8.3f  vote %u/%u\n", g_t, (b), (o)); } while (0)
static void hook_second(unsigned code, unsigned pos);
static void hook_marker(void);
static void hook_lock(int event);

#ifndef DECODER_H
#define DECODER_H "../binary_clock/dcf77_decoder.h"
#endif
#include DECODER_H

// ---- time axis (crystal) ---------------------------------------------
// ticks(t) = g_off + 256 * t * (1 + ppm), based near the 16-bit wrap; the
// low byte passes 0 at each Timer2 overflow (set_axis), and g_off changes
// when the bench rewrites TCNT2.
static uint16_t g_off   = 0;
static double   g_scale = 1.0;
static uint16_t ticks_of(double t) { return (uint16_t)(g_off + (uint32_t)floor(t * 256.0 * g_scale + 1e-6)); }

static double next_poll;        // next Timer2 overflow
static void set_axis(double phase)   // first overflow at t = phase (TCNT2 = 0)
{
  next_poll = phase;
  g_off = (uint16_t)(0xF100u - (uint32_t)floor(phase * 256.0 * g_scale + 1e-6));
}
static long   g_clk_sod;        // displayed time (seconds of the day), +1 at each overflow
static int    g_loaded;         // a time has been loaded
static long   st_disp_bad;      // wrong displayed seconds after the first load
static double g_disp_t = -1;    // next display check (middle of the second)
static void check_display(double t);
// Timer2 overflow ISR, on time, whether the line moves or not: dcf_poll,
// then the clock (with the announced DST change and leap second).
static void run_polls(double t)
{
  while (next_poll <= t) {
    g_t = next_poll;
    dcf_poll((uint16_t)(ticks_of(next_poll) & 0xFF00u));   // TCNT2 = 0 at the overflow
    if (g_clk_sod % 3600 == 3599 && dcf_leap_due((uint8_t)(g_clk_sod / 3600))) {
      if (g_trace || g_print_frames) printf("  %8.3f  leap second (59 held)\n", next_poll);
    } else {
      g_clk_sod = (g_clk_sod + 1) % 86400;
    }
    if (g_clk_sod % 3600 == 0) {
      int d = dcf_dst_due((uint8_t)(g_clk_sod / 3600));
      if (d && (g_trace || g_print_frames)) printf("  %8.3f  DST change %+d h\n", next_poll, d);
      g_clk_sod = (g_clk_sod + 86400 + d * 3600) % 86400;
    }
    g_disp_t = next_poll + 0.5 / g_scale;
    next_poll += 1.0 / g_scale;
  }
}
static void feed(double t, bool active)
{
  run_polls(t);
  g_t = t;
  dcf_edge(ticks_of(t), active);
}

// ---- ground truth (synth) ----------------------------------------------
static double       g_t0;       // start of absolute second 0 (= start of minute -1)
static int          g_nsec;     // number of absolute seconds
static signed char *g_lab;      // position 0..60 of each absolute second
static signed char *g_bit;      // 0, 1, 2 = no pulse
static short       *g_min;      // minute index (-1..)
static int          g_start_hm = 21 * 60 + 57;
static double       g_jump_t = -1, g_jump_s = 0;
static int          g_dst = 0;          // summer time at the start
static int          g_dstchg = 1 << 30; // minute of the DST change (never by default)
static int          g_noa1;             // NOA1=1: change without announcement (bit 16 never sent)
static int          g_weak_k = -1;      // WEAK=<k>:<p>: pulse of second k often missing
static double       g_weak_p;
static double       g_loss_t = -1, g_loss_s = 0;   // LOSS=<t>:<s>: no DCF77 pulses, spurious pulses kept
static bool lost(double t) { return g_loss_t >= 0 && t >= g_loss_t && t < g_loss_t + g_loss_s; }
static int dst_shift(int m) { return m >= g_dstchg ? (g_dst ? -60 : 60) : 0; }

static int abs_sec_of(double ts)          // start of a grid second -> absolute second
{
  return (int)floor(ts - g_t0 - (g_jump_t >= 0 && ts >= g_jump_t ? g_jump_s : 0) + 0.5);
}
static int wall_sod(int s)                // true time (seconds of the day) at the start of second s
{
  int hm = ((g_start_hm + g_min[s] + dst_shift(g_min[s])) % 1440 + 1440) % 1440;
  return hm * 60 + g_lab[s];
}

// ---- instrumentation -----------------------------------------------------
static int  g_conf[3][4];               // truth 0/1/none x read 0/1/none/unreadable
static int  st_locks, st_unlocks, st_recenters, st_frames, st_frames_mis;
static long st_locked_sec, st_unknown_sec, st_wrongpos_sec;
static int  g_frame_d = 99;             // offset (s) of the bits of the delivered frame
static int  g_mk_s = -1;                // absolute second that starts at the delivered top

// Displayed second in the middle of the clock second against the true time
// at that instant (a load that follows the tick by 1 ms is not counted).
static void check_display(double t)
{
  if (!g_lab) return;
  int s = (int)floor(t - g_t0 - (g_jump_t >= 0 && t >= g_jump_t ? g_jump_s : 0));
  if (s < 0 || s >= g_nsec || g_lab[s] < 0) return;
  long tr = wall_sod(s) % 86400;
  if (g_lab[s] == 60 && g_clk_sod == (tr + 86399) % 86400) return;   // :60 shown as 59 (held)
  if (g_clk_sod != tr) {
    st_disp_bad++;
    if (g_trace || (g_print_frames && st_disp_bad % 60 == 1))
      printf("  %8.3f  displayed %02ld:%02ld:%02ld, true %02d:%02d:%02d\n", t,
             g_clk_sod / 3600, g_clk_sod / 60 % 60, g_clk_sod % 60,
             wall_sod(s) % 86400 / 3600, wall_sod(s) / 60 % 60, wall_sod(s) % 60);
  }
}

static void hook_second(unsigned code, unsigned pos)
{
  double ts = g_t - (uint16_t)(ticks_of(g_t) - dcf_sec_ticks) / (256.0 * g_scale);
  int s = abs_sec_of(ts);
  int lab = (g_lab && s >= 0 && s < g_nsec) ? g_lab[s] : -1;
  if (lab >= 0) {
    g_conf[(int)g_bit[s]][code == DCF_NONE ? 2 : code == DCF_BAD ? 3 : code >= dcf_thr ? 1 : 0]++;
    st_locked_sec++;
    if (pos == 255)        st_unknown_sec++;
    else if ((int)pos != lab) st_wrongpos_sec++;
    if (pos == 59) { int d = lab - 59; if (d < -30) d += 60; g_frame_d = d; }
  }
  if (!g_trace) return;
  char bit[8];
  if (code == DCF_NONE)     strcpy(bit, "-");
  else if (code == DCF_BAD) strcpy(bit, "?");
  else                      sprintf(bit, "%c", code >= dcf_thr ? '1' : '0');
  printf("  %8.3f  A%3u B%3u C%3u  %s  s%s", g_t, dcf_a, dcf_b, dcf_c, bit, pos == 255 ? "-" : "");
  if (pos != 255) printf("%u", pos);
  if (lab >= 0) printf("   [true %d]", lab);
  printf("\n");
}
static void hook_marker(void)
{
  double tmk = g_t + (int16_t)(dcf_marker_ticks - ticks_of(g_t)) / (256.0 * g_scale);
  g_mk_s = abs_sec_of(tmk - 0.020);       // top = mean pulse start = true top + 20 ms
  if (g_trace) printf("  %8.3f  MIN\n", g_t);
}
// Phase error of the grid against the ideal phase (second starting 39 ms
// before the mean pulse start, receiver latency 20 ms), in s, or 9 if the
// truth is unknown. Good grid: |error| < 30 ms.
static int st_lock_good, st_lock_bad, st_rec[2][2], st_wd_good, st_wd_bad, st_move_good, st_move_bad;
static double grid_error(void)
{
  if (!g_lab) return 9;
  double ts = g_t - (uint16_t)(ticks_of(g_t) - dcf_sec_ticks) / (256.0 * g_scale);
  double ideal = g_t0 + 0.020 - 0.039 + (g_jump_t >= 0 && ts >= g_jump_t ? g_jump_s : 0);
  double e = fmod(ts - ideal, 1.0);
  if (e > 0.5) e -= 1;
  if (e <= -0.5) e += 1;
  return e;
}
static void hook_lock(int ev)
{
  if (ev == 2) st_recenters++; else if (ev) st_locks++; else st_unlocks++;
  double e = grid_error();
  if (e < 9) {
    int good = fabs(e) < 0.030;
    if (ev == 1) { if (good) st_lock_good++; else st_lock_bad++; }
    else if (ev == 2) {
      double e2 = e + dcf_recenter / 256.0;
      st_rec[good][fabs(e2) < 0.030]++;
    } else if (!dcf_locked) { if (good) st_wd_good++; else st_wd_bad++; }   // watchdog
    else { if (good) st_move_good++; else st_move_bad++; }                 // grid moved
  }
  if (g_trace) printf("  %8.3f  %s (offset %+.0f ms)\n", g_t, ev == 2 ? "RECENTER" : ev ? "LOCK" : "UNLOCK", 1000 * e);
}

// ---- main loop: decode_and_load_dcf77_frame() -----------------------------
static int    n_ok, n_wrong_ok, n_wait, n_err, n_late, n_gate;
static double t_first_ok = -1;
static int    g_load_pending;           // time to load at the top (like the firmware)
static double g_load_t;
static uint8_t g_load_h, g_load_m;
static int    g_load_check, g_load_mk;

static void main_loop(double t)
{
  if (!flag_frame_ready) return;
  flag_frame_ready = false;
  int d = g_frame_d, mk = g_mk_s;
  g_frame_d = 99;
  uint8_t h = 0, m = 0;
  const char *err = dcf_accumulate(dcf_frame_done, (uint32_t)t, &h, &m);
  st_frames++;
  if (g_lab && d != 0) st_frames_mis++;
  if (g_print_frames || g_trace) {
    printf("  %8.3f  frame%s -> %s %02u:%02u margin %u/%u", t, d && g_lab ? " shifted" : "",
           err ? err : "OK", h, m, dcf_margin_m, dcf_margin_h);
    printf("  A1 %d %s%s", dcf_a1, dcf_zone ? "CEST" : "CET", dcf_dst_arm ? " armed" : "");
    printf("\n");
  }
  if (err && strcmp(err, "wait")) { n_err++; return; }
  if (err) { n_wait++; return; }
  // Firmware gate: a time different from the clock must be confirmed by a
  // second frame. The clock is "at the top" only if sw_sec is 59, 0 or 1.
  int clock_hm = dcf_clock_hm((uint8_t)(g_clk_sod / 3600), (uint8_t)(g_clk_sod / 60 % 60), (uint8_t)(g_clk_sod % 60));
  if (dcf_gate(h, m, clock_hm)) {
    n_gate++;
    if (g_print_frames || g_trace) printf("  %8.3f  gate: %02u:%02u to be confirmed\n", t, h, m);
    return;
  }
  // The firmware waits for the top (the grid precedes it by DCF_GUARD), then loads.
  int16_t early = (int16_t)(dcf_marker_ticks - ticks_of(t));
  g_load_pending = 1;
  g_load_t  = early > 0 ? t + early / (256.0 * g_scale) : t;
  g_load_h  = h; g_load_m = m;
  g_load_check = g_lab != NULL; g_load_mk = mk;
}

// Load: TCNT2 rewrite, as in decode_and_load_dcf77_frame().
static void apply_load(void)
{
  g_load_pending = 0;
  uint16_t now = ticks_of(g_load_t);
  uint16_t elapsed = (uint16_t)(now - dcf_marker_ticks);
  if (elapsed >= 250) { n_late++; if (g_trace) printf("  %8.3f  SYNC late\n", g_load_t); return; }
  uint16_t new_now = (uint16_t)((now & 0xFF00u) | elapsed);
  uint16_t delta = (uint16_t)(new_now - now);
  g_off = (uint16_t)(g_off + delta);
  dcf_shift_time(delta);
  next_poll = g_load_t + (256 - elapsed) / (256.0 * g_scale);
  g_clk_sod = (g_load_h * 60 + g_load_m) * 60L;   // sw_sec = 0 at the top
  g_loaded = 1;
  g_disp_t = -1;                          // check scheduled on the old phase: void
  n_ok++;
  if (t_first_ok < 0) t_first_ok = g_load_t;
  int e = 0;
  if (g_load_check) {                     // h:m:00 displayed at the top vs true time of that second
    int mk = g_load_mk;
    e = (mk >= 0 && mk < g_nsec && g_lab[mk] >= 0)
        ? (g_load_h * 60 + g_load_m) * 60 - wall_sod(mk) : 99999;
    if (mk >= 0 && mk < g_nsec && g_lab[mk] == 60) e += 1;   // second 60: not 00 of the next minute
    while (e > 43200) e -= 86400;
    while (e < -43200) e += 86400;
  }
  if (e) n_wrong_ok++;
  if (e || g_trace)
    printf("  %8.3f  SYNC OK %02u:%02u  margin %u/%u%s", g_load_t, g_load_h, g_load_m,
           dcf_margin_m, dcf_margin_h, e ? "  <<< WRONG" : "");
  if (e) printf(" by %+d s", e);
  if (e || g_trace) printf("\n");
}

static void step(double t, bool active, int *prev)
{
  if (g_load_pending && t >= g_load_t) apply_load();
  run_polls(t);
  if (active != *prev) { feed(t, active); *prev = active; }
  main_loop(t);
  if (g_disp_t >= 0 && t >= g_disp_t) { if (g_loaded) check_display(g_disp_t); g_disp_t = -1; }
}

// ---- capture replay ------------------------------------------------------
static int run_csv(const char *path, bool active_high)
{
  FILE *fp = fopen(path, "r");
  if (!fp) { perror(path); return 1; }
  char line[256];
  for (int i = 0; i < 12 && fgets(line, sizeof line, fp); i++) ;
  double t, v, t0 = NAN;
  int prev = -1;
  set_axis(0.37);
  while (fgets(line, sizeof line, fp)) {
    if (sscanf(line, "%lf,%lf", &t, &v) != 2) continue;
    if (isnan(t0)) t0 = t;
    step(t - t0, (v > 2.5) == active_high, &prev);
  }
  fclose(fp);
  return 0;
}

// ---- synthetic signal ------------------------------------------------------
static void encode_zone(int hh, int mm, int bits[59], int dst, int a1);
static double urand(void) { return rand() / (RAND_MAX + 1.0); }
static double nrand(void) { return sqrt(-2 * log(urand() + 1e-12)) * cos(2 * M_PI * urand()); }

// Frame for hh:mm on a fixed date; dst: summer time; a1: DST change
// announced (bit 16).
static void encode_zone(int hh, int mm, int bits[59], int dst, int a1)
{
  memset(bits, 0, 59 * sizeof bits[0]);
  for (int i = 1; i <= 14; i++) bits[i] = rand() & 1;   // weather bits: arbitrary
  bits[16] = a1;
  if (dst) bits[17] = 1; else bits[18] = 1;
  bits[20] = 1;
  int p;
  int mb = (mm / 10) << 4 | mm % 10, hb = (hh / 10) << 4 | hh % 10;
  p = 0; for (int i = 0; i < 7; i++) { bits[21 + i] = mb >> i & 1; p ^= bits[21 + i]; } bits[28] = p;
  p = 0; for (int i = 0; i < 6; i++) { bits[29 + i] = hb >> i & 1; p ^= bits[29 + i]; } bits[35] = p;
  int date[22] = {1,0,0,1,1,0, 0,0,1, 0,0,0,0,1, 0,1,1,0,0,1,0,0};
  p = 0; for (int i = 0; i < 22; i++) { bits[36 + i] = date[i]; p ^= date[i]; } bits[58] = p;
}

// Frame sent during minute m: time of minute m + 1 (DST change included).
// Bit 16 is sent from 01:00:16 to 01:59:16 CET (02:00:16 to 02:59:16 CEST):
// frames of the next 60 minutes, the last one already giving the new time.
static void frame_bits(int m, int bits[59])
{
  int n = m + 1, hm = ((g_start_hm + n + dst_shift(n)) % 1440 + 1440) % 1440;
  int dst = n >= g_dstchg ? !g_dst : g_dst;
  encode_zone(hm / 60, hm % 60, bits, dst, !g_noa1 && n <= g_dstchg && n > g_dstchg - 60);
}

static void add_noise(unsigned char *act, int total_ms, double t0, double t1, double rate)
{
  for (double t = t0; rate > 0 && t < t1; ) {
    t += -log(urand() + 1e-12) / rate;
    double d = 0.005 + fmin(0.315, -0.060 * log(urand() + 1e-12));
    for (int i = (int)(t * 1000); i < (int)((t + d) * 1000) && i < total_ms; i++) act[i] = 1;
  }
}

static int finish(unsigned char *act, int total_ms);
static int run_synth(int minutes, unsigned seed, double noise)
{
  srand(seed);
  const char *e;
  int leap = (e = getenv("LEAP")) ? atoi(e) : -1000;
  if ((e = getenv("START"))) { int v = atoi(e); g_start_hm = v / 100 * 60 + v % 100; }
  g_dst = getenv("DST") && atoi(getenv("DST"));
  if ((e = getenv("DSTCHG"))) g_dstchg = atoi(e);
  g_noa1 = getenv("NOA1") && atoi(getenv("NOA1"));
  if ((e = getenv("WEAK"))) sscanf(e, "%d:%lf", &g_weak_k, &g_weak_p);
  if ((e = getenv("LOSS"))) sscanf(e, "%lf:%lf", &g_loss_t, &g_loss_s);
  double drop = (e = getenv("DROP")) ? atof(e) : 0.03;
  double out_t = -1, out_len = 0, bu_t = -1, bu_len = 0, bu_rate = 0;
  if ((e = getenv("OUTAGE"))) sscanf(e, "%lf:%lf", &out_t, &out_len);
  if ((e = getenv("BURST")))  sscanf(e, "%lf:%lf:%lf", &bu_t, &bu_len, &bu_rate);
  if ((e = getenv("PPM")))    g_scale = 1.0 + atof(e) * 1e-6;
  if ((e = getenv("JUMP")))   { double ms = 0; sscanf(e, "%lf:%lf", &g_jump_t, &ms); g_jump_s = ms / 1000.0; }

  double lead = 23.4 + 10 * urand();      // the signal starts in the middle of a minute
  set_axis(urand());                      // phase of the Timer2 overflow
  int extra = (leap >= -1 && leap < minutes) ? 1 : 0;
  int total_ms = (int)(lead * 1000) + minutes * 60000 + extra * 1000 + (g_jump_s > 0 ? (int)(g_jump_s * 1000) + 1 : 0);
  unsigned char *act = calloc(total_ms, 1);
  int pulses = 0, dropped = 0, cut = 0;
  g_t0   = lead - 60;
  g_nsec = (minutes + 2) * 60 + extra + 2;
  g_lab  = malloc(g_nsec); g_bit = malloc(g_nsec); g_min = malloc(g_nsec * sizeof *g_min);
  memset(g_lab, -1, g_nsec); memset(g_bit, -1, g_nsec);
  for (int i = 0; i < g_nsec; i++) g_min[i] = -2;

  // Real pulses: second k of minute m starts at lead + 60 m + k (minute -1
  // is partial). The frame sent during a minute gives the time of the next
  // minute.
  for (int m = -1; m < minutes; m++) {
    int bits[59];
    frame_bits(m, bits);
    if (leap >= -1 && m <= leap && m >= leap - 59) bits[19] = 1;
    int sh = (leap >= -1 && m > leap) ? 1 : 0;          // seconds inserted before this minute
    int nk = m == leap ? 61 : 60;
    int s0 = 60 * (m + 1) + sh;
    for (int k = 0; k < nk; k++) {
      int b = k < 59 ? bits[k] : (k == 59 && nk == 61) ? 0 : 2;
      g_lab[s0 + k] = (signed char)k; g_bit[s0 + k] = (signed char)b; g_min[s0 + k] = (short)m;
    }
    for (int k = 0; k < nk - 1; k++) {
      int bit = k < 59 ? bits[k] : 0;
      double t = lead + 60.0 * m + sh + k + 0.020 + 0.020 * nrand();
      if (g_jump_t >= 0 && t >= g_jump_t) t += g_jump_s;
      if (t < 0 || lost(t)) continue;
      pulses++;
      double w = bit ? 0.155 + 0.115 * urand() : 0.085 + 0.050 * urand();
      double r = urand();
      if (r < drop) { dropped++; continue; }                                   // missed pulse
      if (k == g_weak_k && urand() < g_weak_p) { dropped++; continue; }        // weak second
      if (r < drop + 0.03) { t += 0.040 + 0.030 * urand(); w -= 0.05; cut++; }  // start clipped
      int a = (int)(t * 1000), b = (int)((t + w) * 1000);
      for (int i = a; i < b && i < total_ms; i++) act[i] = 1;
      if (urand() < 0.3) {                                                      // 8-20 ms gap
        int g = a + (int)((b - a) * urand()), gl = 8 + rand() % 13;
        for (int i = g; i < g + gl && i < b; i++) act[i] = 0;
      }
    }
  }
  {   // positions of the seconds after the last minute
    int sh = (leap >= -1 && minutes > leap) ? 1 : 0, s0 = 60 * (minutes + 1) + sh;
    for (int k = 0; k < 60 && s0 + k < g_nsec; k++) { g_lab[s0 + k] = (signed char)k; g_bit[s0 + k] = 2; g_min[s0 + k] = (short)minutes; }
  }
  add_noise(act, total_ms, 0, total_ms / 1000.0, noise);
  if (bu_t >= 0) add_noise(act, total_ms, bu_t, bu_t + bu_len, bu_rate);
  if (out_t >= 0)
    for (int i = (int)(out_t * 1000); i < (int)((out_t + out_len) * 1000) && i < total_ms; i++) act[i] = 0;
  printf("synth: %d min, seed %u, %.1f spurious pulses/s, %d pulses, %d missed, %d clipped\n",
         minutes, seed, noise, pulses, dropped, cut);
  return finish(act, total_ms);
}

static int finish(unsigned char *act, int total_ms)
{
  int prev = -1;
  for (int i = 0; i < total_ms; i++) step(i / 1000.0, act[i], &prev);
  free(act);
  printf("seconds (truth -> read 0/1/none/unreadable): 0 -> %d/%d/%d/%d  1 -> %d/%d/%d/%d  none -> %d/%d/%d/%d\n",
         g_conf[0][0], g_conf[0][1], g_conf[0][2], g_conf[0][3],
         g_conf[1][0], g_conf[1][1], g_conf[1][2], g_conf[1][3],
         g_conf[2][0], g_conf[2][1], g_conf[2][2], g_conf[2][3]);
  if (getenv("STAT"))
    printf("stat locks %d unlocks %d recenters %d frames %d shifted %d locked_s %ld "
           "unknown_pos_s %ld wrong_pos_s %ld late %d gate %d "
           "good_locks %d bad_locks %d recenter_good_good %d good_bad %d bad_good %d bad_bad %d "
           "watchdog_good %d watchdog_bad %d move_good %d move_bad %d\n",
           st_locks, st_unlocks, st_recenters, st_frames, st_frames_mis, st_locked_sec,
           st_unknown_sec, st_wrongpos_sec, n_late, n_gate,
           st_lock_good, st_lock_bad, st_rec[1][1], st_rec[1][0], st_rec[0][1], st_rec[0][0],
           st_wd_good, st_wd_bad, st_move_good, st_move_bad);
  printf("result: loaded=%d wrong=%d wait=%d rejected=%d wrong_display_s=%ld first_load_s=%.0f\n",
         n_ok, n_wrong_ok, n_wait, n_err, st_disp_bad, t_first_ok);
  return n_wrong_ok ? 2 : 0;
}

// ---- "board" model ---------------------------------------------------------
// Fitted to 29 statistics of a debug log recorded on the assembled clock
// and to the votes of start-ups that never locked; severity 1 = that log.
// Pulses: low start jitter, Poisson gaps, missed or clipped pulses.
// Spurious pulses: short Poisson ones, episodes of stuck line, a precursor
// about 123 ms before the pulse start (merges with it and moves it
// earlier), a quiet zone after the end of the pulse (rate x 0.54 for
// 300 ms), a group around +560 ms into the DCF77 second.
static double envd(const char *name, double def)
{
  const char *v = getenv(name);
  return v && *v ? atof(v) : def;
}
static double erand(double mean) { return -mean * log(urand() + 1e-12); }
static void set_on(unsigned char *act, int total_ms, double t0, double t1, int v)
{
  for (int i = (int)(t0 * 1000); i < (int)(t1 * 1000) && i < total_ms; i++) if (i >= 0) act[i] = (unsigned char)v;
}

typedef struct {
  double lat, jit, w0a, w0b, w1a, w1b, drop, trunc;
  double gap_rate, gap_min, gap_mean, gap_max;   // gaps: per second of pulse; length (ms)
  double ns_rate, ns_mean, ns_cap;               // short spurious pulses: /s, 5 + exp(mean) ms, cap
  double nl_rate, nl_a, nl_b, nl_gap;            // stuck line: /s, length U(a, b) ms, gaps /s
  double quiet_keep, quiet_len;                  // after a pulse: spurious pulses kept with prob. keep for len s
  double pre_p, pre_mu, pre_sig, pre_mean;       // precursor: probability, position (s), sigma (s), mean length (ms)
  double per_p, per_ph, per_sig, per_mean, per_cap;   // group locked to the DCF77 second
} board_t;

static board_t board_params(double sev)
{
  board_t b;
  b.lat = envd("BM_LAT", 0.020);   b.jit = envd("BM_JIT", 0.014);
  b.w0a = envd("BM_W0A", 0.080);   b.w0b = envd("BM_W0B", 0.135);
  b.w1a = envd("BM_W1A", 0.143);   b.w1b = envd("BM_W1B", 0.315);
  b.drop  = envd("BM_DROP", 0.102) * sev;
  b.trunc = envd("BM_TRUNC", 0.067) * sev;
  b.gap_rate = envd("BM_GAPR", 3.05) * sev;  b.gap_min = envd("BM_GAPMIN", 5);
  b.gap_mean = envd("BM_GAPM", 67);          b.gap_max = envd("BM_GAPMAX", 80);
  b.ns_rate  = envd("BM_NS", 7.0) * sev;     b.ns_mean = envd("BM_NSM", 21);   b.ns_cap = envd("BM_NSC", 320);
  b.nl_rate  = envd("BM_NL", 0.243) * sev;   b.nl_a = envd("BM_NLA", 271);     b.nl_b = envd("BM_NLB", 1177);
  b.nl_gap   = envd("BM_NLG", 13.8);
  b.quiet_keep = envd("BM_QKEEP", 0.54);     b.quiet_len = envd("BM_QLEN", 0.300);
  b.pre_p  = fmin(1.0, envd("BM_PREP", 0.3) * sev);
  b.pre_mu = envd("BM_PREMU", -0.123);       b.pre_sig = envd("BM_PRESIG", 0.024);  b.pre_mean = envd("BM_PREM", 200);
  b.per_p  = fmin(1.0, envd("BM_PERP", 1.0) * sev);
  b.per_ph = envd("BM_PERPH", 0.560);        b.per_sig = envd("BM_PERSIG", 0.039);
  b.per_mean = envd("BM_PERM", 107);         b.per_cap = envd("BM_PERC", 500);
  return b;
}

static double *g_pend;                  // end of the pulse of each absolute second (-1: none)
static bool quiet_drop(double t, const board_t *B)
{
  int n = (int)floor(t - g_t0 - B->lat);
  if (n < 0 || n >= g_nsec || g_pend[n] < 0) return false;
  double e = g_pend[n];
  return t >= e && t < e + B->quiet_len && urand() >= B->quiet_keep;
}

static int run_board(int minutes, unsigned seed, double sev)
{
  srand(seed);
  const char *e;
  if ((e = getenv("START"))) { int v = atoi(e); g_start_hm = v / 100 * 60 + v % 100; }
  g_dst = getenv("DST") && atoi(getenv("DST"));
  if ((e = getenv("DSTCHG"))) g_dstchg = atoi(e);
  g_noa1 = getenv("NOA1") && atoi(getenv("NOA1"));
  if ((e = getenv("WEAK"))) sscanf(e, "%d:%lf", &g_weak_k, &g_weak_p);
  if ((e = getenv("LOSS"))) sscanf(e, "%lf:%lf", &g_loss_t, &g_loss_s);
  if ((e = getenv("PPM")))    g_scale = 1.0 + atof(e) * 1e-6;
  board_t B = board_params(sev);
  double lead = 23.4 + 10 * urand();
  set_axis(urand());
  int total_ms = (int)(lead * 1000) + minutes * 60000;
  unsigned char *act = calloc(total_ms, 1);
  int pulses = 0, dropped = 0, cut = 0, gaps = 0;
  g_t0   = lead - 60;
  g_nsec = (minutes + 2) * 60 + 2;
  g_lab  = malloc(g_nsec); g_bit = malloc(g_nsec); g_min = malloc(g_nsec * sizeof *g_min);
  g_pend = malloc(g_nsec * sizeof *g_pend);
  memset(g_lab, -1, g_nsec); memset(g_bit, -1, g_nsec);
  for (int i = 0; i < g_nsec; i++) { g_min[i] = -2; g_pend[i] = -1; }
  for (int m = -1; m <= minutes; m++) {
    int bits[59];
    frame_bits(m, bits);
    int s0 = 60 * (m + 1);
    for (int k = 0; k < 60 && s0 + k < g_nsec; k++) {
      g_lab[s0 + k] = (signed char)k; g_bit[s0 + k] = (signed char)(k < 59 ? bits[k] : 2); g_min[s0 + k] = (short)m;
    }
    if (m == minutes) break;
    for (int k = 0; k < 59; k++) {
      double t = lead + 60.0 * m + k + B.lat + B.jit * nrand();
      double w = bits[k] ? B.w1a + (B.w1b - B.w1a) * urand() : B.w0a + (B.w0b - B.w0a) * urand();
      double r = urand();
      if (t < 0 || lost(t)) continue;
      pulses++;
      if (r < B.drop) { dropped++; continue; }
      if (k == g_weak_k && urand() < g_weak_p) { dropped++; continue; }
      if (r < B.drop + B.trunc) { double d = 0.040 + 0.030 * urand(); t += d; w -= d; cut++; }
      if (w < 0.01) continue;
      set_on(act, total_ms, t, t + w, 1);
      g_pend[s0 + k] = t + w;
      for (double tg = t;;) {                       // gaps inside the pulse
        tg += erand(1.0 / B.gap_rate);
        if (tg >= t + w) break;
        set_on(act, total_ms, tg, fmin(tg + fmin(B.gap_max, B.gap_min + erand(B.gap_mean)) / 1000.0, t + w), 0);
        gaps++;
      }
    }
  }
  double tot = total_ms / 1000.0;
  for (double t = 0; B.ns_rate > 0 && t < tot; ) {  // short spurious pulses
    t += erand(1.0 / B.ns_rate);
    double d = fmin(B.ns_cap, 5 + erand(B.ns_mean)) / 1000.0;
    if (!quiet_drop(t, &B)) set_on(act, total_ms, t, t + d, 1);
  }
  for (double t = 0; B.nl_rate > 0 && t < tot; ) {  // stuck line
    t += erand(1.0 / B.nl_rate);
    double d = (B.nl_a + (B.nl_b - B.nl_a) * urand()) / 1000.0;
    if (quiet_drop(t, &B)) continue;
    set_on(act, total_ms, t, t + d, 1);
    for (double tg = t; B.nl_gap > 0; ) {
      tg += erand(1.0 / B.nl_gap);
      if (tg >= t + d) break;
      set_on(act, total_ms, tg, fmin(tg + (3 + 12 * urand()) / 1000.0, t + d), 0);
    }
  }
  for (int n = 0; n < g_nsec; n++) {                // groups locked to the DCF77 second
    double t0 = g_t0 + n + B.lat;
    if (lost(t0)) continue;                          // they follow the carrier
    if (n % 60 != 59 && urand() < B.pre_p) {
      double t = t0 + B.pre_mu + B.pre_sig * nrand();
      set_on(act, total_ms, t, t + (5 + erand(B.pre_mean)) / 1000.0, 1);
    }
    if (urand() < B.per_p) {
      double t = t0 + B.per_ph + B.per_sig * nrand();
      set_on(act, total_ms, t, t + fmin(B.per_cap, 5 + erand(B.per_mean)) / 1000.0, 1);
    }
  }
  free(g_pend);
  printf("board: %d min, seed %u, severity %.2f, %d pulses, %d missed, %d clipped, %d gaps\n",
         minutes, seed, sev, pulses, dropped, cut, gaps);
  return finish(act, total_ms);
}

int main(int argc, char **argv)
{
  if (argc >= 3 && !strcmp(argv[1], "csv"))
    return run_csv(argv[2], argc >= 4 && !strcmp(argv[3], "active-high"));
  if (argc >= 2 && (!strcmp(argv[1], "synth") || !strcmp(argv[1], "board"))) {
    g_trace = getenv("TRACE") != NULL;
    g_print_frames = getenv("FRAMES") != NULL;
    int minutes = argc >= 3 ? atoi(argv[2]) : 10;
    unsigned seed = argc >= 4 ? (unsigned)atoi(argv[3]) : 1;
    if (!strcmp(argv[1], "board")) return run_board(minutes, seed, argc >= 5 ? atof(argv[4]) : 1.0);
    return run_synth(minutes, seed, argc >= 5 ? atof(argv[4]) : 1.8);
  }
  fprintf(stderr, "usage: %s synth [minutes] [seed] [spurious pulses/s]\n"
                  "       %s board [minutes] [seed] [severity]\n"
                  "       %s csv <capture.csv> [active-high]\n", argv[0], argv[0], argv[0]);
  return 1;
}
