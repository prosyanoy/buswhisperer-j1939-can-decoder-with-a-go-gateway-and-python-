"""Fault injection: hostile input must never crash the chain or corrupt good data."""
import random
import subprocess

import pytest

from buswhisperer_hil import canlog
from buswhisperer_hil.runner import GATEWAY, gateway_env, run_decoder, run_gateway


def good_frames(b, n=10):
    for i in range(n):
        b.frame(canlog.PGN_EEC1, 0, canlog.eec1(1000 + i))


def test_malformed_lines_are_skipped_and_counted(builder, write_log, tmp_path):
    good_frames(builder, 5)
    for line in canlog.MALFORMED_LINES:
        builder.raw(line)
        builder.frame(canlog.PGN_EEC1, 0, canlog.eec1(2000))
    r = run_gateway(write_log(builder), tmp_path)
    assert r.returncode == 0, r.stderr
    speeds = [s.value for s in r.by_name("engine_speed")]
    assert speeds == [1000.0 + i for i in range(5)] + [2000.0] * len(canlog.MALFORMED_LINES)
    d = r.decoder
    assert d["std_ids"] == 1 and d["rtr"] == 1 and d["fd"] == 1
    assert d["overlong"] == 1
    assert d["bad_syntax"] + d["range"] >= 8
    assert d["parsed"] == d["frames"] == 5 + len(canlog.MALFORMED_LINES) + 1  # 5 good + 14 follow-ups + the empty-payload frame


def test_no_trailing_newline_and_crlf(write_log, tmp_path):
    text = "(1.0) can0 0CF00400#FFFF7D401FFFFFFF\r\n(2.0) can0 0CF00400#FFFF7D401FFFFFFF"
    r = run_gateway(write_log(text), tmp_path)
    assert len(r.by_name("engine_speed")) == 2


def test_empty_and_comment_only_log(write_log, tmp_path):
    for text in ["", "\n\n", "# only a comment\n"]:
        r = run_gateway(write_log(text), tmp_path)
        assert r.returncode == 0 and r.batches == []


def test_binary_garbage_file(write_log, tmp_path):
    rng = random.Random(1)
    blob = bytes(rng.randrange(256) for _ in range(20000))
    p = tmp_path / "blob.log"
    p.write_bytes(blob)
    r = run_gateway(p, tmp_path)
    assert r.returncode == 0


@pytest.mark.parametrize("seed", range(8))
def test_random_corruption_of_valid_log(builder, tmp_path, seed):
    rng = random.Random(seed)
    good_frames(builder, 30)
    builder.bam(0, canlog.PGN_VIN, b"1FUJGLDR12LM12345*")
    data = bytearray(builder.text().encode())
    for _ in range(60):
        data[rng.randrange(len(data))] = rng.randrange(256)
    p = tmp_path / f"fuzz{seed}.log"
    p.write_bytes(bytes(data))
    records, stats, rc = run_decoder(p)
    assert rc == 0                       # decoder survived (ASan not needed for exit 0 here)
    r = run_gateway(p, tmp_path)
    assert r.returncode == 0 and len(r.signals) == len(records)


# ---- transport protocol faults ------------------------------------------------
VIN = b"1FUJGLDR12LM12345*"


def test_missing_packet_discards_message(builder, write_log, tmp_path):
    builder.bam(0, canlog.PGN_VIN, VIN, skip_seq={2})
    r = run_gateway(write_log(builder), tmp_path)
    assert r.by_name("vin") == []
    assert r.decoder["tp_errors"] == 1          # seq 3 arrived while 2 was expected


def test_timeout_between_packets(builder, write_log, tmp_path):
    builder.frame(canlog.PGN_TP_CM, 0, bytes([32, 18, 0, 3, 0xFF, 0xEC, 0xFE, 0]), prio=7)
    builder.frame(canlog.PGN_TP_DT, 0, bytes([1]) + VIN[:7], prio=7, dt_ms=50)
    builder.frame(canlog.PGN_TP_DT, 0, bytes([2]) + VIN[7:14], prio=7, dt_ms=800)  # > 750 ms
    builder.frame(canlog.PGN_TP_DT, 0, bytes([3]) + VIN[14:].ljust(7, b"\xff"), prio=7, dt_ms=50)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.by_name("vin") == []
    assert r.decoder["tp_timeouts"] == 1 and r.decoder["tp_orphans"] == 2


def test_gap_just_under_timeout_succeeds(builder, write_log, tmp_path):
    builder.bam(0, canlog.PGN_VIN, VIN, gap_ms=740)
    r = run_gateway(write_log(builder), tmp_path)
    assert [s.text for s in r.by_name("vin")] == [VIN[:-1].decode()]


def test_duplicate_packet_tolerated(builder, write_log, tmp_path):
    builder.bam(0, canlog.PGN_VIN, VIN, dup_seq={1})
    r = run_gateway(write_log(builder), tmp_path)
    assert len(r.by_name("vin")) == 1 and r.decoder["tp_duplicates"] == 1


@pytest.mark.parametrize("size,packets", [(2000, 255), (1786, 255), (8, 2), (0, 0), (65535, 255),
                                           (18, 4), (18, 2)])
def test_bad_announce_rejected(builder, write_log, tmp_path, size, packets):
    builder.bam(0, canlog.PGN_VIN, VIN, announce_size=size, announce_packets=packets)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.by_name("vin") == []
    assert r.decoder["tp_errors"] >= 1 and r.decoder["tp_started"] == 0


def test_max_size_message_accepted(builder, write_log, tmp_path):
    payload = bytes([0x41] * 1784) + b"*"        # 1785 bytes, 255 packets
    builder.bam(0, canlog.PGN_VIN, payload, gap_ms=1)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.by_name("vin")[0].text == "A" * 1784 and r.decoder["tp_completed"] == 1


def test_dt_storm_without_announce(builder, write_log, tmp_path):
    for seq in range(1, 300):
        builder.frame(canlog.PGN_TP_DT, 0, bytes([seq & 0xFF]) + b"\x41" * 7, prio=7, dt_ms=1)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.returncode == 0 and r.decoder["tp_orphans"] == 299


def test_many_concurrent_senders_evict_old_sessions(builder, write_log, tmp_path):
    for sa in range(1, 9):   # 8 announces, table holds 4
        builder.frame(canlog.PGN_TP_CM, sa, bytes([32, 18, 0, 3, 0xFF, 0xEC, 0xFE, 0]), prio=7, dt_ms=1)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.returncode == 0 and r.decoder["tp_started"] == 8 and r.decoder["tp_aborted"] == 4


# ---- gateway failure modes ----------------------------------------------------
def test_missing_log_fails_cleanly(tmp_path):
    r = run_gateway(tmp_path / "nope.log", tmp_path)
    assert r.returncode == 1 and "no such file" in r.stderr.lower()


def test_missing_decoder_fails_cleanly(builder, write_log, tmp_path):
    good_frames(builder, 1)
    p = subprocess.run([str(GATEWAY)], capture_output=True, text=True,
                       env=gateway_env(BW_LOG=str(write_log(builder)), BW_DECODER="/nonexistent/bw_decode",
                                       BW_OUT=str(tmp_path / "o.pb")))
    assert p.returncode == 1 and "decoder not found" in p.stderr


def test_bad_config_fails_cleanly(tmp_path):
    p = subprocess.run([str(GATEWAY)], capture_output=True, text=True,
                       env=gateway_env(BW_BATCH_SIZE="abc"))
    assert p.returncode == 1 and "BW_BATCH_SIZE" in p.stderr
