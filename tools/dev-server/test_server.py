from __future__ import annotations

import http.client
import io
import tempfile
import threading
import unittest
import wave
from pathlib import Path

from server import (
    DevelopmentServer,
    EchoHandler,
    MAX_AUDIO_BYTES,
    MAX_REQUEST_BYTES,
)


class FastEchoHandler(EchoHandler):
    stream_delay_seconds = 0


class EchoServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary_directory = tempfile.TemporaryDirectory()
        cls.capture_directory = Path(cls.temporary_directory.name)
        cls.server = DevelopmentServer(
            ("127.0.0.1", 0),
            FastEchoHandler,
            cls.capture_directory,
        )
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        cls.temporary_directory.cleanup()

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        response_body = response.read()
        connection.close()
        return response.status, response_body

    def test_health(self) -> None:
        status, body = self.request("GET", "/health")
        self.assertEqual(status, 200)
        self.assertEqual(body, b"3gent Stage 0 server is ready")

    def test_echoes_utf8_text(self) -> None:
        status, body = self.request("POST", "/echo", "hello 3DS ✓".encode())
        self.assertEqual(status, 200)
        self.assertEqual(
            body,
            "hello from 3gent dev server: hello 3DS ✓".encode(),
        )

    def test_rejects_unknown_path(self) -> None:
        status, _ = self.request("POST", "/missing", b"hello")
        self.assertEqual(status, 404)

    def test_rejects_oversized_body(self) -> None:
        status, _ = self.request("POST", "/echo", b"x" * (MAX_REQUEST_BYTES + 1))
        self.assertEqual(status, 413)

    def test_stream_returns_long_incremental_fixture(self) -> None:
        status, body = self.request("POST", "/stream", b"stream test")
        self.assertEqual(status, 200)
        self.assertIn(b"3gent incremental output test", body)
        self.assertIn(b"Chunk 24", body)
        self.assertIn(b"Stream complete", body)

    def test_saves_bounded_pcm_wav(self) -> None:
        wav_data = io.BytesIO()
        with wave.open(wav_data, "wb") as recording:
            recording.setnchannels(1)
            recording.setsampwidth(2)
            recording.setframerate(16364)
            recording.writeframes(b"\x00\x00\x10\x00")

        body = wav_data.getvalue()
        status, response = self.request(
            "POST",
            "/audio",
            body,
            {"Content-Type": "audio/wav"},
        )
        self.assertEqual(status, 200)
        self.assertIn(b"saved", response)
        self.assertEqual(
            (self.capture_directory / "latest.wav").read_bytes(),
            body,
        )

    def test_rejects_invalid_audio(self) -> None:
        status, _ = self.request(
            "POST",
            "/audio",
            b"not a wav",
            {"Content-Type": "audio/wav"},
        )
        self.assertEqual(status, 400)

    def test_rejects_truncated_audio(self) -> None:
        wav_data = io.BytesIO()
        with wave.open(wav_data, "wb") as recording:
            recording.setnchannels(1)
            recording.setsampwidth(2)
            recording.setframerate(16364)
            recording.writeframes(b"\x00\x00\x10\x00")

        status, _ = self.request(
            "POST",
            "/audio",
            wav_data.getvalue()[:-2],
            {"Content-Type": "audio/wav"},
        )
        self.assertEqual(status, 400)

    def test_rejects_oversized_audio(self) -> None:
        status, _ = self.request(
            "POST",
            "/audio",
            b"x" * (MAX_AUDIO_BYTES + 1),
            {"Content-Type": "audio/wav"},
        )
        self.assertEqual(status, 413)


if __name__ == "__main__":
    unittest.main()
