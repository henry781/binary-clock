#!/bin/sh
# Long benchmark of the DCF77 decoder: the statistics quoted in
# docs/dcf77-decoder.md. Several minutes of CPU time; use JOBS to run the
# signals in parallel.
#
#   bench.sh [noise | dst | events | all]     (default: all)
set -eu
D=$(dirname "$0")
SWEEP=$D/sweep.sh
WHAT=${1:-all}

run() {   # run <label> <noise> <minutes> <first seed> <last seed> [VAR=value ...]
  label=$1; n=$2; m=$3; s0=$4; s1=$5; shift 5
  printf '%-48s ' "$label"
  env "$@" "$SWEEP" "$n" "$m" "$s0" "$s1"
}

if [ "$WHAT" = noise ] || [ "$WHAT" = all ]; then
  echo "== Noise levels (1000 signals of 40 min)"
  for n in 0 1.8 3 5 8; do run "synthetic, $n spurious pulses/s" "$n" 40 1000 1999 MODE=synth; done
  for n in 0.5 0.75 1 1.3; do run "board model, severity $n" "$n" 40 1000 1999 MODE=board; done
fi

if [ "$WHAT" = dst ] || [ "$WHAT" = all ]; then
  echo "== DST changes at minute 81 (300 signals of 100 min)"
  for c in "synth 1.8" "synth 5" "synth 8" "board 0.5" "board 0.75" "board 1"; do
    set -- $c
    run "winter to summer, $1 $2" "$2" 100 1000 1299 MODE=$1 START=0039 DSTCHG=81
    run "summer to winter, $1 $2" "$2" 100 1000 1299 MODE=$1 START=0139 DST=1 DSTCHG=81
  done
fi

if [ "$WHAT" = events ] || [ "$WHAT" = all ]; then
  echo "== Events (300 signals each)"
  run "leap second 00:59:60 CET, 5/s"              5 100 1000 1299 LEAP=80 START=2339
  run "leap second 01:59:60 CEST, 5/s"             5 100 1000 1299 LEAP=80 START=0039 DST=1
  run "leap second 20 min after start-up, CET"     5  30 1000 1299 LEAP=20 START=0039
  run "leap second 20 min after start-up, CEST"    5  30 1000 1299 LEAP=20 START=0139 DST=1
  run "midnight, 3/s"                              3 100 1000 1299 START=2339
  run "crystal +60 ppm, 3/s"                       3  40 1000 1299 PPM=60
  run "crystal -60 ppm, 3/s"                       3  40 1000 1299 PPM=-60
  run "5 min outage at 900 s, 3/s"                 3  40 1000 1299 OUTAGE=900:300
  run "phase jump +70 ms, 3/s"                     3  40 1000 1299 JUMP=900:70
  run "phase jump -90 ms, 3/s"                     3  40 1000 1299 JUMP=900:-90
  run "burst of 15/s for 5 min, 3/s"               3  40 1000 1299 BURST=600:300:15
  run "second 57 missing 40 %, board 0.75"      0.75  60 1000 1299 MODE=board WEAK=57:0.4
  run "second 57 missing 40 %, board 1"            1  60 1000 1299 MODE=board WEAK=57:0.4
  run "carrier lost 10 min, 3/s"                   3  40 1000 1299 LOSS=900:600
  run "carrier lost 10 min, board 0.75"         0.75  60 1000 1299 MODE=board LOSS=900:600
  run "carrier lost 01:04-01:59 CET, board 1"      1 100 1000 1299 MODE=board START=0039 LOSS=1500:3300
  for c in "synth 5" "synth 8" "board 0.75" "board 1"; do
    set -- $c
    run "02:00 CET without a change, $1 $2"  "$2" 100 1000 1299 MODE=$1 START=0139
    run "03:00 CEST without a change, $1 $2" "$2" 100 1000 1299 MODE=$1 START=0239 DST=1
  done
  for c in "synth 1.8" "synth 5" "board 0.5" "board 0.75"; do
    set -- $c
    run "start 01:50 CET, change at 02:00, $1 $2"  "$2" 60 1000 1299 MODE=$1 START=0150 DSTCHG=10
    run "start 02:50 CEST, change at 03:00, $1 $2" "$2" 60 1000 1299 MODE=$1 START=0250 DST=1 DSTCHG=10
    run "change without announcement, $1 $2"       "$2" 100 1000 1299 MODE=$1 START=0039 DSTCHG=81 NOA1=1
  done
fi
