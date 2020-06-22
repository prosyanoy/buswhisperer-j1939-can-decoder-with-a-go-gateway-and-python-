"""Independent protobuf decoder for proto/buswhisperer.proto (no protobuf package).

Deliberately separate from the Go encoder so the end-to-end test is a true
cross-implementation check of the wire format.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Signal:
    timestamp_us: int = 0
    source_address: int = 0
    pgn: int = 0
    name: str = ""
    unit: str = ""
    value: float = 0.0
    text: str = ""


@dataclass
class Batch:
    sequence: int = 0
    signals: list[Signal] = field(default_factory=list)


def _varint(buf: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        if pos >= len(buf) or shift > 63:
            raise ValueError("truncated or oversized varint")
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def _fields(buf: bytes):
    pos = 0
    while pos < len(buf):
        tag, pos = _varint(buf, pos)
        num, wire = tag >> 3, tag & 7
        if wire == 0:
            val, pos = _varint(buf, pos)
        elif wire == 1:
            val, pos = buf[pos:pos + 8], pos + 8
        elif wire == 2:
            n, pos = _varint(buf, pos)
            val, pos = buf[pos:pos + n], pos + n
            if len(val) != n:
                raise ValueError("truncated length-delimited field")
        elif wire == 5:
            val, pos = buf[pos:pos + 4], pos + 4
        else:
            raise ValueError(f"unsupported wire type {wire}")
        yield num, wire, val


def decode_signal(buf: bytes) -> Signal:
    s = Signal()
    for num, wire, val in _fields(buf):
        if num == 1 and wire == 0:
            s.timestamp_us = val
        elif num == 2 and wire == 0:
            s.source_address = val
        elif num == 3 and wire == 0:
            s.pgn = val
        elif num == 4 and wire == 2:
            s.name = val.decode()
        elif num == 5 and wire == 2:
            s.unit = val.decode()
        elif num == 6 and wire == 1:
            (s.value,) = struct.unpack("<d", val)
        elif num == 7 and wire == 2:
            s.text = val.decode()
    return s


def decode_batch(buf: bytes) -> Batch:
    b = Batch()
    for num, wire, val in _fields(buf):
        if num == 1 and wire == 0:
            b.sequence = val
        elif num == 2 and wire == 2:
            b.signals.append(decode_signal(val))
    return b


def decode_delimited_stream(data: bytes) -> list[Batch]:
    batches, pos = [], 0
    while pos < len(data):
        n, pos = _varint(data, pos)
        if pos + n > len(data):
            raise ValueError("truncated delimited message")
        batches.append(decode_batch(data[pos:pos + n]))
        pos += n
    return batches
