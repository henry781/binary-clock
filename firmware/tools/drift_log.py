#!/usr/bin/env python3
"""drift_log.py - measures the drift of the clock crystal.

Reads the "T <n> hh:mm:ss ..." lines that the clock firmware prints every
second when built with VERBOSE_TICK=1, and timestamps them with the PC clock
(which should be NTP-synchronized). After a few hours, the ratio between the
tick count and the PC time gives the drift in ppm.

Usage:  python3 drift_log.py <port> [baud] [file.csv]
        - USB-serial adapter on PD1 of the clock: 9600 baud (default)
        - through the serial bridge of dcf77_simulator.ino: 115200 baud
        Requires pyserial (pip install pyserial).
Stop with Ctrl-C to print the summary (ppm, seconds gained or lost per day).
"""
import sys, time, re
import serial

port = sys.argv[1]
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
out  = open(sys.argv[3] if len(sys.argv) > 3 else "drift.csv", "a")
ser  = serial.Serial(port, baud, timeout=2)
pat  = re.compile(r"^T (\d+) ")
first = None
last  = None
try:
    while True:
        line = ser.readline().decode(errors="replace").strip()
        m = pat.match(line)
        if not m:
            if line: print("   ", line)
            continue
        n = int(m.group(1)); t = time.time()
        if first is None:
            first = (n, t)
        last = (n, t)
        out.write(f"{t:.3f},{n}\n"); out.flush()
        dn = n - first[0]; dt = t - first[1]
        if dn > 0:
            err = dn - dt                    # > 0: the clock runs fast (or counts double ticks)
            ppm = err / dt * 1e6 if dt > 0 else 0
            print(f"ticks={dn:6d}  pc={dt:9.2f}s  offset={err:+7.3f}s  {ppm:+8.1f} ppm", end="\r")
except KeyboardInterrupt:
    pass
if first and last and last[1] > first[1]:
    dn = last[0] - first[0]; dt = last[1] - first[1]; err = dn - dt
    print(f"\n{dn} ticks in {dt:.1f} s: offset {err:+.3f} s = {err/dt*1e6:+.1f} ppm "
          f"({err/dt*86400:+.2f} s/day). A whole-second step (+1, +2 s) within minutes "
          f"means double ticks, not drift.")
