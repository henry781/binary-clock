#!/bin/sh
# Regression test of the DCF77 decoder, run by CI (make test).
#
# Each case simulates SEEDS signals (default 40) under one condition. A case
# passes when every run completes, no wrong time is ever loaded, no wrong
# second is displayed after the first load, and at least the given share of
# the signals gets synchronized. SIM = simulator binary, JOBS = parallel jobs.
set -eu
D=$(dirname "$0")
SIM=${SIM:-$D/dcf77_sim}
SEEDS=${SEEDS:-40}
JOBS=${JOBS:-4}

# Internal: one simulator run, printing its last line and its exit status.
if [ "${1:-}" = "--run" ]; then
  shift
  st=0
  out=$("$SIM" "$@" 2>&1) || st=$?
  printf '%s exit=%d\n' "$(printf '%s\n' "$out" | tail -n 1)" "$st"
  exit 0
fi

[ -x "$SIM" ] || { echo "run_tests.sh: $SIM not found (run make)" >&2; exit 1; }
failed=0

# check <description> <min % synchronized> [VAR=value ...] -- <mode> <minutes> <noise>
check() {
  name=$1; need=$2; shift 2
  envs=
  while [ "$1" != "--" ]; do envs="$envs $1"; shift; done
  shift
  # $envs is split on purpose: one VAR=value word per variable.
  line=$(seq 1 "$SEEDS" | xargs -P "$JOBS" -I{} env $envs sh "$0" --run "$1" "$2" {} "$3" |
    awk -v need="$need" -v n="$SEEDS" '
      function f(key, def,   i, kv) {
        for (i = 2; i <= NF; i++) { split($i, kv, "="); if (kv[1] == key) return kv[2] + 0 }
        return def
      }
      { runs++; st = f("exit", -1)
        if ($1 != "result:" || (st != 0 && st != 2)) { crashed++; next }
        wrong += f("wrong", 0); disp += f("wrong_display_s", 0); t = f("first_load_s", -1)
        if (t >= 0) { sync++; tsum += t } }
      END {
        crashed += n - runs
        ok = crashed == 0 && wrong == 0 && disp == 0 && sync * 100 >= need * n
        printf "%s  synchronized %3d/%d  first load %5.0f s  wrong times %d  wrong seconds %d%s\n",
               ok ? "PASS" : "FAIL", sync, n, sync ? tsum / sync : -1, wrong, disp,
               crashed ? "  failed runs " crashed : "" }')
  printf '%-40s %s\n' "$name" "$line"
  case $line in FAIL*) failed=1 ;; esac
}

check "clean signal"                     100                                   -- synth  40 0
check "1.8 spurious pulses/s"            100                                   -- synth  40 1.8
check "5 spurious pulses/s"              100                                   -- synth  40 5
check "8 spurious pulses/s"               50                                   -- synth  40 8
check "board noise, severity 0.5"        100                                   -- board  40 0.5
check "board noise, severity 0.75"       100                                   -- board  40 0.75
check "board noise, severity 1"           50                                   -- board  40 1
check "winter to summer time"            100 START=0039 DSTCHG=81              -- synth 100 1.8
check "summer to winter time"            100 START=0139 DST=1 DSTCHG=81        -- synth 100 1.8
check "summer to winter, board 0.5"      100 START=0139 DST=1 DSTCHG=81        -- board 100 0.5
check "02:00 CET without a change"       100 START=0139                        -- synth 100 5
check "leap second, CET"                 100 LEAP=80 START=2339                -- synth 100 5
check "leap second, CEST"                100 LEAP=80 START=0039 DST=1          -- synth 100 5
check "midnight"                         100 START=2339                        -- synth 100 3
check "crystal +60 ppm"                  100 PPM=60                            -- synth  40 3
check "crystal -60 ppm"                  100 PPM=-60                           -- synth  40 3
check "5 min signal outage"              100 OUTAGE=900:300                    -- synth  40 3
check "phase jump +70 ms"                100 JUMP=900:70                       -- synth  40 3
check "phase jump -90 ms"                100 JUMP=900:-90                      -- synth  40 3
check "burst of 15 pulses/s for 5 min"   100 BURST=600:300:15                  -- synth  40 3
check "second 57 missing 40 % of time"   100 WEAK=57:0.4                       -- board  60 0.75
check "carrier lost for 10 min"          100 LOSS=900:600                      -- synth  40 3

if [ "$failed" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "all cases passed"
