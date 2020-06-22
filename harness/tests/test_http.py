"""HTTP sink: delivery, retry on 5xx, failure reporting."""
import subprocess

from buswhisperer_hil import canlog
from buswhisperer_hil.receiver import Receiver
from buswhisperer_hil.runner import GATEWAY, gateway_env, parse_summary


def run_http(log, url, **env):
    p = subprocess.run([str(GATEWAY)], capture_output=True, text=True, timeout=30,
                       env=gateway_env(BW_LOG=str(log), BW_SINK="http", BW_HTTP_URL=url, **env))
    return p, parse_summary(p.stderr)


def make_log(builder, write_log, n=25):
    for i in range(n):
        builder.frame(canlog.PGN_EEC1, 0, canlog.eec1(1000 + i))
    return write_log(builder)


def test_delivers_protobuf_batches(builder, write_log):
    log = make_log(builder, write_log)
    with Receiver() as rx:
        p, summary = run_http(log, rx.url, BW_BATCH_SIZE="10")
    assert p.returncode == 0, p.stderr
    assert [b.sequence for b in rx.batches] == [1, 2, 3, 4, 5]   # 50 signals / 10
    assert set(rx.content_types) == {"application/x-protobuf"}
    speeds = [s.value for b in rx.batches for s in b.signals if s.name == "engine_speed"]
    assert speeds == [1000.0 + i for i in range(25)]
    assert summary["batches"] == 5 and summary["failed_batches"] == 0


def test_retries_transient_failures(builder, write_log):
    log = make_log(builder, write_log, n=5)
    with Receiver(fail_first=2) as rx:
        p, summary = run_http(log, rx.url, BW_HTTP_RETRIES="3")
    assert p.returncode == 0, p.stderr
    assert rx.requests == 3 and len(rx.batches) == 1


def test_unreachable_endpoint_reports_failure(builder, write_log):
    log = make_log(builder, write_log, n=5)
    with Receiver() as rx:
        url = rx.url
    p, summary = run_http(log, url, BW_HTTP_RETRIES="1")       # server is now stopped
    assert p.returncode == 1
    assert summary["failed_batches"] == 1 and "failed to deliver" in p.stderr
