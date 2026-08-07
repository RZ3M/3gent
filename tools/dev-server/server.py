#!/usr/bin/env python3
"""Disposable Stage 0 HTTP echo server for 3gent development."""

from __future__ import annotations

import argparse
import sys
import threading
import time
import wave
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_REQUEST_BYTES = 4 * 1024
DEFAULT_MAX_AUDIO_SECONDS = 300
AUDIO_SAMPLE_RATE = 16364
AUDIO_BYTES_PER_SECOND = AUDIO_SAMPLE_RATE * 2
RESPONSE_PREFIX = "hello from 3gent dev server: "
DEFAULT_CAPTURE_DIRECTORY = Path(__file__).resolve().parent / "captures"


class ChunkedBodyError(Exception):
    """Raised when an HTTP chunked request body is malformed."""


class AudioStreamTooLarge(Exception):
    """Raised when a streamed capture crosses its configured limit."""


class EchoHandler(BaseHTTPRequestHandler):
    server_version = "3gent-stage0/0.0.6"
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

    def _read_audio_chunk(self, remaining_limit: int) -> bytes | None:
        size_line = self.rfile.readline(64)
        if not size_line or not size_line.endswith(b"\r\n"):
            raise ChunkedBodyError("incomplete chunk size")

        size_token = size_line[:-2].split(b";", 1)[0].strip()
        try:
            chunk_size = int(size_token, 16)
        except ValueError as error:
            raise ChunkedBodyError("invalid chunk size") from error

        if chunk_size < 0:
            raise ChunkedBodyError("invalid chunk size")
        if chunk_size == 0:
            trailer = self.rfile.readline(1024)
            while trailer not in {b"\r\n", b""}:
                if not trailer.endswith(b"\r\n"):
                    raise ChunkedBodyError("invalid chunk trailer")
                trailer = self.rfile.readline(1024)
            return None
        if chunk_size > remaining_limit:
            raise AudioStreamTooLarge

        chunk = self.rfile.read(chunk_size)
        if len(chunk) != chunk_size or self.rfile.read(2) != b"\r\n":
            raise ChunkedBodyError("incomplete chunk data")
        return chunk

    def _save_audio_stream(self) -> None:
        content_type = self.headers.get_content_type().lower()
        valid_format = (
            content_type == "application/x-3gent-pcm"
            and self.headers.get_param("format") == "s16le"
            and self.headers.get_param("rate") == "16364"
            and self.headers.get_param("channels") == "1"
        )
        if not valid_format:
            self._send_text(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                "expected application/x-3gent-pcm s16le/16364Hz/mono",
            )
            return

        transfer_encodings = {
            value.strip().lower()
            for value in self.headers.get("Transfer-Encoding", "").split(",")
        }
        if "chunked" not in transfer_encodings:
            self._send_text(
                HTTPStatus.BAD_REQUEST,
                "chunked Transfer-Encoding required",
            )
            return

        if not self.server.audio_stream_lock.acquire(blocking=False):
            self._send_text(HTTPStatus.CONFLICT, "another audio stream is active")
            return

        capture_directory = self.server.capture_directory
        capture_path = capture_directory / "latest.wav"
        temporary_path = capture_directory / "latest.wav.tmp"
        total_pcm_bytes = 0

        try:
            capture_directory.mkdir(parents=True, exist_ok=True)
            with temporary_path.open("w+b", buffering=0) as output_file:
                with wave.open(output_file, "wb") as recording:
                    recording.setnchannels(1)
                    recording.setsampwidth(2)
                    recording.setframerate(AUDIO_SAMPLE_RATE)

                    while True:
                        chunk = self._read_audio_chunk(
                            self.server.max_audio_pcm_bytes - total_pcm_bytes
                        )
                        if chunk is None:
                            break
                        recording.writeframesraw(chunk)
                        total_pcm_bytes += len(chunk)

            if total_pcm_bytes == 0 or total_pcm_bytes % 2 != 0:
                temporary_path.unlink(missing_ok=True)
                self._send_text(
                    HTTPStatus.BAD_REQUEST,
                    "audio stream must contain complete PCM16 samples",
                )
                return

            temporary_path.replace(capture_path)
        except AudioStreamTooLarge:
            temporary_path.unlink(missing_ok=True)
            self._send_text(
                HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                f"audio stream exceeds {self.server.max_audio_seconds} seconds",
            )
            return
        except ChunkedBodyError as error:
            temporary_path.unlink(missing_ok=True)
            self._send_text(HTTPStatus.BAD_REQUEST, str(error))
            return
        except OSError as error:
            temporary_path.unlink(missing_ok=True)
            self._send_text(HTTPStatus.INTERNAL_SERVER_ERROR, f"audio save failed: {error}")
            return
        finally:
            self.server.audio_stream_lock.release()

        duration_ms = total_pcm_bytes * 1000 // AUDIO_BYTES_PER_SECOND
        print(
            f"audio stream from {self.client_address[0]}: {total_pcm_bytes} PCM "
            f"bytes, {duration_ms} ms, saved to {capture_path}",
            flush=True,
        )
        self._send_text(
            HTTPStatus.OK,
            f"saved {total_pcm_bytes + 44}-byte WAV ({duration_ms} ms) as "
            f"{capture_path}",
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
        if self.path == "/audio/stream":
            self._save_audio_stream()
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
        max_audio_seconds: int = DEFAULT_MAX_AUDIO_SECONDS,
    ) -> None:
        if max_audio_seconds <= 0:
            raise ValueError("max_audio_seconds must be positive")
        self.capture_directory = capture_directory
        self.max_audio_seconds = max_audio_seconds
        self.max_audio_pcm_bytes = max_audio_seconds * AUDIO_BYTES_PER_SECOND
        self.audio_stream_lock = threading.Lock()
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
    parser.add_argument(
        "--max-audio-seconds",
        type=int,
        default=DEFAULT_MAX_AUDIO_SECONDS,
        help="maximum duration of one streamed microphone capture",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    server = DevelopmentServer(
        (args.host, args.port),
        EchoHandler,
        args.capture_dir,
        args.max_audio_seconds,
    )
    print(
        f"3gent Stage 0 server listening on http://{args.host}:{args.port}\n"
        f"Microphone captures will be saved to {args.capture_dir / 'latest.wav'}\n"
        f"Maximum microphone stream: {args.max_audio_seconds} seconds\n"
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
