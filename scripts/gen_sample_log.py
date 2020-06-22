#!/usr/bin/env python3
"""Regenerates data/sample.log: a 20 s synthetic drive (idle -> pull away -> cruise)
with a VIN broadcast over BAM, plus a few deliberately bad lines."""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "harness"))
from buswhisperer_hil import canlog  # noqa: E402


def main() -> None:
    b = canlog.LogBuilder()
    b.now_us = 1_700_000_000_000_000
    b.raw("# BusWhisperer sample capture (synthetic): idle, accelerate, cruise")
    for step in range(200):                       # 100 ms per step
        t = step / 10
        speed = min(80.0, max(0.0, (t - 3) * 8))
        rpm = 700 + speed * 18 + 15 * math.sin(t * 3)
        temp = round(70 + min(20, t))
        pedal = 5 + min(40, speed / 2)
        b.frame(canlog.PGN_EEC1, 0x00, canlog.eec1(rpm, 10 + pedal / 2), prio=3, dt_ms=25)
        b.frame(canlog.PGN_EEC2, 0x00, canlog.eec2(pedal), prio=3, dt_ms=25)
        b.frame(canlog.PGN_CCVS, 0x00, canlog.ccvs(speed), dt_ms=25)
        b.frame(canlog.PGN_ET1, 0x00, canlog.et1(temp), dt_ms=25)
        if step % 20 == 0:
            b.frame(canlog.PGN_LFE, 0x00, canlog.lfe(2 + speed * 0.4), dt_ms=1)
        if step == 50:
            b.bam(0x00, canlog.PGN_VIN, b"1FUJGLDR12LM12345*")
        if step == 120:
            b.bam(0x00, canlog.PGN_VIN, b"1FUJGLDR12LM12345*", skip_seq={2})  # lossy transfer
        if step == 150:
            b.raw("(1700000015.000000) can0 18FEF100#")        # empty payload
            b.raw("this line is not a CAN frame")
            b.raw("(1700000015.000100) can0 123#DEAD")         # 11-bit id, ignored
    (ROOT / "data" / "sample.log").write_text(b.text())


if __name__ == "__main__":
    main()
