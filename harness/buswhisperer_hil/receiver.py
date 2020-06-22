"""A tiny HTTP ingest endpoint that records protobuf batches (test double)."""
from __future__ import annotations

import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .pbwire import Batch, decode_batch


class Receiver:
    """Context manager: serves POST /ingest on an ephemeral port.

    fail_first: number of initial requests answered with 503 (retry testing).
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 0, fail_first: int = 0):
        self.batches: list[Batch] = []
        self.content_types: list[str] = []
        self.requests = 0
        self._fail_first = fail_first
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                outer.requests += 1
                if outer.requests <= outer._fail_first:
                    self.send_response(503)
                    self.end_headers()
                    return
                try:
                    batch = decode_batch(body)
                except ValueError:
                    self.send_response(400)
                    self.end_headers()
                    return
                outer.batches.append(batch)
                outer.content_types.append(self.headers.get("Content-Type", ""))
                self.send_response(204)
                self.end_headers()

            def log_message(self, *args):  # silence
                pass

        self._server = ThreadingHTTPServer((host, port), Handler)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)

    @property
    def url(self) -> str:
        host, port = self._server.server_address[:2]
        return f"http://{host}:{port}/ingest"

    def __enter__(self) -> "Receiver":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._server.shutdown()
        self._server.server_close()


if __name__ == "__main__":  # docker-compose demo receiver
    import os
    import sys

    rx = Receiver(host="0.0.0.0", port=int(os.environ.get("PORT", "8080")))
    orig = rx._server.RequestHandlerClass.do_POST

    def logging_post(handler):
        orig(handler)
        if rx.batches:
            b = rx.batches[-1]
            names = sorted({s.name for s in b.signals})
            print(f"batch {b.sequence}: {len(b.signals)} signals {names}", flush=True)

    rx._server.RequestHandlerClass.do_POST = logging_post
    print(f"receiver listening on {rx.url}", file=sys.stderr, flush=True)
    rx._server.serve_forever()
