"""Exercise native HTTP shutdown with real sockets, on Windows and Linux.

    cmake -S native/tests -B <build>
    cmake --build <build> --config Release
    python native/tests/test_http.py <path-to-http_check>
"""
import json
import socket
import subprocess
import sys
import time
import unittest

EXE = sys.argv.pop(1)


class HttpShutdownTests(unittest.TestCase):
    def setUp(self):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.proc = subprocess.Popen(
            [EXE, str(self.port)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        self.sockets = []
        self.assertEqual(self.proc.stdout.readline().strip(), "READY")

    def tearDown(self):
        for sock in self.sockets:
            sock.close()
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.communicate(timeout=5)

    def connect(self, request=b""):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.sockets.append(sock)
        if request:
            sock.sendall(request)
        return sock

    def stop(self, expected):
        start = time.monotonic()
        out, err = self.proc.communicate("stop\n", timeout=10)
        self.assertEqual(self.proc.returncode, 0, err)
        self.assertEqual(out.strip(), f"STOPPED {expected}")
        return time.monotonic() - start

    def test_normal_request(self):
        sock = self.connect(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
        response = b""
        while chunk := sock.recv(8192):
            response += chunk
        self.assertIn(b"200 OK", response)
        self.assertEqual(json.loads(response.split(b"\r\n\r\n", 1)[1]), {"ok": True})
        self.stop(1)

    def test_idle_and_partial_connections(self):
        self.connect()
        self.connect(b"GET / HTTP/1.1\r\nHost:")
        self.connect(b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 50\r\n\r\n{")
        time.sleep(0.15)
        self.assertLess(self.stop(0), 2)

    def test_incomplete_body_is_not_handled(self):
        sock = self.connect(b"POST / HTTP/1.1\r\nContent-Length: 50\r\n\r\n{")
        sock.shutdown(socket.SHUT_WR)
        self.assertEqual(sock.recv(8192), b"")
        self.stop(0)

    def test_stop_waits_for_handler(self):
        self.connect(b"GET /slow HTTP/1.1\r\n\r\n")
        self.assertEqual(self.proc.stdout.readline().strip(), "HANDLING")
        self.assertGreater(self.stop(1), 2)

    def test_stop_interrupts_blocked_response(self):
        sock = self.connect()
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        sock.sendall(b"GET /large HTTP/1.1\r\n\r\n")
        time.sleep(0.2)
        self.assertLess(self.stop(1), 2)

    def test_restart_with_pending_connection(self):
        for _ in range(3):
            self.connect(b"GET /")
            self.proc.stdin.write("restart\n")
            self.proc.stdin.flush()
            self.assertEqual(self.proc.stdout.readline().strip(), "READY")
        self.test_normal_request()


if __name__ == "__main__":
    unittest.main()
