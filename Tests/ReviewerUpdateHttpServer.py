"""Independent controlled HTTP/TLS stalls for the manual update checker. UTF-8 logs.

Runs only when explicitly invoked with a compiled reviewer CLI. No Google writes,
browser, installer, or plugin editor. The clear-text TLS listener intentionally
never completes a handshake, to exercise cancellation during connection setup.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socketserver
import subprocess
import sys
import threading
import time
from urllib.parse import parse_qs, urlsplit


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), FixtureHandler)
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.counts: Counter[str] = Counter()
        self.observations: list[dict] = []

    @property
    def base(self) -> str:
        return "http://127.0.0.1:" + str(self.server_port)

    def record(self, path: str, headers: dict) -> int:
        with self.lock:
            self.counts[path] += 1
            self.observations.append({"path": path, "headers": headers, "time": time.monotonic()})
            return self.counts[path]


class FixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args) -> None:
        pass

    def reply(self, body: bytes, status: int = 200, headers: dict | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        self.close_connection = True

    def do_GET(self) -> None:
        server: FixtureServer = self.server  # type: ignore[assignment]
        path = urlsplit(self.path).path
        if path == "/__count":
            wanted = parse_qs(urlsplit(self.path).query).get("path", [""])[0]
            with server.lock:
                count = server.counts[wanted]
            self.reply(str(count).encode("ascii"))
            return
        ordinal = server.record(path, dict(self.headers))
        manifest = ("0.4," + server.base + "/landing?dynamic=from-cell-B1\r\n").encode("utf-8")
        if path.startswith("/redirect/"):
            remaining = int(path.rsplit("/", 1)[-1])
            if remaining:
                self.reply(b"", 302, {"Location": "/redirect/" + str(remaining - 1)})
            else:
                self.reply(manifest)
            return
        if path == "/absolute-redirect":
            self.reply(b"", 302, {"Location": server.base + "/valid"})
            return
        if path == "/loop":
            self.reply(b"", 302, {"Location": "/loop"})
            return
        if path == "/headers-stall":
            server.stop_event.wait(30)
            self.close_connection = True
            return
        if path in ("/body-stall", "/drip"):
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Length", "60000")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(b"0")
                self.wfile.flush()
                if path == "/body-stall":
                    server.stop_event.wait(30)
                else:
                    while not server.stop_event.wait(0.15):
                        self.wfile.write(b".")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            self.close_connection = True
            return
        if path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Length", str(len(manifest) + 1000))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(manifest)
                self.wfile.flush()
            except OSError:
                pass
            self.close_connection = True
            return
        if path == "/duplicate":
            if not server.stop_event.wait(0.5):
                self.reply(manifest)
            return
        if path in ("/body-at-cap", "/oversize-no-length"):
            body = manifest + b" " * (65536 - len(manifest))
            if path == "/body-at-cap":
                self.reply(body)
            else:
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body + b"overflow")
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True
            return
        if path == "/flip":
            self.reply(manifest if ordinal == 1 else b'not.a.version,https://example.test/invalid\r\n')
            return
        bodies = {
            "/valid": manifest,
            "/equal": ("0.3," + server.base + "/landing\n").encode("utf-8"),
            "/older": ("0.2.99," + server.base + "/landing\n").encode("utf-8"),
            "/numeric-newer": ("0.10," + server.base + "/landing\n").encode("utf-8"),
            "/quoted": ('\ufeff"0.4.0","' + server.base + '/landing?q=a,b"\r\n').encode("utf-8"),
            "/invalid-utf8": b"0.4,https://example.test/\xff\xfe\n",
            "/malformed": b'0.4,"https://example.test/"garbage\r\n',
            "/empty-url": b"0.4,\r\n",
            "/unsafe-url": b"0.4,file:///C:/Windows/not-an-update\r\n",
            "/oversize": b"0.4,https://example.test/" + b"x" * 65537,
            "/html": b"<!doctype html><html><body>Sign in</body></html>",
            "/landing": b"unexpected automatic B1 fetch",
        }
        if path == "/http-error":
            self.reply(manifest, 503)
        elif path in bodies:
            self.reply(bodies[path])
        else:
            self.reply(b"unknown fixture", 404)


class SilentTlsHandler(socketserver.BaseRequestHandler):
    def handle(self) -> None:
        with self.server.lock:  # type: ignore[attr-defined]
            self.server.accepted += 1  # type: ignore[attr-defined]
        self.request.settimeout(2)
        try:
            initial = self.request.recv(4096)
            with self.server.lock:  # type: ignore[attr-defined]
                self.server.client_bytes += len(initial)  # type: ignore[attr-defined]
        except OSError:
            pass
        self.server.stop_event.wait(30)  # type: ignore[attr-defined]


class SilentTlsServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self) -> None:
        super().__init__(("127.0.0.1", 0), SilentTlsHandler)
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.accepted = 0
        self.client_bytes = 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("client", type=Path)
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("build/reviewer-update-controlled-results.txt"))
    args = parser.parse_args()
    http, tls = FixtureServer(), SilentTlsServer()
    threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in (http, tls)]
    for thread in threads:
        thread.start()
    command = [str(args.client.resolve()), "--base", http.base,
               "--tls", "https://127.0.0.1:" + str(tls.server_address[1])]
    if args.dll:
        command += ["--dll", str(args.dll.resolve())]
    if args.live:
        command += ["--live"]
    try:
        try:
            result = subprocess.run(command, capture_output=True, timeout=150,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            transcript = result.stdout.decode("utf-8") + result.stderr.decode("utf-8")
            exit_code = result.returncode
        except subprocess.TimeoutExpired as error:
            transcript = (error.stdout or b"").decode("utf-8") + (error.stderr or b"").decode("utf-8")
            transcript += "FAIL independent CLI exceeded outer 150-second fail-safe; this is not a passing cancellation result\n"
            exit_code = 124
        args.output.write_text(transcript, encoding="utf-8")
        print(transcript, end="")
        with http.lock:
            data = {"counts": dict(http.counts), "requests": list(http.observations)}
        with tls.lock:
            data["silent_tls"] = {"accepted": tls.accepted, "client_bytes": tls.client_bytes}
        args.output.with_suffix(".http.json").write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return exit_code
    finally:
        for server in (http, tls):
            server.stop_event.set()
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join(timeout=2)


if __name__ == "__main__":
    for output in (sys.stdout, sys.stderr):
        if hasattr(output, "reconfigure"):
            output.reconfigure(encoding="utf-8", errors="strict")
    raise SystemExit(main())
