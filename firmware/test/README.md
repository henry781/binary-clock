# DCF77 decoder tests

`dcf77_sim.c` compiles the firmware's decoder, [`../binary_clock/dcf77_decoder.h`](../binary_clock/dcf77_decoder.h),
unchanged, and drives it the way the firmware does: line edges, one tick
per second, frame decoding, the gate, and the re-alignment of the time base
on each load. It generates DCF77 signals whose time is known, adds noise,
and checks every loaded time and every displayed second.

```sh
make            # build dcf77_sim (needs a C compiler)
make test       # regression suite, about 10 s; run by CI
make bench      # full statistics quoted in docs/dcf77-decoder.md, a few minutes
```

## Running the simulator

```sh
./dcf77_sim synth [minutes] [seed] [spurious pulses/s]   # synthetic noise
./dcf77_sim board [minutes] [seed] [severity]            # noise model of the assembled clock
./dcf77_sim csv <capture.csv> [active-high]              # replay an oscilloscope capture of PD3
```

In the synth and board modes, the last line summarizes the run:

```
result: loaded=31 wrong=0 wait=6 rejected=0 wrong_display_s=0 first_load_s=447
```

`loaded` times loaded, `wrong` wrong times loaded (the exit status is 2
if there is any), `wait` frames not confident yet, `rejected` frames that
failed a check (frames held back by the gate are counted in the `STAT=1`
line), `wrong_display_s` seconds displayed wrong after the first load,
`first_load_s` time of the first load (−1: never).

Environment variables add events to the signal (DST change, leap second,
crystal offset, outage, phase jump, noise burst, carrier loss…) or print
traces; they are listed at the top of [`dcf77_sim.c`](dcf77_sim.c). For
example, a change to summer time at minute 81 of a 100-minute signal
starting at 00:39 CET:

```sh
START=0039 DSTCHG=81 ./dcf77_sim synth 100 1 3
```

[`sweep.sh`](sweep.sh) runs many seeds of one condition and prints totals:

```sh
MODE=board ./sweep.sh 0.75 40 1 300    # board model, severity 0.75, 300 signals of 40 min
```

To evaluate a modified decoder without touching the firmware, build the
simulator against another copy of the header:

```sh
cc -O2 -DDECODER_H='"my_decoder.h"' -o dcf77_sim dcf77_sim.c -lm
```
