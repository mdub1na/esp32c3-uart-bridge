"""Host tests: bounded fake streams only; no serial port is enumerated or opened."""

import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock


TOOL = Path(__file__).resolve().parents[1] / "tools" / "serial_probe.py"
SPEC = importlib.util.spec_from_file_location("serial_probe", TOOL)
probe = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = probe
SPEC.loader.exec_module(probe)


class BoundedEcho:
    """A small duplex FIFO: transmit stalls unless the receiver runs concurrently."""

    def __init__(self, transform=None, capacity=67, initial=b"", drop_after=None):
        self.buffer = bytearray(initial)
        self.capacity = capacity
        self.condition = threading.Condition()
        self.transform = transform or (lambda chunk, offset: chunk)
        self.drop_after = drop_after
        self.sent = bytearray()
        self.request_sizes = []

    def write(self, data):
        self.request_sizes.append(len(data))
        with self.condition:
            until = time.monotonic() + 0.2
            while len(self.buffer) >= self.capacity:
                remaining = until - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("test FIFO full: receiver did not drain")
                self.condition.wait(remaining)
            # Deliberately accept partial writes, including within large chunks.
            count = min(len(data), self.capacity - len(self.buffer), 53)
            offset = len(self.sent)
            accepted = bytes(data[:count])
            self.sent.extend(accepted)
            echo = self.transform(accepted, offset)
            if self.drop_after is not None:
                echo = echo[:max(0, self.drop_after - offset)]
            self.buffer.extend(echo)
            self.condition.notify_all()
            return count

    def read(self, size):
        with self.condition:
            if not self.buffer:
                self.condition.wait(0.003)
            count = min(size, len(self.buffer), 29)
            data = bytes(self.buffer[:count])
            del self.buffer[:count]
            self.condition.notify_all()
            return data


class HostProbeTests(unittest.TestCase):
    def test_binary_roundtrip_drains_while_writing_and_handles_partial_writes(self):
        payload = probe.make_pattern(65536, 123)
        stream = BoundedEcho()
        result = probe.run_loopback(stream, payload, timeout=4, quiet=0.015, rate=0)
        self.assertEqual("pass", result.status, result)
        self.assertEqual(payload, stream.sent)
        self.assertEqual(set(range(256)), set(payload[:256]))
        self.assertGreater(max(stream.request_sizes), 4096)
        self.assertEqual(65536, result.sent_bytes)
        self.assertEqual(65536, result.received_bytes)
        self.assertEqual(hashlib.sha256(payload).hexdigest(), result.sent_sha256)
        self.assertEqual(result.sent_sha256, result.received_sha256)

    def test_default_pacing_limits_bursts_and_does_not_send_faster_than_uart_budget(self):
        stream = BoundedEcho(capacity=8192)
        payload = probe.make_pattern(1024, 123)
        result = probe.run_loopback(stream, payload, timeout=1, quiet=0.01)
        self.assertEqual("pass", result.status, result)
        self.assertLessEqual(max(stream.request_sizes), 64)
        self.assertEqual(9216.0, result.rate_limit_bytes_per_second)
        # First burst need not wait; subsequent bytes consume the UART budget.
        self.assertGreaterEqual(result.elapsed_seconds, (len(payload) - 64) / 9216)

    def test_seed_reproduces_pattern_and_different_seed_changes_random_tail(self):
        first = probe.make_pattern(1024, 42)
        self.assertEqual(first, probe.make_pattern(1024, 42))
        self.assertNotEqual(first[256:], probe.make_pattern(1024, 43)[256:])
        with self.assertRaises(ValueError):
            probe.make_pattern(255, 42)

    def test_empty_response_times_out_without_false_success(self):
        result = probe.run_loopback(BoundedEcho(drop_after=0), bytes(range(256)), 0.06, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertEqual(256, result.sent_bytes)
        self.assertEqual(0, result.received_bytes)
        self.assertIn("timeout", result.error)
        self.assertLess(result.elapsed_seconds, 0.5)

    def test_partial_response_cannot_pass(self):
        result = probe.run_loopback(BoundedEcho(drop_after=103), bytes(range(256)), 0.06, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertEqual(103, result.received_bytes)
        self.assertIn("timeout", result.error)

    def test_same_length_corruption_reports_offset(self):
        def corrupt(chunk, offset):
            data = bytearray(chunk)
            if offset <= 101 < offset + len(chunk):
                data[101 - offset] ^= 0x80
            return bytes(data)

        result = probe.run_loopback(BoundedEcho(transform=corrupt), bytes(range(256)), 0.5, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertEqual(256, result.received_bytes)
        self.assertIn("offset 101", result.error)
        self.assertNotEqual(result.sent_sha256, result.received_sha256)

    def test_extra_output_is_rejected_even_if_expected_prefix_is_exact(self):
        stream = BoundedEcho(transform=lambda chunk, offset: chunk + (
            b"!" if offset + len(chunk) == 256 else b""
        ))
        result = probe.run_loopback(stream, bytes(range(256)), 0.5, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertEqual(257, result.received_bytes)
        self.assertIn("extra", result.error)

    def test_unsolicited_input_is_not_discarded_or_accepted_as_a_clean_echo(self):
        result = probe.run_loopback(BoundedEcho(initial=b"old bytes"), bytes(range(256)), 0.5, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertIn("extra", result.error)

    def test_no_trailing_quiet_window_means_no_pass(self):
        stream = BoundedEcho(capacity=512)
        original_read = stream.read
        first_read = True

        def delayed_read(size):
            nonlocal first_read
            if first_read:
                first_read = False
                time.sleep(0.02)
            return original_read(size)

        stream.read = delayed_read
        result = probe.run_loopback(stream, bytes(range(256)), 0.05, 0.04, rate=0)
        self.assertEqual("fail", result.status)
        self.assertIn("silence", result.error)

    def test_write_failure_is_reported_and_writer_exits(self):
        stream = BoundedEcho()
        stream.write = mock.Mock(side_effect=OSError("disconnected"))
        result = probe.run_loopback(stream, bytes(range(256)), 0.5, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertIn("write failed: disconnected", result.error)
        self.assertEqual(0, result.sent_bytes)

    def test_write_stall_has_a_bounded_deadline(self):
        stream = BoundedEcho()
        stream.write = mock.Mock(return_value=0)
        result = probe.run_loopback(stream, bytes(range(256)), 0.05, 0.01, rate=0)
        self.assertEqual("fail", result.status)
        self.assertEqual(0, result.sent_bytes)
        self.assertIn("all bytes were sent", result.error)
        self.assertLess(result.elapsed_seconds, 0.5)

    def test_open_configures_line_states_and_raw_flow_control_before_open(self):
        class RecordingSerial:
            def __init__(self, **kwargs):
                self.kwargs = kwargs
                self.dtr = True
                self.rts = True
                self.port = None
                self.open_snapshot = None

            def open(self):
                self.open_snapshot = (self.dtr, self.rts, self.port)

            def close(self):
                pass

        fake_serial = types.SimpleNamespace(Serial=RecordingSerial)
        with mock.patch.object(probe, "_serial_module", return_value=fake_serial):
            stream = probe.open_serial("fake-only")
        self.assertEqual((False, False, "fake-only"), stream.open_snapshot)
        self.assertIsNone(stream.kwargs["port"])
        self.assertEqual((115200, 8, "N", 1), tuple(stream.kwargs[k] for k in (
            "baudrate", "bytesize", "parity", "stopbits"
        )))
        for setting in ("xonxoff", "rtscts", "dsrdtr"):
            self.assertIs(stream.kwargs[setting], False)
        self.assertGreater(stream.kwargs["timeout"], 0)
        self.assertGreater(stream.kwargs["write_timeout"], 0)

    def test_capture_is_verbatim_bounded_and_never_transmits(self):
        source = bytes(range(256)) * 30
        stream = types.SimpleNamespace(read=io.BytesIO(source).read,
                                       write=mock.Mock(side_effect=AssertionError("must not transmit")))
        output = io.BytesIO()
        result = probe.capture_raw(stream, output, duration=1, max_bytes=5001)
        self.assertEqual(source[:5001], output.getvalue())
        self.assertEqual(5001, result.received_bytes)
        self.assertEqual(hashlib.sha256(source[:5001]).hexdigest(), result.received_sha256)
        self.assertEqual("byte_limit", result.stopped_by)
        stream.write.assert_not_called()

    def test_capture_existing_output_fails_before_opening_a_port(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture.bin"
            output.write_bytes(b"preserve me")
            with mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
                status = probe.main(["capture", "fake-only", "--output", str(output)])
            self.assertEqual(2, status)
            opening.assert_not_called()
            self.assertEqual(b"preserve me", output.read_bytes())

    def test_invalid_loopback_arguments_do_not_open_a_port(self):
        with mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                probe.main(["loopback", "fake-only", "--bytes", "1"])
        self.assertEqual(2, error.exception.code)
        opening.assert_not_called()

    def test_optional_startup_capture_preserves_bytes_on_same_connection_before_strict_echo(self):
        startup = b"ESP-ROM: USB_UART_CHIP_RESET\r\n" + bytes(range(256)) + b"\xfe"
        stream = BoundedEcho(initial=startup, capacity=512)
        stream.close = mock.Mock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "startup.bin"
            stdout = io.StringIO()
            with mock.patch.object(probe, "open_serial", return_value=stream) as opening, mock.patch(
                "sys.stdout", new=stdout
            ):
                status = probe.main([
                    "loopback", "fake-only", "--bytes", "256", "--rate", "0",
                    "--timeout", "1", "--quiet", "0.01",
                    "--startup-log", str(output), "--startup-duration", "0.02",
                ])
            self.assertEqual(startup, output.read_bytes())
        report = json.loads(stdout.getvalue())
        self.assertEqual(0, status, report)
        self.assertEqual("pass", report["status"])
        self.assertEqual(256, report["received_bytes"])
        self.assertEqual(hashlib.sha256(bytes(range(256))).hexdigest(), report["received_sha256"])
        self.assertEqual(len(startup), report["startup_capture"]["received_bytes"])
        self.assertEqual(hashlib.sha256(startup).hexdigest(), report["startup_capture"]["received_sha256"])
        self.assertEqual("duration", report["startup_capture"]["stopped_by"])
        self.assertEqual(bytes(range(256)), stream.sent)
        opening.assert_called_once_with("fake-only")
        stream.close.assert_called_once_with()

    def test_existing_startup_log_fails_before_opening_port(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "startup.bin"
            output.write_bytes(b"preserve evidence")
            with mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
                status = probe.main(["loopback", "fake-only", "--startup-log", str(output)])
            self.assertEqual(2, status)
            opening.assert_not_called()
            self.assertEqual(b"preserve evidence", output.read_bytes())

    def test_startup_capture_limit_aborts_before_any_transmit(self):
        stream = BoundedEcho(initial=bytes(range(256)), capacity=512)
        stream.close = mock.Mock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "startup.bin"
            stdout = io.StringIO()
            with mock.patch.object(probe, "open_serial", return_value=stream), mock.patch.object(
                probe, "STARTUP_MAX_BYTES", 64
            ), mock.patch("sys.stdout", new=stdout):
                status = probe.main(["loopback", "fake-only", "--startup-log", str(output)])
            self.assertEqual(bytes(range(64)), output.read_bytes())
        report = json.loads(stdout.getvalue())
        self.assertEqual(1, status)
        self.assertEqual("fail", report["status"])
        self.assertIn("not started", report["error"])
        self.assertEqual(64, report["startup_capture"]["received_bytes"])
        self.assertEqual(b"", stream.sent)
        stream.close.assert_called_once_with()

    def test_startup_duration_without_explicit_log_is_rejected_before_open(self):
        with mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                probe.main(["loopback", "fake-only", "--startup-duration", "1"])
        self.assertEqual(2, error.exception.code)
        opening.assert_not_called()


class OneShotTests(unittest.TestCase):
    def capture(self, chunks, *, after_request=(), short_write=False, max_bytes=512):
        elapsed = 0.0
        incoming = list(chunks)
        first_write = []

        def read(size):
            nonlocal elapsed
            elapsed += 0.025
            if not incoming:
                return b""
            chunk = incoming.pop(0)
            if len(chunk) > size:
                incoming.insert(0, chunk[size:])
            return chunk[:size]

        def write(frame):
            first_write.append(elapsed)
            incoming.extend(after_request)
            return len(frame) - int(short_write)

        stream = types.SimpleNamespace(read=read, write=mock.Mock(side_effect=write))
        output = io.BytesIO()
        with mock.patch.object(probe.time, "monotonic", side_effect=lambda: elapsed):
            result = probe.run_one_shot(
                stream, output, prompt=b"board> ", line=b"help", duration=0.5,
                reply_duration=0.25, max_bytes=max_bytes,
            )
        return result, output.getvalue(), stream.write, first_write

    def test_one_frame_follows_exact_split_prompt_and_preserves_response(self):
        boot = (b"boot \x80\r\nboa", b"rd", b"> ")
        reply = (b"help\r\n\x00\xff", b"\r\nboard> ")
        result, raw, write, times = self.capture(boot, after_request=reply)
        self.assertEqual("request_written_to_bridge", result.status)
        self.assertEqual("reply_duration", result.stopped_by)
        self.assertEqual(b"".join(boot + reply), raw)
        self.assertEqual(hashlib.sha256(raw).hexdigest(), result.received_sha256)
        self.assertGreaterEqual(times[0], 0.075 + probe.PROMPT_QUIET_SECONDS)
        write.assert_called_once_with(b"\x00ESP-UART:help\n")

    def test_line_suffix_and_unsettled_prompt_do_not_trigger(self):
        chunks = (b"message board> ", b"\r\nboard> ", b"continued output")
        result, raw, write, _ = self.capture(chunks)
        self.assertEqual("prompt_timeout", result.stopped_by)
        self.assertEqual(b"".join(chunks), raw)
        write.assert_not_called()

    def test_byte_limit_prevents_send_even_when_capture_ends_at_prompt(self):
        boot = b"info\r\nboard> "
        result, raw, write, _ = self.capture((boot, b" trailing"), max_bytes=len(boot))
        self.assertEqual("byte_limit", result.stopped_by)
        self.assertEqual("no_request_written", result.status)
        self.assertEqual(boot, raw)
        write.assert_not_called()

    def test_short_write_is_not_retried(self):
        result, _, write, _ = self.capture((b"board> ",), short_write=True)
        self.assertEqual("short_bridge_write", result.status)
        write.assert_called_once_with(b"\x00ESP-UART:help\n")

    def test_native_port_fails_before_opening_a_serial_connection(self):
        devices = [
            types.SimpleNamespace(device="native", vid=0x303A, pid=0x1001),
            types.SimpleNamespace(device="luatos", vid=0x1A86, pid=0x55D3),
        ]
        serial_tools = types.SimpleNamespace(
            list_ports=types.SimpleNamespace(comports=mock.Mock(return_value=devices))
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "capture.bin"
            with mock.patch.dict(sys.modules, {"serial": types.SimpleNamespace(), "serial.tools": serial_tools}), \
                    mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
                status = probe.main([
                    "one-shot", "native", "--prompt", "board> ", "--line", "help", "--output", str(output),
                ])
            self.assertEqual(2, status)
            opening.assert_not_called()

    def test_nonprintable_or_overlong_line_is_rejected_before_opening(self):
        with mock.patch.object(probe, "open_serial") as opening, mock.patch("sys.stderr", new=io.StringIO()):
            for line in ("", "line\nother", "é", "a" * 129):
                with self.subTest(line=line), self.assertRaises(SystemExit) as error:
                    probe.main(["one-shot", "fake-only", "--prompt", "board> ", "--line", line,
                                "--output", "/never-created"])
                self.assertEqual(2, error.exception.code)
            opening.assert_not_called()


if __name__ == "__main__":
    unittest.main()
