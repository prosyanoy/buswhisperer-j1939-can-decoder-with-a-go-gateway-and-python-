#!/usr/bin/env python3
"""Replays a candump log at real-time speed through bw_decode and prints live decoded values.

usage: scripts/live_demo.py [log] [speed]     (defaults: data/sample.log, 1.0; speed 0 = no pacing)
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
log = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "data" / "sample.log")
speed = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
decoder = ROOT / "build" / "bw_decode"
if not decoder.exists():
    sys.exit("build/bw_decode missing: run `make all` first")

proc = subprocess.Popen([str(decoder), "--stats", log], stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
latest, vin = {}, "-"
t0 = wall0 = last_print = None
for line in proc.stdout:
    s = json.loads(line)
    if t0 is None:
        t0, wall0, last_print = s["ts_us"], time.monotonic(), s["ts_us"]
    if speed > 0:
        delay = (s["ts_us"] - t0) / 1e6 / speed - (time.monotonic() - wall0)
        if delay > 0:
            time.sleep(delay)
    if "text" in s:
        vin = s["text"]
        print(f"[{(s['ts_us'] - t0) / 1e6:6.2f}s] VIN received via BAM multi-packet: {vin}", flush=True)
    else:
        latest[s["name"]] = s["value"]
    if s["ts_us"] - last_print >= 500_000 and latest:
        last_print = s["ts_us"]
        g = latest.get
        print(f"[{(s['ts_us'] - t0) / 1e6:6.2f}s] rpm {g('engine_speed', 0):7.1f} | "
              f"speed {g('vehicle_speed', 0):6.1f} km/h | coolant {g('coolant_temp', 0):4.0f} C | "
              f"pedal {g('accelerator_pedal', 0):5.1f} % | fuel {g('fuel_rate', 0):5.1f} L/h", flush=True)
proc.wait()
print("decoder stats:", proc.stderr.read().strip())
