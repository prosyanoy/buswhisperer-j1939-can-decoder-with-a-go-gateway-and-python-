"""End-to-end: candump log -> C decoder -> Go gateway -> protobuf file -> decoded in Python."""
import pytest

from buswhisperer_hil import canlog
from buswhisperer_hil.runner import ROOT, run_decoder, run_gateway


def drive_cycle(b, n=40):
    """Synthetic drive: returns the expected (name, value) series."""
    expected = []
    for i in range(n):
        rpm, kmh, temp, pedal, fuel, torque = 700 + 25 * i, 0.5 * i, 60 + i * 0.5 // 1, (i * 2) % 100, 1.5 + 0.05 * i, 10 + i
        temp = round(temp)
        b.frame(canlog.PGN_EEC1, 0x00, canlog.eec1(rpm, torque), prio=3)
        b.frame(canlog.PGN_CCVS, 0x00, canlog.ccvs(kmh))
        b.frame(canlog.PGN_ET1, 0x00, canlog.et1(temp))
        b.frame(canlog.PGN_EEC2, 0x00, canlog.eec2(pedal), prio=3)
        b.frame(canlog.PGN_LFE, 0x00, canlog.lfe(fuel))
        expected.append(dict(engine_speed=rpm, engine_torque=torque, vehicle_speed=kmh,
                             coolant_temp=temp, accelerator_pedal=pedal, fuel_rate=fuel))
    return expected


RES = dict(engine_speed=0.125, engine_torque=1, vehicle_speed=1 / 256, coolant_temp=1,
           accelerator_pedal=0.4, fuel_rate=0.05)


def test_all_signals_round_trip(builder, write_log, tmp_path):
    expected = drive_cycle(builder)
    r = run_gateway(write_log(builder), tmp_path)
    assert r.returncode == 0, r.stderr
    assert len(r.signals) == 6 * len(expected)
    for name, res in RES.items():
        got = r.by_name(name)
        assert len(got) == len(expected), name
        for sig, exp in zip(got, expected):
            assert sig.value == pytest.approx(exp[name], abs=res / 2 + 1e-9), name
            assert sig.source_address == 0
    assert {s.unit for s in r.by_name("engine_speed")} == {"rpm"}
    assert {s.unit for s in r.by_name("coolant_temp")} == {"degC"}


def test_timestamps_and_order_preserved(builder, write_log, tmp_path):
    drive_cycle(builder, n=5)
    r = run_gateway(write_log(builder), tmp_path)
    ts = [s.timestamp_us for s in r.signals]
    assert ts == sorted(ts) and len(set(ts)) == 5 * 5   # two EEC1 signals share a frame timestamp
    assert ts[0] == 1_700_000_000_010_000


@pytest.mark.parametrize("size", [1, 7, 50, 1000])
def test_batching(builder, write_log, tmp_path, size):
    drive_cycle(builder, n=20)  # 120 signals
    r = run_gateway(write_log(builder), tmp_path, BW_BATCH_SIZE=str(size))
    assert r.returncode == 0
    assert len(r.signals) == 120
    assert len(r.batches) == -(-120 // size)
    assert [b.sequence for b in r.batches] == list(range(1, len(r.batches) + 1))
    assert all(len(b.signals) <= size for b in r.batches)
    assert r.summary["batches"] == len(r.batches) and r.summary["signals"] == 120


def test_not_available_values_are_dropped(builder, write_log, tmp_path):
    builder.frame(canlog.PGN_EEC1, 0, b"\xff" * 8)
    builder.frame(canlog.PGN_ET1, 0, b"\xfb" + b"\xff" * 7)
    builder.frame(canlog.PGN_ET1, 0, canlog.et1(-40))   # raw 0: valid, -40 degC
    r = run_gateway(write_log(builder), tmp_path)
    assert [(s.name, s.value) for s in r.signals] == [("coolant_temp", -40.0)]


def test_multi_packet_vin(builder, write_log, tmp_path):
    vin = "1FUJGLDR12LM12345"
    builder.frame(canlog.PGN_EEC1, 0x00, canlog.eec1(900))
    builder.bam(0x00, canlog.PGN_VIN, vin.encode() + b"*")
    builder.frame(canlog.PGN_EEC1, 0x00, canlog.eec1(950))
    r = run_gateway(write_log(builder), tmp_path)
    assert [s.text for s in r.by_name("vin")] == [vin]
    assert r.by_name("vin")[0].pgn == canlog.PGN_VIN
    assert len(r.by_name("engine_speed")) == 2
    assert r.decoder["tp_started"] == 1 and r.decoder["tp_completed"] == 1


def test_concurrent_bams_from_two_ecus(builder, write_log, tmp_path):
    # Interleave two sessions packet-by-packet by building them separately.
    a, b = canlog.LogBuilder(), canlog.LogBuilder()
    a.bam(0x00, canlog.PGN_VIN, b"AAAAAAAAAAAAAAAA*")
    b.bam(0x17, canlog.PGN_VIN, b"BBBBBBBBBBBBBBBB*")
    lines = [x for pair in zip(a.frames, b.frames) for x in pair]
    builder.frames = lines
    r = run_gateway(write_log(builder), tmp_path)
    got = {s.source_address: s.text for s in r.by_name("vin")}
    assert got == {0x00: "A" * 16, 0x17: "B" * 16}


def test_classic_candump_format(write_log, tmp_path):
    text = "  can0  0CF00400   [8]  FF FF 7D 40 1F FF FF FF\n"  # 0x1F40/8 = 1000 rpm, torque 0
    r = run_gateway(write_log(text), tmp_path)
    assert [(s.name, s.value) for s in r.signals] == [("engine_speed", 1000.0), ("engine_torque", 0.0)]


def test_shipped_sample_log(tmp_path):
    r = run_gateway(ROOT / "data" / "sample.log", tmp_path)
    assert r.returncode == 0, r.stderr
    names = {s.name for s in r.signals}
    assert {"engine_speed", "vehicle_speed", "coolant_temp", "vin"} <= names
    # The gateway must agree exactly with calling the decoder directly.
    records, stats, rc = run_decoder(ROOT / "data" / "sample.log")
    assert rc == 0 and len(records) == len(r.signals)
    assert [x["name"] for x in records] == [s.name for s in r.signals]


def test_stdin_input(builder, tmp_path):
    import subprocess
    from buswhisperer_hil.runner import GATEWAY, gateway_env, parse_summary
    drive_cycle(builder, n=2)
    p = subprocess.run([str(GATEWAY)], input=builder.text(), capture_output=True, text=True,
                       env=gateway_env(BW_LOG="-", BW_OUT=str(tmp_path / "o.pb")))
    assert p.returncode == 0 and parse_summary(p.stderr)["signals"] == 12
