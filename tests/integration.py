"""Black-box tests against real executables; Python peers independently implement the wire format."""

import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

SENDER, RECEIVER = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
SHORT_IO = [str(Path(value).resolve()) for value in sys.argv[3:5]]
sys.argv[1:] = []
MAGIC = b"FTAPP001"


def read_exact(peer, size):
    data = bytearray()
    while len(data) < size:
        block = peer.recv(size - len(data))
        if not block:
            raise AssertionError("Unexpected EOF from executable")
        data.extend(block)
    return bytes(data)


def status(code):
    return b"FTAK" + struct.pack("!I", code)


class Transfers(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.output = self.directory / "received.bin"
        self.source = self.directory / "source.bin"
        self.counter = 0
        self.sender_executable = SENDER
        self.receiver_executable = RECEIVER

    def cleanup_process(self, process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def start(self, command, env=None):
        self.counter += 1
        log = self.directory / ("process-%d.log" % self.counter)
        stream = log.open("wb")
        self.addCleanup(stream.close)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=stream, env=env)
        self.addCleanup(self.cleanup_process, process)
        return process, log

    def result(self, process, log, success=True):
        stdout, _ = process.communicate(timeout=10)
        stderr = log.read_text(encoding="utf-8", errors="replace")
        if success:
            self.assertEqual(process.returncode, 0, stderr)
        else:
            self.assertNotEqual(process.returncode, 0, stderr)
        return stdout, stderr

    def receiver(self, *extra, host="127.0.0.1", timeout=3):
        family = socket.AF_INET6 if ":" in host else socket.AF_INET
        with socket.socket(family) as probe:
            probe.bind((host, 0))
            port = probe.getsockname()[1]
        process, log = self.start([self.receiver_executable, str(port), str(self.output), "--bind", host,
                                   "--timeout", str(timeout), *extra])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if "Listening on " in log.read_text(encoding="utf-8", errors="replace"):
                return process, log, port
            if process.poll() is not None:
                self.fail("Receiver did not start: " + log.read_text(encoding="utf-8", errors="replace"))
            time.sleep(0.01)
        self.fail("Receiver readiness timed out")

    def sender(self, port, *extra, host="127.0.0.1"):
        return subprocess.run([self.sender_executable, host, str(port), str(self.source), "--timeout", "3", *extra],
                              capture_output=True, timeout=10)

    def peer(self, port):
        peer = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.addCleanup(peer.close)
        return peer

    def begin(self, peer, size):
        peer.sendall(MAGIC + struct.pack("!Q", size))
        self.assertEqual(read_exact(peer, 8), status(0))

    def finish(self, peer, payload):
        peer.sendall(payload + struct.pack("!I", zlib.crc32(payload)))
        peer.shutdown(socket.SHUT_WR)

    def no_partial(self):
        self.assertEqual(list(self.directory.glob(".file-transfer-*.part")), [])

    def test_round_trips_and_block_boundaries(self):
        for size in (0, 1, 1023, 1024, 1025, 65535, 65536, 65537, 4 * 1024 * 1024 + 31):
            with self.subTest(size=size):
                payload = os.urandom(size)
                self.source.write_bytes(payload)
                process, log, port = self.receiver()
                sent = self.sender(port)
                self.assertEqual(sent.returncode, 0, sent.stderr)
                self.assertIn(b"receiver verified", sent.stdout)
                self.result(process, log)
                self.assertEqual(self.output.read_bytes(), payload)
                self.output.unlink()
                self.no_partial()

    def test_unicode_and_spaces_in_paths(self):
        self.source = self.directory / "中文 source 🐈.bin"
        self.output = self.directory / "中文 output 🐈.bin"
        self.source.write_bytes(b"unicode paths")
        process, log, port = self.receiver()
        sent = self.sender(port, "--quiet")
        self.assertEqual(sent.returncode, 0, sent.stderr)
        self.assertEqual(sent.stdout + sent.stderr, b"")
        self.result(process, log)
        self.assertEqual(self.output.read_bytes(), self.source.read_bytes())

    @unittest.skipUnless(len(SHORT_IO) == 2, "Linux short-I/O test binaries not supplied")
    def test_short_and_interrupted_socket_operations(self):
        self.sender_executable, self.receiver_executable = SHORT_IO
        payload = os.urandom(512 * 1024 + 11)
        self.source.write_bytes(payload)
        process, log, port = self.receiver()
        sent = self.sender(port)
        self.assertEqual(sent.returncode, 0, sent.stderr)
        self.result(process, log)
        self.assertEqual(self.output.read_bytes(), payload)
        self.no_partial()

    def test_receiver_accepts_fragmented_python_peer(self):
        # Includes the standard CRC32 test vector (0xcbf43926), independent of C++ implementation.
        process, log, port = self.receiver()
        peer = self.peer(port)
        payload = b"123456789"
        for byte in MAGIC + struct.pack("!Q", len(payload)):
            peer.sendall(bytes([byte]))
            time.sleep(0.001)
        self.assertEqual(read_exact(peer, 8), status(0))
        for byte in payload + struct.pack("!I", 0xCBF43926):
            peer.sendall(bytes([byte]))
            time.sleep(0.001)
        peer.shutdown(socket.SHUT_WR)
        self.assertEqual(read_exact(peer, 8), status(1))
        self.result(process, log)
        self.assertEqual(self.output.read_bytes(), payload)

    def test_sender_protocol_with_python_receiver(self):
        for payload in (b"", b"123456789", os.urandom(256 * 1024 + 13)):
            with self.subTest(size=len(payload)), socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                listener.settimeout(5)
                self.source.write_bytes(payload)
                process, log = self.start([SENDER, "localhost", str(listener.getsockname()[1]),
                                           str(self.source), "--timeout", "3"])
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    self.assertEqual(read_exact(peer, 16), MAGIC + struct.pack("!Q", len(payload)))
                    for byte in status(0):
                        peer.sendall(bytes([byte]))
                    self.assertEqual(read_exact(peer, len(payload)), payload)
                    self.assertEqual(read_exact(peer, 4), struct.pack("!I", zlib.crc32(payload)))
                    self.assertEqual(peer.recv(1), b"")
                    self.assertIsNone(process.poll(), "Sender must wait for receiver confirmation")
                    peer.sendall(status(1))
                self.result(process, log)

    def test_existing_destination_is_preserved(self):
        self.output.write_bytes(b"original")
        completed = subprocess.run([RECEIVER, "9000", str(self.output)], capture_output=True, timeout=5)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(b"already exists", completed.stderr)
        self.assertEqual(self.output.read_bytes(), b"original")
        self.no_partial()

    def test_force_replaces_only_after_success(self):
        self.output.write_bytes(b"original")
        self.source.write_bytes(b"replacement")
        process, log, port = self.receiver("--force")
        self.assertEqual(self.output.read_bytes(), b"original")
        sent = self.sender(port)
        self.assertEqual(sent.returncode, 0, sent.stderr)
        self.result(process, log)
        self.assertEqual(self.output.read_bytes(), b"replacement")
        self.no_partial()

    def test_failed_force_preserves_original(self):
        self.output.write_bytes(b"original")
        process, log, port = self.receiver("--force")
        peer = self.peer(port)
        self.begin(peer, 9)
        peer.sendall(b"123456789" + struct.pack("!I", 0))
        peer.shutdown(socket.SHUT_WR)
        self.assertEqual(read_exact(peer, 8), status(2))
        _, stderr = self.result(process, log, success=False)
        self.assertIn("checksum mismatch", stderr)
        self.assertEqual(self.output.read_bytes(), b"original")
        self.no_partial()

    def test_destination_created_during_transfer_is_preserved(self):
        process, log, port = self.receiver()
        peer = self.peer(port)
        self.begin(peer, 3)
        self.output.write_bytes(b"someone else's file")
        self.finish(peer, b"new")
        self.assertEqual(read_exact(peer, 8), status(2))
        self.result(process, log, success=False)
        self.assertEqual(self.output.read_bytes(), b"someone else's file")
        self.no_partial()

    def test_truncated_payload_and_checksum(self):
        for payload in (b"ab", b"abc", b"abc\x00\x00"):
            with self.subTest(payload=payload):
                process, log, port = self.receiver()
                peer = self.peer(port)
                self.begin(peer, 3)
                peer.sendall(payload)
                peer.shutdown(socket.SHUT_WR)
                self.assertEqual(read_exact(peer, 8), status(2))
                self.result(process, log, success=False)
                self.assertFalse(self.output.exists())
                self.no_partial()

    def test_extra_payload_is_rejected(self):
        process, log, port = self.receiver()
        peer = self.peer(port)
        self.begin(peer, 0)
        peer.sendall(struct.pack("!I", 0) + b"extra")
        peer.shutdown(socket.SHUT_WR)
        self.assertEqual(read_exact(peer, 8), status(2))
        self.result(process, log, success=False)
        self.assertFalse(self.output.exists())
        self.no_partial()

    def test_invalid_protocol_and_size_limit(self):
        for header in (b"INVALID!" + struct.pack("!Q", 0), MAGIC + struct.pack("!Q", 101),
                       MAGIC + struct.pack("!Q", 2**64 - 1)):
            with self.subTest(header=header):
                process, log, port = self.receiver("--max-size", "100")
                peer = self.peer(port)
                peer.sendall(header)
                self.assertEqual(read_exact(peer, 8), status(2))
                self.result(process, log, success=False)
                self.assertFalse(self.output.exists())
                self.no_partial()

    def test_sender_reports_rejection(self):
        self.source.write_bytes(b"too large")
        process, log, port = self.receiver("--max-size", "0")
        sent = self.sender(port)
        self.assertNotEqual(sent.returncode, 0)
        self.assertIn(b"rejected", sent.stderr)
        self.result(process, log, success=False)
        self.assertFalse(self.output.exists())
        self.no_partial()

    def test_receive_timeout_cleans_up(self):
        process, log, port = self.receiver(timeout=1)
        peer = self.peer(port)
        self.begin(peer, 10)
        peer.sendall(b"partial")
        _, stderr = self.result(process, log, success=False)
        self.assertIn("timeout", stderr.lower())
        self.assertFalse(self.output.exists())
        self.no_partial()

    def test_accept_timeout_cleans_up(self):
        process, log, _ = self.receiver(timeout=1)
        _, stderr = self.result(process, log, success=False)
        self.assertIn("timeout", stderr.lower())
        self.no_partial()

    def test_sender_requires_final_acknowledgement(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            self.source.write_bytes(b"abc")
            process, log = self.start([SENDER, "127.0.0.1", str(listener.getsockname()[1]),
                                       str(self.source), "--timeout", "1"])
            with listener.accept()[0] as peer:
                peer.settimeout(5)
                read_exact(peer, 16)
                peer.sendall(status(0))
                read_exact(peer, 7)
                self.assertEqual(peer.recv(1), b"")
                _, stderr = self.result(process, log, success=False)
                self.assertIn("delivery was not confirmed", stderr)

    def test_source_size_changes_abort_transfer(self):
        for changed in (b"short", b"longer than the original"):
            with self.subTest(changed=changed), socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                listener.settimeout(5)
                self.source.write_bytes(b"0123456789")
                process, log = self.start([SENDER, "127.0.0.1", str(listener.getsockname()[1]),
                                           str(self.source), "--timeout", "2"])
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    self.assertEqual(read_exact(peer, 16), MAGIC + struct.pack("!Q", 10))
                    self.source.write_bytes(changed)
                    peer.sendall(status(0))
                    received = bytearray()
                    while True:
                        block = peer.recv(1024)
                        if not block:
                            break
                        received.extend(block)
                    self.assertLess(len(received), 14, "Changed input must not send a complete payload and checksum")
                self.result(process, log, success=False)

    def test_sender_rejects_invalid_responses(self):
        for response in (b"BAD!" + struct.pack("!I", 0), status(99), status(1), b"FTA"):
            with self.subTest(response=response), socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                listener.listen(1)
                listener.settimeout(5)
                self.source.write_bytes(b"abc")
                process, log = self.start([SENDER, "127.0.0.1", str(listener.getsockname()[1]),
                                           str(self.source), "--timeout", "2"])
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    read_exact(peer, 16)
                    peer.sendall(response)
                    peer.shutdown(socket.SHUT_WR)
                    self.result(process, log, success=False)

    def test_quiet_receiver(self):
        # A Python client retries only refused connections; a successful connection is the transfer.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process, log = self.start([RECEIVER, str(port), str(self.output), "--bind", "127.0.0.1",
                                   "--quiet", "--timeout", "3"])
        deadline = time.monotonic() + 5
        while True:
            try:
                peer = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except ConnectionRefusedError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    self.fail(log.read_text(encoding="utf-8", errors="replace"))
                time.sleep(0.01)
        with peer:
            peer.settimeout(3)
            self.begin(peer, 0)
            self.finish(peer, b"")
            self.assertEqual(read_exact(peer, 8), status(1))
        stdout, stderr = self.result(process, log)
        self.assertEqual(stdout, b"")
        self.assertEqual(stderr, "")
        self.no_partial()

    @unittest.skipIf(os.name == "nt", "Windows symlinks may require extra privileges")
    def test_symlink_destination_is_not_followed(self):
        self.source.write_bytes(b"original")
        self.output.symlink_to(self.source)
        for flags in ([], ["--force"]):
            completed = subprocess.run([RECEIVER, "9000", str(self.output), *flags],
                                       capture_output=True, timeout=5)
            self.assertNotEqual(completed.returncode, 0)
            self.assertTrue(self.output.is_symlink())
            self.assertEqual(self.source.read_bytes(), b"original")
            self.no_partial()

    def test_invalid_cli(self):
        cases = [(SENDER, []), (SENDER, ["host", "0", "file"]),
                 (SENDER, ["host", "65536", "file"]), (SENDER, ["host", "12x", "file"]),
                 (RECEIVER, ["1234", "out", "--timeout", "0"]),
                 (RECEIVER, ["1234", "out", "--timeout", "-1"]),
                 (RECEIVER, ["1234", "out", "--timeout"]),
                 (RECEIVER, ["1234", "out", "--max-size", str(2**64)]),
                 (RECEIVER, ["1234", "out", "--unknown"])]
        for executable, args in cases:
            with self.subTest(args=args):
                completed = subprocess.run([executable, *args], capture_output=True, timeout=5)
                self.assertEqual(completed.returncode, 2, completed.stderr)

    def test_missing_input_fails_before_connect(self):
        completed = self.sender(1)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn(b"regular file", completed.stderr)

    def test_missing_output_directory(self):
        completed = subprocess.run([RECEIVER, "9000", str(self.directory / "missing" / "out")],
                                   capture_output=True, timeout=5)
        self.assertNotEqual(completed.returncode, 0)
        self.no_partial()

    def test_help_version_and_addresses(self):
        for executable, args in ((SENDER, ["--help"]), (RECEIVER, ["--help"]),
                                 (SENDER, ["--version"]), (RECEIVER, ["--list-ips"]), (RECEIVER, [])):
            with self.subTest(args=args):
                completed = subprocess.run([executable, *args], capture_output=True, timeout=5)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertTrue(completed.stdout)

    def test_ipv6(self):
        try:
            with socket.socket(socket.AF_INET6) as probe:
                probe.bind(("::1", 0))
        except OSError:
            self.skipTest("IPv6 loopback unavailable")
        self.source.write_bytes(b"IPv6")
        process, log, port = self.receiver(host="::1")
        sent = self.sender(port, host="::1")
        self.assertEqual(sent.returncode, 0, sent.stderr)
        self.result(process, log)
        self.assertEqual(self.output.read_bytes(), b"IPv6")

    @unittest.skipIf(os.name == "nt", "POSIX signal test")
    def test_interrupt_cleans_up(self):
        process, log, port = self.receiver()
        peer = self.peer(port)
        self.begin(peer, 100)
        process.send_signal(signal.SIGINT)
        _, stderr = self.result(process, log, success=False)
        self.assertIn("cancelled", stderr)
        self.assertFalse(self.output.exists())
        self.no_partial()


if __name__ == "__main__":
    unittest.main(verbosity=2)
