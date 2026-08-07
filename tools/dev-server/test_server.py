from __future__ import annotations

import http.client
import threading
import unittest

from server import DevelopmentServer, EchoHandler, MAX_REQUEST_BYTES


class EchoServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = DevelopmentServer(("127.0.0.1", 0), EchoHandler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def request(
        self,
        method: str,
        path: str,
        body: bytes | None = None,
    ) -> tuple[int, bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2)
        connection.request(method, path, body=body)
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


if __name__ == "__main__":
    unittest.main()
