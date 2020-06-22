"""Runs the gateway binary against a log and returns what it produced."""
from __future__ import annotations

import json
import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from .pbwire import Batch, Signal, decode_delimited_stream

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("BW_BUILD_DIR", ROOT / "build"))
GATEWAY = BUILD / "gateway"
DECODER = BUILD / "bw_decode"


@dataclass
class RunResult:
    returncode: int
    stderr: str
    summary: dict = field(default_factory=dict)
    batches: list[Batch] = field(default_factory=list)

    @property
    def signals(self) -> list[Signal]:
        return [s for b in self.batches for s in b.signals]

    def by_name(self, name: str) -> list[Signal]:
        return [s for s in self.signals if s.name == name]

    @property
    def decoder(self) -> dict:
        return self.summary.get("decoder") or {}


def parse_summary(stderr: str) -> dict:
    for line in reversed(stderr.splitlines()):
        if line.startswith("summary "):
            return json.loads(line[len("summary "):])
    return {}


def gateway_env(**extra: str) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("BW_")}
    env["BW_DECODER"] = str(DECODER)
    env.update(extra)
    return env


def run_gateway(log: Path, workdir: Path, timeout: float = 30, **env: str) -> RunResult:
    """File-sink run: returns decoded batches from the output file."""
    out = workdir / "out.pb"
    p = subprocess.run([str(GATEWAY)], env=gateway_env(BW_LOG=str(log), BW_SINK="file",
                                                       BW_OUT=str(out), **env),
                       capture_output=True, text=True, timeout=timeout)
    batches = decode_delimited_stream(out.read_bytes()) if out.exists() else []
    return RunResult(p.returncode, p.stderr, parse_summary(p.stderr), batches)


def run_decoder(log: Path) -> tuple[list[dict], dict, int]:
    """Direct bw_decode run: (signal records, stats, returncode)."""
    p = subprocess.run([str(DECODER), "--stats", str(log)], capture_output=True, text=True, timeout=30)
    records = [json.loads(line) for line in p.stdout.splitlines()]
    stats = json.loads(p.stderr.strip().splitlines()[-1]) if p.stderr.strip() else {}
    return records, stats, p.returncode
