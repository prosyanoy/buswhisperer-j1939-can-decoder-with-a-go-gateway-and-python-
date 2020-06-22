import subprocess

import pytest

from buswhisperer_hil import canlog, runner


@pytest.fixture(scope="session", autouse=True)
def built():
    """Builds the C decoder and Go gateway if they are missing."""
    if not (runner.GATEWAY.exists() and runner.DECODER.exists()):
        subprocess.run(["make", "-C", str(runner.ROOT), "all"], check=True)
    assert runner.GATEWAY.exists() and runner.DECODER.exists()


@pytest.fixture
def write_log(tmp_path):
    def _write(builder_or_text) -> "Path":
        text = builder_or_text.text() if hasattr(builder_or_text, "text") else builder_or_text
        p = tmp_path / "in.log"
        p.write_text(text)
        return p
    return _write


@pytest.fixture
def builder():
    return canlog.LogBuilder()
