# DCF77 decoder

The decoder, [`firmware/binary_clock/dcf77_decoder.h`](../firmware/binary_clock/dcf77_decoder.h),
turns the output of the DCF77 receiver into the time shown by the clock.
Its design goal: **never display a wrong time**, and synchronize as fast
as the signal allows. It has no hardware dependency; the firmware calls it
from its interrupts, and the [simulator](../firmware/test) compiles the
same file on a PC.

- [The signal](#the-signal)
- [Why a robust decoder](#why-a-robust-decoder)
- [Second grid](#second-grid)
- [Reading each second](#reading-each-second)
- [Position in the minute](#position-in-the-minute)
- [Decoding over several minutes](#decoding-over-several-minutes)
- [Gate](#gate)
- [DST changes and leap seconds](#dst-changes-and-leap-seconds)
- [Loading the time](#loading-the-time)
- [Debug output](#debug-output)
- [Validation](#validation)

## The signal

DCF77 broadcasts German legal time on 77.5 kHz from Mainflingen, near
Frankfurt. At the start of every second the carrier is reduced for 100 ms
(bit 0) or 200 ms (bit 1); there is no reduction at second 59, which marks
the minute. The receiver outputs one pulse per reduction. A frame of 59
bits, sent during one minute, gives the time of the following minute:

| Bits | Content |
|------|---------|
| 0 | start of minute, always 0 |
| 1–14 | weather and civil protection data (not used) |
| 15 | call bit (not used) |
| 16 | A1: DST change at the end of this hour |
| 17, 18 | time zone: 17 = CEST (UTC+2), 18 = CET (UTC+1) |
| 19 | A2: leap second at the end of this hour |
| 20 | start of time, always 1 |
| 21–27, 28 | minutes, BCD, and even parity |
| 29–34, 35 | hours, BCD, and even parity |
| 36–57, 58 | date (not used) and even parity |
| 59 | no pulse: minute marker |

## Why a robust decoder

On the clock the receiver sits a few centimeters from the electronics.
Besides the time pulses, its output carries 2 to 8 spurious pulses per
second (5 to 320 ms long), episodes where the line sticks active for up
to a second, and time pulses that are cut by short gaps, clipped or
missing. A quarter of the bits or more read wrong.

A textbook decoder measures each pulse, checks the parity bits and
accepts two consistent frames. Under these conditions it almost never
sees a valid frame, and since three parity bits are all that protect the
result, it occasionally accepts a wrong one. This decoder instead:

1. locks a grid of seconds on the crystal time base, so that a missed or
   spurious pulse never shifts the frame;
2. gives each second a weight, the log-likelihood of "1" against "0",
   instead of a hard decision;
3. finds the minute marker from the history of many minutes;
4. accumulates the frames over several minutes before it trusts a time;
5. changes the time of a running clock only when a second frame confirms
   it, except for a DST change.

## Second grid

Time is measured in ticks of 1/256 s (3.9 ms), from Timer2.

**Phase vote.** Each pulse 47 to 312 ms long, joined across gaps shorter
than 23 ms, votes for the phase of its start in one of 32 bins of 8 ticks.
Every 8 seconds the three adjacent bins with the most votes win if they
hold at least 6 votes and twice as many as the best other triplet; the
votes are then halved, which gives the vote a sliding memory. Real pulses
start every second at the same phase; spurious pulses spread over all
phases.

**Lock and tracking.** The grid starts 39 ms before the mean pulse start
and every second lasts exactly 256 ticks. The phase is *confirmed* while
the bins around it keep receiving at least 7 votes (the confirmation
lasts 64 s). While confirmed, the grid follows the average offset of
pulse starts that fall within ±47 ms, by at most 2 ticks per correction.
A clear vote 47 to 94 ms away recenters the grid; a vote farther away
moves it, unless second 59 has been found, since only the true DCF77
phase has a second 59.

**Protection.** Once a frame has given confident minutes, the grid is
protected for 20 minutes: it moves only for an overwhelming vote (12 votes
or more and three times the next triplet). Without this, a group of
delayed or spurious pulses winning a weak vote would pull a correct grid
out of phase.

**Watchdog.** A grid that stays locked for 12 minutes without finding a
clear second 59 has locked onto something regular that is not DCF77: it
is dropped and its phase is excluded from the vote for 4 minutes.

## Reading each second

Every second is measured in fixed windows relative to the expected start
of its pulse:

| Window | Position | Meaning |
|--------|----------|---------|
| A | −39 to +100 ms | a pulse is present (26 ticks for a full pulse) |
| BW | +113 to +199 ms | second half of a "1" (empty for a "0") |
| C | +290 ms to the end of the second | noise |

plus the *code*: the length beyond 100 ms of the pulse that starts in A,
joined across gaps shorter than 23 ms (0 to 38 ticks). A second with less
than 6 ticks (23 ms) active in A has no pulse; one with 6 to 12 ticks (23
to 51 ms), or with the line active more than half of C, is unreadable. A
second is *noisy* when A is weak or full, or C more than 25 % active.

The weight of the second is the log-likelihood ratio
ln P(measurement | 1) / P(measurement | 0), in half-nats, looked up in
`dcf_wtab` by class (clean, noisy, A full, A weak), code and BW activity.
The table was measured with the simulator under six noise conditions,
keeping the most cautious value of each cell and 0 wherever the sign is
not consistent. A "1" weighs at most +3 and a "0" down to −5: noise turns
a "0" into a "1" (by lengthening it) far more often than the reverse. A
second without a pulse weighs 0; an unreadable one keeps the small weight
of its table cell, and a run of more than 8 seconds without a readable
pulse stops the frame (see below). Thanks to the fixed grid, no second,
however damaged, shifts the ones that follow.

## Position in the minute

The seconds go into the 60 slots of a ring indexed by a counter modulo 60.
Each slot keeps a score from 0 to 15 that rises when window A is empty
(+3 for a clean gap) and falls slightly when it holds a pulse; a clear "0"
read 21 slots later lowers it by 2, since bit 20 is always 1. Second 59 is
the slot that reaches 6 and leads all others by 4. As long as no slot
qualifies, no frame is delivered: a missed pulse is never mistaken for
the minute marker. Once a frame read at that position has given confident
minutes, the slot is kept for as long as the grid is protected.

At the end of second 59 the decoder delivers the frame, the 59 weights of
the minute, provided the phase is confirmed (or the grid protected) and
the minute contains no run of more than 8 unreadable seconds. It also
delivers the number of noisy seconds and the time of the start of second
0, the top of the minute.

## Decoding over several minutes

The minutes field and the hours field are decoded separately, by scoring
every possible value: 60 minute values and 48 hour values (24 hours in
each of CET and CEST). For each frame, a value gains the weight of every
bit it has set in its BCD code and parity and loses the weight of every bit
it has clear; the zone bits 17 and 18 count for the hours. Between two
frames the scores move with the time: the minutes rotate by the number of
minutes elapsed, and each hour score follows its best path across the
change of hour, so that an uncertain minute near :59 does not commit the
hour. Scores are kept relative to the best value and bounded, so that a
false history fades out.

A time is *confident* after at least two frames, when the best minute
leads the runner-up by 28 (about 7 nats, a likelihood ratio of about
1,000 to 1) and the best hour by 32 (hours change only once per hour, so
systematic errors build up there).

Before scoring, a frame goes through these checks:

- frames with many noisy seconds count less (×¾ from 46, ×½ from 50) and
  are ignored from 54 noisy seconds out of 59; clean frames count ×1.5;
- bit 0 = 0, bit 20 = 1 and exactly one of bits 17 and 18: the frame is
  rejected if two of the three checks fail;
- its top must come a whole number of minutes (±1 s) after the previous
  frame; two out-of-phase frames that agree with each other start a new
  history;
- after more than two hours without a frame the scores start over.

## Gate

A confident time is loaded at once if it equals the time on the clock
(the clock shows second 59, 0 or 1 at that moment); this re-aligns the
phase of the clock every minute. A confident time that differs from the
clock is only loaded if the next confident frame confirms it. A single
wrong decode can therefore never overwrite a correct clock. The one
exception is a DST change: when the clock still shows 02:xx CET (or
03:xx CEST) and the decoder sees exactly one hour more (or less) in the
other zone, the new time is loaded immediately.

While the grid is protected, a frame whose phase no longer receives votes
is still delivered, because in heavy noise the votes can stay low for
minutes with the grid exactly right. Such a frame adds to the scores, but
can only re-align a clock that already shows its time; it never changes
the time, the zone or the announcements.

## DST changes and leap seconds

**DST change.** Bit 16 is set during the hour before the change. The
decoder sums its weights over the hour (bounded to 0..60); in the
simulator, noise alone never brings the sum above 14, while a real
announcement reaches 13 to 60. When the scores pass the end of 01 h CET
(02 h CEST), both branches stay alive, the normal hour in the same zone
and the changed hour in the other zone, weighted by the announcement.
If the sum reaches 24, the clock itself changes at the top of the hour,
even when the frame of that minute is unreadable. Otherwise the gate's
exception loads the new time from the first confident frame.

**Leap second.** Bit 19 is set during the hour before a leap second,
which is inserted after 00:59:59 CET (01:59:59 CEST): that minute has 61
seconds. When the announcement has been read clearly, the clock holds
second 59 for two seconds, the frame of the long minute is ignored and the
position in the minute is learned again. If the announcement was missed,
the next frame betrays the leap second by a pulse in place of the marker
gap, and the clock, one second fast, is re-aligned by the next confident
frame.

## Loading the time

The grid starts 39 ms before the top of the minute, so the frame can be
complete slightly early; the firmware waits for the top, then, with
interrupts disabled, loads the counters and rewrites `TCNT2` with the
number of ticks elapsed since the top: the next tick falls exactly one
second after it. The decoder's timestamps are moved to the new time base
at the same moment. A frame closed more than about one second after its
top is too late to be loaded (`SYNC late`). What remains is the
demodulation latency of the receiver, a few tens of milliseconds.

## Debug output

Built with `DCF77_DEBUG_PULSES=1`, the firmware prints the measurements
of every second:

```
 D <A> <code> <BW> <C> <bit> s<position>
```

| Field | Meaning |
|-------|---------|
| A | ticks active in window A (26 for a full pulse) |
| code | pulse length beyond 100 ms, in ticks: about 0–10 for a "0", 13–38 for a "1" |
| BW | ticks active in window BW (22 at most, full for a "1") |
| C | ticks active in window C (noise) |
| bit | `0`, `1`, `-` (no pulse) or `?` (unreadable) |
| position | position in the minute, `s-` while unknown |

`D - - - - .` means the grid is not locked. Events are appended:
` vote b/o` every 8 s (votes of the best triplet and of the best other
triplet: the grid locks when b ≥ 6 and b ≥ 2 × o), ` MIN` at the top of
the minute, ` LOCK`, ` RECENTER` and ` UNLOCK` for the grid.

How to read it:

- `vote 0/0`: no pulse of 47–312 ms reaches PD3: check the wiring and
  `DCF77_OUT_ACTIVE_LOW`.
- b and o of the same order: no regular pulses, the signal is too weak
  or too noisy.
- A around 26, code low for `0` and high for `1` with BW full, C low, and
  the position counting up to 59, where `MIN` appears: good reception.
- Many `?`, high C values, or `1`s with a low code: noise. Turn the
  antenna, move the clock away from power supplies, screens and
  computers.

## Validation

The [simulator](../firmware/test) compiles the decoder unchanged and
reproduces the firmware code around it: line edges, a tick every second
at a random phase, frame decoding, gate, and the `TCNT2` rewrite on each
load. It generates DCF77 signals whose time is known, checks every loaded
time to the second, and checks the displayed time in the middle of every
second after the first load. Two noise models are available: *synthetic*
(Poisson spurious pulses at a given rate, plus jitter, missed, clipped
and cut pulses) and *board*, fitted to recordings of the receiver on the
assembled clock (severity 1 reproduces them).

`make -C firmware/test bench`, 1,000 signals of 40 minutes each (first load:
mean over the signals that synchronized):

| Signal | Times loaded | Wrong | Never synchronized | First load |
|--------|-------------:|------:|-------------------:|------------------:|
| no noise | 34,456 | 0 | 0 % | 6 min |
| 1.8 spurious pulses/s | 31,477 | 0 | 0 % | 8 min |
| 3 spurious pulses/s | 29,164 | 0 | 0 % | 10 min |
| 5 spurious pulses/s | 23,347 | 0 | 0 % | 14 min |
| 8 spurious pulses/s | 6,557 | 0 | 22 % | 31 min |
| board model, severity 0.5 | 28,110 | 0 | 0 % | 11 min |
| board model, severity 0.75 | 22,866 | 0 | 0 % | 15 min |
| board model, severity 1 | 9,695 | 0 | 18 % | 27 min |
| board model, severity 1.3 | 56 | 0 | 99 % | 34 min |

No wrong second is displayed in any of these runs. The same holds, over
300 signals each, at midnight, with a crystal off by ±60 ppm, through a
5-minute signal outage, a phase jump of +70 or −90 ms, a 5-minute burst
of 15 spurious pulses per second, a second whose pulse is missing 40 % of
the time, a 10-minute loss of the carrier, an announced leap second, and
at 02:00 CET or 03:00 CEST on days without a change.

Announced DST changes never load a wrong time either, but the new time
can take a while to be displayed. In heavy noise (board model, severity
1), 16 to 18 % of the signals keep showing the old time for up to
11 minutes after the change, until confident frames confirm the new one;
below that noise level, at most 1 % of the signals are affected. When the
clock starts 10 minutes before a change, before it could read the
announcement, 2 to 30 % of the signals show the old time for up to
8 minutes.

Two corner cases do load a wrong time. A leap second 20 minutes after
start-up, with its announcement only partly read, left the clock one
second fast for up to 9 minutes in 8 signals out of 600, one of them
through a load one second off. A DST change without announcement, which
DCF77 never sends, keeps the old time for up to 16 minutes, and in 6 of
1,200 signals the old time is loaded once more after the change.

On the assembled clock, in heavy noise, the decoder ran for almost six
hours without displaying a wrong second: the time was first loaded after
10 minutes, then re-confirmed 224 times in 332 minutes, never more than
15 minutes apart. At that noise level about one start-up in five takes
more than 40 minutes to load the time; the orientation of the antenna
and its distance from the electronics matter more than anything in the
code.
