"""Builds candump-format logs: valid J1939 traffic, TP sessions, malformed input."""
from __future__ import annotations

from dataclasses import dataclass, field

PGN_EEC1, PGN_EEC2, PGN_CCVS, PGN_ET1, PGN_LFE, PGN_VIN = 61444, 61443, 65265, 65262, 65266, 65260
PGN_TP_CM, PGN_TP_DT = 0xEC00, 0xEB00


def make_id(priority: int, pgn: int, sa: int, da: int = 0xFF) -> int:
    pf = (pgn >> 8) & 0xFF
    ps = da if pf < 240 else pgn & 0xFF
    return (priority & 7) << 26 | ((pgn >> 16) & 3) << 24 | pf << 16 | ps << 8 | sa


@dataclass(frozen=True)
class Frame:
    ts_us: int
    can_id: int
    data: bytes

    def line(self, iface: str = "can0") -> str:
        return (f"({self.ts_us // 1_000_000}.{self.ts_us % 1_000_000:06d}) {iface} "
                f"{self.can_id:08X}#{self.data.hex().upper()}")


# ---- payload encoders (physical value -> raw bytes), independent of the C table ----
def _u16(v: int) -> bytes:
    return v.to_bytes(2, "little")


def eec1(rpm: float, torque_pct: float = 0.0) -> bytes:
    return bytes([0xFF, 0xFF, round(torque_pct + 125)]) + _u16(round(rpm / 0.125)) + b"\xff\xff\xff"


def eec2(pedal_pct: float) -> bytes:
    return b"\xff" + bytes([round(pedal_pct / 0.4)]) + b"\xff" * 6


def ccvs(kmh: float) -> bytes:
    return b"\xff" + _u16(round(kmh * 256)) + b"\xff" * 5


def et1(coolant_c: float) -> bytes:
    return bytes([round(coolant_c + 40)]) + b"\xff" * 7


def lfe(lph: float) -> bytes:
    return _u16(round(lph / 0.05)) + b"\xff" * 6


@dataclass
class LogBuilder:
    """Accumulates frames/raw lines in time order."""
    frames: list[str] = field(default_factory=list)
    now_us: int = 1_700_000_000_000_000

    def advance(self, ms: float) -> int:
        self.now_us += int(ms * 1000)
        return self.now_us

    def frame(self, pgn: int, sa: int, data: bytes, prio: int = 6, da: int = 0xFF,
              dt_ms: float = 10) -> None:
        self.frames.append(Frame(self.advance(dt_ms), make_id(prio, pgn, sa, da), data).line())

    def raw(self, line: str) -> None:
        self.frames.append(line)

    def bam(self, sa: int, pgn: int, payload: bytes, gap_ms: float = 50,
            skip_seq: set[int] | None = None, dup_seq: set[int] | None = None,
            announce_size: int | None = None, announce_packets: int | None = None) -> None:
        """BAM announce + TP.DT packets. Fault knobs let tests corrupt the session."""
        n = (len(payload) + 6) // 7
        size = len(payload) if announce_size is None else announce_size
        pk = n if announce_packets is None else announce_packets
        cm = bytes([32]) + _u16(size) + bytes([pk, 0xFF]) + pgn.to_bytes(3, "little")
        self.frame(PGN_TP_CM, sa, cm, prio=7, dt_ms=gap_ms)
        for seq in range(1, n + 1):
            if skip_seq and seq in skip_seq:
                continue
            chunk = payload[(seq - 1) * 7: seq * 7].ljust(7, b"\xff")
            dt = self.frame
            dt(PGN_TP_DT, sa, bytes([seq]) + chunk, prio=7, dt_ms=gap_ms)
            if dup_seq and seq in dup_seq:
                dt(PGN_TP_DT, sa, bytes([seq]) + chunk, prio=7, dt_ms=gap_ms)

    def text(self) -> str:
        return "\n".join(self.frames) + "\n"


# Lines the decoder must reject without crashing or emitting signals.
MALFORMED_LINES = [
    "garbage",
    "(1700000000.000000) can0",
    "(1700000000.000000) can0 18FEF100#",              # empty payload: valid, decodes nothing
    "(1700000000.000000) can0 18FEF100#FF0",           # odd hex digits
    "(1700000000.000000) can0 18FEF100#ZZZZ",
    "(1700000000.000000) can0 18FEF100#00112233445566778899AA",  # > 8 bytes
    "(1700000000.000000) can0 123#DEADBEEF",           # 11-bit id
    "(1700000000.000000) can0 18FEF100#R",              # remote frame
    "(1700000000.000000) can0 18FEF100##1001122334455",  # CAN FD
    "(1700000000.000000) can0 FFFFFFFF#00",            # id > 29 bits
    "(abc.def) can0 18FEF100#00",
    "\x00\x01\x02\xff\xfe binary junk",
    "(99999999999999999999.0) can0 18FEF100#00",
    "A" * 5000,                                         # longer than the decoder's line buffer
]
