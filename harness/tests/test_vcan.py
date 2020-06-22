"""Optional SocketCAN loop: sends frames over vcan0, records them as a candump log,
then replays the capture through the chain. Skipped unless vcan0 exists.

Setup (needs root):  sudo scripts/vcan_setup.sh
"""
import os
import socket
import struct
import time

import pytest

from buswhisperer_hil import canlog
from buswhisperer_hil.runner import run_gateway

pytestmark = pytest.mark.skipif(
    not (hasattr(socket, "AF_CAN") and os.path.exists("/sys/class/net/vcan0")),
    reason="vcan0 not available (see scripts/vcan_setup.sh)")

FMT = "=IB3x8s"


def test_vcan_capture_and_replay(tmp_path):
    rx = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    tx = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    rx.bind(("vcan0",))
    tx.bind(("vcan0",))
    rx.settimeout(1.0)
    sent = [canlog.make_id(3, canlog.PGN_EEC1, 0) | socket.CAN_EFF_FLAG] * 5
    for i, cid in enumerate(sent):
        tx.send(struct.pack(FMT, cid, 8, canlog.eec1(1000 + 100 * i)))
    lines = []
    for _ in sent:
        cid, dlc, data = struct.unpack(FMT, rx.recv(16))
        now = time.time_ns() // 1000
        lines.append(canlog.Frame(now, cid & socket.CAN_EFF_MASK, data[:dlc]).line("vcan0"))
    log = tmp_path / "vcan.log"
    log.write_text("\n".join(lines) + "\n")
    r = run_gateway(log, tmp_path)
    assert [s.value for s in r.by_name("engine_speed")] == [1000.0 + 100 * i for i in range(5)]
