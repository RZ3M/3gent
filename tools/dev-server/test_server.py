from __future__ import annotations

import http.client
import tempfile
import threading
import time
import unittest
import wave
from pathlib import Path

from server import (
    AUDIO_BYTES_PER_SECOND,
    DevelopmentServer,
    EchoHandler,
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
            1,
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

    def stream_audio(
        self,
        chunks: list[bytes],
    ) -> tuple[int, bytes]:
        connection = self.open_audio_stream()

        for chunk in chunks:
            connection.send(f"{len(chunk):x}\r\n".encode())
            connection.send(chunk)
            connection.send(b"\r\n")
        connection.send(b"0\r\n\r\n")

        response = connection.getresponse()
        response_body = response.read()
        connection.close()
        return response.status, response_body

    def open_audio_stream(self) -> http.client.HTTPConnection:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        connection.putrequest("POST", "/audio/stream")
        connection.putheader(
            "Content-Type",
            "application/x-3gent-pcm; format=s16le; rate=16364; channels=1",
        )
        connection.putheader("Transfer-Encoding", "chunked")
        connection.endheaders()
        return connection

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

    def test_reuses_connection_for_multiple_echoes(self) -> None:
        connection = http.client.HTTPConnection(
            "127.0.0.1",
            self.port,
            timeout=2,
        )
        try:
            connection.request("POST", "/echo", body=b"first")
            first_response = connection.getresponse()
            self.assertEqual(first_response.status, 200)
            first_response.read()
            self.assertIsNotNone(connection.sock)
            first_client_port = connection.sock.getsockname()[1]

            connection.request("POST", "/echo", body=b"second")
            second_response = connection.getresponse()
            self.assertEqual(second_response.status, 200)
            second_response.read()
            self.assertIsNotNone(connection.sock)
            self.assertEqual(
                connection.sock.getsockname()[1],
                first_client_port,
            )
        finally:
            connection.close()

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

    def test_streams_pcm_chunks_into_wav(self) -> None:
        pcm_data = b"\x00\x00\x10\x00\x20\x00\x30\x00"
        status, response = self.stream_audio([pcm_data[:4], pcm_data[4:]])
        self.assertEqual(status, 200)
        self.assertIn(b"saved", response)
        with wave.open(str(self.capture_directory / "latest.wav"), "rb") as saved:
            self.assertEqual(saved.getnchannels(), 1)
            self.assertEqual(saved.getsampwidth(), 2)
            self.assertEqual(saved.getframerate(), 16364)
            self.assertEqual(saved.readframes(saved.getnframes()), pcm_data)

    def test_reuses_connection_for_multiple_audio_streams(self) -> None:
        connection = self.open_audio_stream()
        try:
            first_pcm = b"\x01\x00\x02\x00"
            connection.send(f"{len(first_pcm):x}\r\n".encode())
            connection.send(first_pcm + b"\r\n0\r\n\r\n")
            first_response = connection.getresponse()
            self.assertEqual(first_response.status, 200)
            first_response.read()
            self.assertIsNotNone(connection.sock)
            first_client_port = connection.sock.getsockname()[1]

            connection.putrequest("POST", "/audio/stream")
            connection.putheader(
                "Content-Type",
                "application/x-3gent-pcm; "
                "format=s16le; rate=16364; channels=1",
            )
            connection.putheader("Transfer-Encoding", "chunked")
            connection.endheaders()
            second_pcm = b"\x03\x00\x04\x00"
            connection.send(f"{len(second_pcm):x}\r\n".encode())
            connection.send(second_pcm + b"\r\n0\r\n\r\n")
            second_response = connection.getresponse()
            self.assertEqual(second_response.status, 200)
            second_response.read()
            self.assertIsNotNone(connection.sock)
            self.assertEqual(
                connection.sock.getsockname()[1],
                first_client_port,
            )
        finally:
            connection.close()

    def test_writes_temporary_wav_before_stream_finishes(self) -> None:
        connection = self.open_audio_stream()
        try:
            pcm_data = b"\x00\x00" * 8192
            connection.send(f"{len(pcm_data):x}\r\n".encode())
            connection.send(pcm_data)
            connection.send(b"\r\n")

            temporary_path = self.capture_directory / "latest.wav.tmp"
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                if temporary_path.exists() and temporary_path.stat().st_size > 44:
                    break
                time.sleep(0.01)
            self.assertTrue(temporary_path.exists())
            self.assertGreater(temporary_path.stat().st_size, 44)

            connection.send(b"0\r\n\r\n")
            response = connection.getresponse()
            response.read()
            self.assertEqual(response.status, 200)
        finally:
            connection.close()

    def test_rejects_empty_audio_stream(self) -> None:
        status, _ = self.stream_audio([])
        self.assertEqual(status, 400)

    def test_rejects_incomplete_pcm16_sample(self) -> None:
        status, _ = self.stream_audio([b"\x00"])
        self.assertEqual(status, 400)

    def test_rejects_audio_stream_over_duration_limit(self) -> None:
        status, _ = self.stream_audio([b"\x00" * (AUDIO_BYTES_PER_SECOND + 2)])
        self.assertEqual(status, 413)

    def test_rejects_non_streaming_audio_format(self) -> None:
        status, _ = self.request(
            "POST",
            "/audio/stream",
            b"not pcm",
            {"Content-Type": "audio/wav"},
        )
        self.assertEqual(status, 415)

    def test_disconnect_discards_partial_stream(self) -> None:
        capture_path = self.capture_directory / "latest.wav"
        temporary_path = self.capture_directory / "latest.wav.tmp"
        previous_capture = b"previous completed capture"
        capture_path.write_bytes(previous_capture)

        connection = self.open_audio_stream()
        pcm_data = b"\x00\x00" * 8192
        connection.send(f"{len(pcm_data):x}\r\n".encode())
        connection.send(pcm_data)
        connection.send(b"\r\n")

        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if temporary_path.exists() and temporary_path.stat().st_size > 44:
                break
            time.sleep(0.01)
        self.assertTrue(temporary_path.exists())
        connection.close()

        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and temporary_path.exists():
            time.sleep(0.01)
        self.assertFalse(temporary_path.exists())
        self.assertEqual(capture_path.read_bytes(), previous_capture)


if __name__ == "__main__":
    unittest.main()
