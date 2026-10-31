#!/bin/sh
# Runs dcf77_sim over a range of seeds and prints one summary line.
#
#   sweep.sh [noise] [minutes] [first seed] [last seed]
#
# MODE=synth (default; noise = spurious pulses per second) or MODE=board
# (noise = severity of the board model). SIM = simulator binary (default
# ./dcf77_sim next to this script), JOBS = parallel jobs (default 4). Other
# dcf77_sim variables (START, DST, DSTCHG, LEAP, PPM, ...) pass through.
#
# Output fields: loaded (times loaded), wrong (wrong times loaded), wait and
# rejected (frames), never (signals that never loaded a time), first (mean
# time to the first load, over the signals that loaded one), display (wrong
# displayed seconds: total, number of signals affected, worst signal). Exits
# with status 1 if a run did not complete.
set -eu
D=$(dirname "$0")
SIM=${SIM:-$D/dcf77_sim}

# Internal: one simulator run, printing the seed, its last line and its exit status.
if [ "${1:-}" = "--run" ]; then
  shift
  seed=$3
  st=0
  out=$("$SIM" "$@" 2>&1) || st=$?
  printf '%s %s exit=%d\n' "$seed" "$(printf '%s\n' "$out" | tail -n 1)" "$st"
  exit 0
fi

NOISE=${1:-3}; MIN=${2:-40}; S0=${3:-1}; S1=${4:-60}; MODE=${MODE:-synth}
[ -x "$SIM" ] || { echo "sweep.sh: $SIM not found (run make)" >&2; exit 1; }

seq "$S0" "$S1" | xargs -P "${JOBS:-4}" -I{} sh "$0" --run "$MODE" "$MIN" {} "$NOISE" | sort -n | awk -v n=$((S1 - S0 + 1)) '
function field(name, def,   i, kv) {
  for (i = 3; i <= NF; i++) { split($i, kv, "="); if (kv[1] == name) return kv[2] + 0 }
  return def
}
{
  seed = $1; runs++; st = field("exit", -1)
  if ($2 != "result:" || (st != 0 && st != 2)) { failed++; fseeds = fseeds " " seed; next }
  ok += field("loaded", 0); w = field("wrong", 0); wrong += w; if (w > 0) bad = bad " " seed
  wait += field("wait", 0); rej += field("rejected", 0)
  d = field("wrong_display_s", 0); disp += d
  if (d > 0) { nd++; if (d > dmax) { dmax = d; dseed = seed } }
  t = field("first_load_s", -1); if (t < 0) never++; else { ft += t; nf++ }
}
END {
  failed += n - runs
  printf "loaded %6d  wrong %3d  wait %6d  rejected %5d  never %3d/%d  first %5d s  display %d s (%d signals, worst %d s, seed %s)%s%s\n",
         ok, wrong, wait, rej, never, n, nf ? ft / nf : -1, disp, nd, dmax, (nd ? dseed : "-"),
         (wrong ? "  wrong at seeds:" bad : ""), (failed ? "  FAILED RUNS " failed ":" fseeds : "")
  exit failed ? 1 : 0
}'
