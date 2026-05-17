#!/usr/bin/env python3
"""Loopback HTTP fixture for eta-http package tests."""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse


FIXTURE_BODY = b"eta-http-loopback-body\n"
REDIRECT_BODY = b"eta-http-redirect-target\n"
DOWNLOAD_1K_BODY = b"A" * 1024
DOWNLOAD_10MB_SIZE = 10 * 1024 * 1024
DOWNLOAD_PATTERN = bytes(range(256))
DOWNLOAD_10MB_SHA256 = hashlib.sha256(
    DOWNLOAD_PATTERN * (DOWNLOAD_10MB_SIZE // len(DOWNLOAD_PATTERN))
).hexdigest()


class LoopbackHandler(BaseHTTPRequestHandler):
    server_version = "EtaHttpFixture/0.3"
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/":
            self._send_response(200, FIXTURE_BODY, {"X-Fixture": "loopback"})
            return
        if path == "/missing":
            self._send_response(404, b"missing\n", {"X-Fixture": "loopback"})
            return
        if path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/redirect-target")
            self.send_header("Content-Length", "0")
            self.send_header("X-Fixture", "loopback")
            self.end_headers()
            return
        if path == "/redirect-target":
            self._send_response(200, REDIRECT_BODY, {"X-Fixture": "loopback"})
            return
        if path == "/headers-echo":
            header_value = self.headers.get("X-Eta-Test", "")
            self._send_response(
                200,
                header_value.encode("utf-8"),
                {"Content-Type": "text/plain", "X-Fixture": "loopback"},
            )
            return
        if path == "/cookie/set":
            self._send_response(
                200,
                b"cookie-set\n",
                {"Set-Cookie": "eta_cookie=crumb; Path=/", "X-Fixture": "loopback"},
            )
            return
        if path == "/cookie/echo":
            cookie_value = self.headers.get("Cookie", "")
            self._send_response(
                200,
                cookie_value.encode("utf-8"),
                {"Content-Type": "text/plain", "X-Fixture": "loopback"},
            )
            return
        if path == "/stall":
            time.sleep(0.35)
            self._send_response(200, b"delayed\n", {"X-Fixture": "loopback"})
            return
        if path == "/error-500":
            self._send_response(500, b"boom\n", {"X-Fixture": "loopback"})
            return
        if path == "/json-get":
            self._send_response(
                200,
                b"42",
                {"Content-Type": "application/json", "X-Fixture": "loopback"},
            )
            return
        if path == "/download/1k":
            self._send_response(
                200,
                DOWNLOAD_1K_BODY,
                {
                    "Content-Type": "application/octet-stream",
                    "X-Body-Sha256": hashlib.sha256(DOWNLOAD_1K_BODY).hexdigest(),
                    "X-Fixture": "loopback",
                },
            )
            return
        if path == "/download/10mb":
            self._send_large_download()
            return
        self._send_response(404, b"not-found\n", {"X-Fixture": "loopback"})

    def do_HEAD(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path in ("/", "/redirect-target", "/headers-echo"):
            self._send_response(200, b"", {"X-Fixture": "loopback"})
            return
        self._send_response(404, b"", {"X-Fixture": "loopback"})

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""

        if path == "/echo":
            self._send_response(200, body, {"X-Fixture": "loopback"})
            return

        if path == "/multipart":
            part_names = self._multipart_part_names(body)
            payload = ",".join(part_names).encode("utf-8")
            self._send_response(200, payload, {"Content-Type": "text/plain", "X-Fixture": "loopback"})
            return

        if path == "/json":
            self._send_response(200, body, {"Content-Type": "application/json", "X-Fixture": "loopback"})
            return

        self._send_response(404, b"not-found\n", {"X-Fixture": "loopback"})

    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
        _ = (fmt, args)

    def _multipart_part_names(self, body: bytes) -> list[str]:
        content_type = self.headers.get("Content-Type", "")
        if "multipart/form-data" not in content_type:
            return []

        boundary = ""
        for entry in content_type.split(";"):
            token = entry.strip()
            if token.startswith("boundary="):
                boundary = token.split("=", 1)[1].strip().strip('"')
                break
        if not boundary:
            return []

        delimiter = b"--" + boundary.encode("utf-8")
        out: list[str] = []
        for chunk in body.split(delimiter):
            part = chunk.strip()
            if not part or part == b"--" or part.startswith(b"--"):
                continue

            header_blob, _, _ = part.partition(b"\r\n\r\n")
            if not header_blob:
                continue
            for header_line in header_blob.decode("utf-8", "replace").split("\r\n"):
                lower = header_line.lower()
                if not lower.startswith("content-disposition:"):
                    continue
                for item in header_line.split(";"):
                    token = item.strip()
                    if token.startswith("name="):
                        value = token.split("=", 1)[1].strip().strip('"')
                        if value:
                            out.append(value)
                        break

        out.sort()
        return out

    def _send_response(self, status: int, body: bytes, extra_headers: dict[str, str]) -> None:
        self.send_response(status)
        if "Content-Type" not in extra_headers:
            self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        for key, value in extra_headers.items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD" and body:
            self.wfile.write(body)

    def _send_large_download(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(DOWNLOAD_10MB_SIZE))
        self.send_header("X-Body-Sha256", DOWNLOAD_10MB_SHA256)
        self.send_header("X-Fixture", "loopback")
        self.end_headers()

        if self.command == "HEAD":
            return

        remaining = DOWNLOAD_10MB_SIZE
        chunk = DOWNLOAD_PATTERN * 64  # 16 KiB
        while remaining > 0:
            size = min(len(chunk), remaining)
            self.wfile.write(chunk[:size])
            remaining -= size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--run", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), LoopbackHandler)
    host, port = server.server_address
    base_url = f"http://{host}:{port}"

    if not args.run:
        print(base_url, flush=True)
        try:
            server.serve_forever(poll_interval=0.2)
        finally:
            server.server_close()
        return 0

    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True)
    thread.start()

    env = os.environ.copy()
    env["ETA_HTTP_FIXTURE_BASE_URL"] = base_url
    try:
        completed = subprocess.run(args.run, env=env, check=False)
        return int(completed.returncode)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5.0)


if __name__ == "__main__":
    raise SystemExit(main())
