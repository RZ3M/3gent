#!/usr/bin/env python3
"""Disposable Stage 0 HTTP echo server for 3gent development."""

from __future__ import annotations

import argparse
import io
import sys
import time
import wave
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_REQUEST_BYTES = 4 * 1024
MAX_AUDIO_BYTES = 384 * 1024
RESPONSE_PREFIX = "hello from 3gent dev server: "
DEFAULT_CAPTURE_DIRECTORY = Path(__file__).resolve().parent / "captures"


class EchoHandler(BaseHTTPRequestHandler):
    server_version = "3gent-stage0/0.0.5"
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

    def _read_body(self, maximum_size: int) -> bytes | None:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            self._send_text(HTTPStatus.LENGTH_REQUIRED, "Content-Length required")
            return None

        try:
            content_length = int(raw_length)
        except ValueError:
            self._send_text(HTTPStatus.BAD_REQUEST, "invalid Content-Length")
            return None

        if content_length < 0 or content_length > maximum_size:
            self._send_text(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                f"request must be at most {maximum_size} bytes",
            )
            return None

        body = self.rfile.read(content_length)
        if len(body) != content_length:
            self._send_text(HTTPStatus.BAD_REQUEST, "incomplete request body")
            return None

        return body

    def _read_message(self) -> str | None:
        body = self._read_body(MAX_REQUEST_BYTES)
        if body is None:
            return None

        try:
            return body.decode("utf-8")
        except UnicodeDecodeError:
            self._send_text(HTTPStatus.BAD_REQUEST, "request body must be UTF-8")
            return None

    def _save_audio(self) -> None:
        if self.headers.get_content_type() != "audio/wav":
            self._send_text(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "audio/wav required")
            return

        body = self._read_body(MAX_AUDIO_BYTES)
        if body is None:
            return

        try:
            with wave.open(io.BytesIO(body), "rb") as recording:
                frame_count = recording.getnframes()
                expected_pcm_bytes = (
                    frame_count
                    * recording.getnchannels()
                    * recording.getsampwidth()
                )
                pcm_bytes = recording.readframes(frame_count)
                valid_format = (
                    recording.getnchannels() == 1
                    and recording.getsampwidth() == 2
                    and recording.getframerate() == 16364
                    and recording.getcomptype() == "NONE"
                    and frame_count > 0
                    and len(pcm_bytes) == expected_pcm_bytes
                )
        except (EOFError, wave.Error):
            valid_format = False

        if not valid_format:
            self._send_text(
                HTTPStatus.BAD_REQUEST,
                "expected non-empty mono PCM16 WAV at 16364 Hz",
            )
            return

        capture_directory = self.server.capture_directory
        capture_directory.mkdir(parents=True, exist_ok=True)
        capture_path = capture_directory / "latest.wav"
        temporary_path = capture_directory / "latest.wav.tmp"
        temporary_path.write_bytes(body)
        temporary_path.replace(capture_path)

        print(
            f"audio from {self.client_address[0]}: {len(body)} bytes, "
            f"saved to {capture_path}",
            flush=True,
        )
        self._send_text(
            HTTPStatus.OK,
            f"saved {len(body)}-byte WAV as {capture_path}",
        )

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
        if self.path == "/audio":
            self._save_audio()
            return

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

    def __init__(
        self,
        server_address: tuple[str, int],
        handler: type[BaseHTTPRequestHandler],
        capture_directory: Path = DEFAULT_CAPTURE_DIRECTORY,
    ) -> None:
        self.capture_directory = capture_directory
        super().__init__(server_address, handler)

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
    parser.add_argument(
        "--capture-dir",
        type=Path,
        default=DEFAULT_CAPTURE_DIRECTORY,
        help="directory for the latest Stage 0 microphone WAV",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = DevelopmentServer(
        (args.host, args.port),
        EchoHandler,
        args.capture_dir,
    )
    print(
        f"3gent Stage 0 server listening on http://{args.host}:{args.port}\n"
        f"Microphone captures will be saved to {args.capture_dir / 'latest.wav'}\n"
        "DEVELOPMENT ONLY: no authentication; use disposable test data.",
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
