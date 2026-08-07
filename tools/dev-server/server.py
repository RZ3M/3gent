#!/usr/bin/env python3
"""Disposable Stage 0 HTTP echo server for 3gent development."""

from __future__ import annotations

import argparse
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_REQUEST_BYTES = 4 * 1024
RESPONSE_PREFIX = "hello from 3gent dev server: "


class EchoHandler(BaseHTTPRequestHandler):
    server_version = "3gent-stage0/0.0.3"
    stream_delay_seconds = 0.08

    def _send_text(self, status: HTTPStatus, text: str) -> None:
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _read_message(self) -> str | None:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            self._send_text(HTTPStatus.LENGTH_REQUIRED, "Content-Length required")
            return None

        try:
            content_length = int(raw_length)
        except ValueError:
            self._send_text(HTTPStatus.BAD_REQUEST, "invalid Content-Length")
            return None

        if content_length < 0 or content_length > MAX_REQUEST_BYTES:
            self._send_text(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                f"request must be at most {MAX_REQUEST_BYTES} bytes",
            )
            return None

        body = self.rfile.read(content_length)
        if len(body) != content_length:
            self._send_text(HTTPStatus.BAD_REQUEST, "incomplete request body")
            return None

        try:
            return body.decode("utf-8")
        except UnicodeDecodeError:
            self._send_text(HTTPStatus.BAD_REQUEST, "request body must be UTF-8")
            return None

    def _send_stream(self, message: str) -> None:
        chunks = [
            "3gent incremental output test\n",
            f"Prompt received: {message[:80]}\n",
            "The server is sending this response in small pieces.\n",
        ]
        chunks.extend(
            f"Chunk {index:02d}: incremental agent output reached the 3DS.\n"
            for index in range(1, 25)
        )
        chunks.append("Stream complete. Use UP/DOWN to inspect scrollback.\n")
        encoded_chunks = [chunk.encode("utf-8") for chunk in chunks]

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header(
            "Content-Length",
            str(sum(len(chunk) for chunk in encoded_chunks)),
        )
        self.send_header("Connection", "close")
        self.end_headers()

        for chunk in encoded_chunks:
            self.wfile.write(chunk)
            self.wfile.flush()
            time.sleep(self.stream_delay_seconds)
        self.close_connection = True

    def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        if self.path != "/health":
            self._send_text(HTTPStatus.NOT_FOUND, "not found")
            return

        self._send_text(HTTPStatus.OK, "3gent Stage 0 server is ready")

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        if self.path not in {"/echo", "/stream"}:
            self._send_text(HTTPStatus.NOT_FOUND, "not found")
            return

        message = self._read_message()
        if message is None:
            return

        encoded_message = message.encode("utf-8")
        preview = message.replace("\r", "\\r").replace("\n", "\\n")[:80]
        print(
            f"{self.path.removeprefix('/')} from {self.client_address[0]}: "
            f"{len(encoded_message)} bytes, preview={preview!r}",
            flush=True,
        )
        if self.path == "/stream":
            self._send_stream(message)
        else:
            self._send_text(HTTPStatus.OK, RESPONSE_PREFIX + message)


class DevelopmentServer(ThreadingHTTPServer):
    allow_reuse_address = True

    def handle_error(self, request: object, client_address: tuple[str, int]) -> None:
        error = sys.exc_info()[1]
        if isinstance(error, (BrokenPipeError, ConnectionResetError)):
            print(
                f"client {client_address[0]} disconnected before completion",
                flush=True,
            )
            return
        super().handle_error(request, client_address)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="bind address (use 0.0.0.0 for a 3DS on the same LAN)",
    )
    parser.add_argument("--port", type=int, default=8080, help="TCP port")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = DevelopmentServer((args.host, args.port), EchoHandler)
    print(
        f"3gent Stage 0 server listening on http://{args.host}:{args.port}\n"
        "DEVELOPMENT ONLY: no authentication; use disposable test text.",
        flush=True,
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
