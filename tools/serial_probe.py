#!/usr/bin/env python3
"""Raw binary checks for the ESP32-C3 USB–UART bridge.

The explicit ``loopback`` and ``one-shot`` commands write to the serial port.
The ``capture`` command saves incoming bytes verbatim. Opening a native
Espressif serial port can still reset it through driver/OS line-state handling.
Preconfiguring DTR and RTS low reduces that risk.
"""

import argparse
import hashlib
import json
import math
import random
import sys
import threading
import time
from contextlib import ExitStack
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import BinaryIO, Optional, Sequence


BAUDRATE = 115200
READ_TIMEOUT = 0.025
WRITE_TIMEOUT = 0.2
READ_SIZE = 4096
WRITE_SIZES = (1, 31, 255, 4097, 16384, 7, 1024)
DEFAULT_RATE = 9216.0  # 80% of the UART's 115200 / 10 bytes per second.
PACED_WRITE_SIZE = 64
STARTUP_MAX_BYTES = 1048576
LUATOS_USB_ID = (0x1A86, 0x55D3)
LUATOS_FRAME_PREFIX = b"\x00ESP-UART:"
LUATOS_LINE_LIMIT = 128
PROMPT_QUIET_SECONDS = 0.1


def _serial_module():
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required: python3 -m pip install -r requirements-host.txt"
        ) from exc
    return serial


def open_serial(port: str):
    """Configure a closed port before opening; never pulse DTR/RTS afterward."""
    serial = _serial_module()
    connection = serial.Serial(
        port=None,
        baudrate=BAUDRATE,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=READ_TIMEOUT,
        write_timeout=WRITE_TIMEOUT,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
    )
    connection.dtr = False
    connection.rts = False
    connection.port = port
    try:
        connection.open()
    except Exception:
        connection.close()
        raise
    return connection


def make_pattern(size: int, seed: int) -> bytes:
    """Include every byte value, then reproducible pseudorandom binary data."""
    if size < 256:
        raise ValueError("--bytes must be at least 256 to test every byte value")
    rng = random.Random(seed)
    return bytes(range(256)) + bytes(rng.getrandbits(8) for _ in range(size - 256))


@dataclass(frozen=True)
class LoopbackResult:
    status: str
    expected_bytes: int
    sent_bytes: int
    received_bytes: int
    elapsed_seconds: float
    rate_limit_bytes_per_second: float
    sent_sha256: str
    received_sha256: str
    error: Optional[str]


def run_loopback(
    connection, payload: bytes, timeout: float = 20.0, quiet: float = 0.2,
    rate: float = DEFAULT_RATE,
) -> LoopbackResult:
    """Transmit and receive concurrently, requiring exact bytes and quiet tail.

    The caller supplies an already-open binary stream with bounded read/write
    timeouts, such as ``open_serial``. No buffers are reset: unsolicited bytes
    count against the test. A pass includes ``quiet`` seconds of observed
    trailing silence, within the overall deadline.
    """
    if not payload:
        raise ValueError("payload must not be empty")
    if not math.isfinite(timeout) or not math.isfinite(quiet) or quiet <= 0 or timeout <= quiet:
        raise ValueError("timeout must be finite and greater than positive quiet time")
    if not math.isfinite(rate) or rate < 0:
        raise ValueError("rate must be finite and nonnegative (zero disables pacing)")
    started = time.monotonic()
    deadline = started + timeout
    stop = threading.Event()
    writer_done = threading.Event()
    lock = threading.Lock()
    state = {"sent": 0, "error": None}

    def writer():
        offset = 0
        chunk_index = 0
        next_write = started
        try:
            while offset < len(payload) and not stop.is_set():
                end = min(offset + WRITE_SIZES[chunk_index % len(WRITE_SIZES)], len(payload))
                chunk_index += 1
                while offset < end and not stop.is_set():
                    if rate:
                        delay = next_write - time.monotonic()
                        if delay > 0:
                            stop.wait(min(delay, max(0, deadline - time.monotonic())))
                        if stop.is_set():
                            return
                    if time.monotonic() >= deadline:
                        return
                    write_end = min(end, offset + PACED_WRITE_SIZE) if rate else end
                    count = connection.write(payload[offset:write_end])
                    if count is None or count < 0 or count > write_end - offset:
                        raise RuntimeError("serial write returned an invalid byte count")
                    if count == 0:
                        stop.wait(0.001)
                        continue
                    offset += count
                    if rate:
                        # Do not accumulate credit while a write is blocked.
                        next_write = max(next_write, time.monotonic()) + count / rate
                    with lock:
                        state["sent"] = offset
        except Exception as exc:
            with lock:
                state["error"] = "write failed: {}".format(exc)
        finally:
            writer_done.set()

    thread = threading.Thread(target=writer, name="serial-probe-tx", daemon=True)
    received = bytearray()
    error = None
    quiet_deadline = None
    settled = False
    thread.start()
    try:
        while time.monotonic() < deadline:
            with lock:
                write_error = state["error"]
            if write_error is not None:
                error = write_error
                break
            # Read one byte beyond the expected length to detect extra output.
            count = min(READ_SIZE, max(1, len(payload) + 1 - len(received)))
            try:
                chunk = connection.read(count)
            except Exception as exc:
                error = "read failed: {}".format(exc)
                break
            if chunk:
                received.extend(chunk)
                if len(received) > len(payload):
                    error = "received unexpected extra bytes"
                    break
            now = time.monotonic()
            if writer_done.is_set() and len(received) == len(payload):
                if quiet_deadline is None:
                    quiet_deadline = now + quiet
                if now >= quiet_deadline and now <= deadline:
                    settled = True
                    break
    finally:
        stop.set()
        thread.join(WRITE_TIMEOUT + 0.5)
    with lock:
        sent = state["sent"]
        write_error = state["error"]
    if error is None:
        if thread.is_alive():
            error = "writer did not stop; connection did not honor its write timeout"
        elif write_error is not None:
            error = write_error
        elif sent != len(payload):
            error = "timeout before all bytes were sent"
        elif len(received) != len(payload):
            error = "timeout before the exact response was received"
        elif received != payload:
            mismatch = next(i for i, pair in enumerate(zip(payload, received)) if pair[0] != pair[1])
            error = "byte mismatch at offset {} (expected 0x{:02x}, received 0x{:02x})".format(
                mismatch, payload[mismatch], received[mismatch]
            )
        elif not settled:
            error = "timeout before trailing silence was verified"
    return LoopbackResult(
        status="pass" if error is None else "fail",
        expected_bytes=len(payload),
        sent_bytes=sent,
        received_bytes=len(received),
        elapsed_seconds=round(time.monotonic() - started, 6),
        rate_limit_bytes_per_second=rate,
        sent_sha256=hashlib.sha256(payload[:sent]).hexdigest(),
        received_sha256=hashlib.sha256(received).hexdigest(),
        error=error,
    )


@dataclass(frozen=True)
class CaptureResult:
    status: str
    received_bytes: int
    elapsed_seconds: float
    received_sha256: str
    stopped_by: str


def capture_raw(
    connection, output: BinaryIO, duration: float, max_bytes: int
) -> CaptureResult:
    """Read only; preserve every byte and stop at either explicit bound."""
    if not math.isfinite(duration) or duration <= 0 or max_bytes <= 0:
        raise ValueError("capture duration and byte limit must be positive")
    started = time.monotonic()
    received = 0
    digest = hashlib.sha256()
    while received < max_bytes and time.monotonic() - started < duration:
        chunk = connection.read(min(READ_SIZE, max_bytes - received))
        if chunk:
            output.write(chunk)
            digest.update(chunk)
            received += len(chunk)
    return CaptureResult(
        status="captured",
        received_bytes=received,
        elapsed_seconds=round(time.monotonic() - started, 6),
        received_sha256=digest.hexdigest(),
        stopped_by="byte_limit" if received == max_bytes else "duration",
    )


def run_one_shot(
    connection, output: BinaryIO, *, prompt: bytes, line: bytes,
    duration: float, reply_duration: float, max_bytes: int,
) -> CaptureResult:
    """Preserve the raw stream and write one framed request after a quiet prompt."""
    started = time.monotonic()
    deadline = started + duration
    digest = hashlib.sha256()
    received = 0
    tail = b""
    prompt_seen_at = None
    status = "no_request_written"
    stopped_by = "prompt_timeout"
    frame = LUATOS_FRAME_PREFIX + line + b"\n"

    while received < max_bytes and time.monotonic() < deadline:
        chunk = connection.read(min(READ_SIZE, max_bytes - received))
        now = time.monotonic()
        if chunk:
            output.write(chunk)
            digest.update(chunk)
            received += len(chunk)
            if status == "no_request_written":
                # One preceding byte suffices to tell a full line from a suffix.
                tail = (tail + chunk)[-(len(prompt) + 1):]
                whole_prompt = tail.endswith(prompt) and (
                    len(tail) == len(prompt) or tail[:1] in (b"\r", b"\n")
                )
                prompt_seen_at = now if whole_prompt else None
        elif (status == "no_request_written" and prompt_seen_at is not None
              and now < deadline and now - prompt_seen_at >= PROMPT_QUIET_SECONDS):
            # A partially written frame is uncertain. Never retry it.
            if connection.write(frame) != len(frame):
                status = "short_bridge_write"
                stopped_by = "short_write"
                break
            status = "request_written_to_bridge"
            stopped_by = "reply_duration"
            deadline = time.monotonic() + reply_duration

    return CaptureResult(
        status=status,
        received_bytes=received,
        elapsed_seconds=round(time.monotonic() - started, 6),
        received_sha256=digest.hexdigest(),
        stopped_by="byte_limit" if received == max_bytes else stopped_by,
    )


def _require_luatos_port(port: str) -> None:
    _serial_module()
    from serial.tools import list_ports

    if not any(
        device.device == port and (device.vid, device.pid) == LUATOS_USB_ID
        for device in list_ports.comports()
    ):
        raise ValueError("one-shot requires the LuatOS CH343 serial port (1a86:55d3)")


def _printable_ascii(value: str, limit: int) -> bytes:
    if not 1 <= len(value) <= limit or any(not 0x20 <= ord(char) <= 0x7E for char in value):
        raise argparse.ArgumentTypeError(f"must contain 1 to {limit} printable ASCII bytes")
    return value.encode("ascii")


def _positive_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a positive finite number")
    return number


def _positive_int(value: str) -> int:
    number = int(value, 0)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def _nonnegative_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError("must be finite and nonnegative")
    return number


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ESP32-C3 raw USB–UART bridge probe (115200, 8N1).",
        epilog=(
            "Loopback sends arbitrary binary bytes; use an isolated UART loopback or "
            "the internal diagnostic build. A normal receive-only build cannot echo. "
            "Port opening may reset native Espressif USB despite DTR/RTS preconfiguration."
        ),
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list", help="enumerate local serial ports without opening them")
    loopback = commands.add_parser("loopback", help="send binary data and require an exact echo")
    loopback.add_argument("port")
    loopback.add_argument("--bytes", type=_positive_int, default=65536, dest="byte_count")
    loopback.add_argument("--seed", type=lambda value: int(value, 0), default=0xC3)
    loopback.add_argument("--timeout", type=_positive_float, default=20.0)
    loopback.add_argument("--quiet", type=_positive_float, default=0.2,
                          help="required trailing silence in seconds (default: 0.2)")
    loopback.add_argument("--rate", type=_nonnegative_float, default=DEFAULT_RATE,
                          help="TX bytes/second (default: 9216); 0 for unpaced internal diagnostics")
    loopback.add_argument("--start-delay", type=_nonnegative_float, default=0.0,
                          help="seconds after port open before TX, allowing BOOT arming (e.g. 3)")
    loopback.add_argument("--startup-log", type=Path,
                          help="explicit raw startup capture before strict loopback; never overwrite")
    loopback.add_argument("--startup-duration", type=_positive_float,
                          help="startup capture seconds (default with --startup-log: 1.0)")
    capture = commands.add_parser("capture", help="save raw received bytes; send no data")
    capture.add_argument("port")
    capture.add_argument("--output", type=Path, required=True,
                         help="explicit raw output path; existing files are never overwritten")
    capture.add_argument("--duration", type=_positive_float, default=5.0)
    capture.add_argument("--max-bytes", type=_positive_int, default=1048576)
    one_shot = commands.add_parser("one-shot", help="request one LuatOS UART line after an exact prompt")
    one_shot.add_argument("port")
    one_shot.add_argument("--prompt", type=lambda value: _printable_ascii(value, 512), required=True,
                          help="literal console prompt, including any trailing spaces")
    one_shot.add_argument("--line", type=lambda value: _printable_ascii(value, LUATOS_LINE_LIMIT), required=True,
                          help="printable command text; firmware adds a newline")
    one_shot.add_argument("--output", type=Path, required=True,
                          help="new file for unchanged UART output")
    one_shot.add_argument("--duration", type=_positive_float, default=120.0,
                          help="seconds to wait for the prompt (default: 120)")
    one_shot.add_argument("--reply-duration", type=_positive_float, default=10.0,
                          help="seconds to capture after the bridge request (default: 10)")
    one_shot.add_argument("--max-bytes", type=_positive_int, default=1048576)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "loopback" and (args.byte_count < 256 or args.timeout <= args.quiet):
        parser.error("loopback requires --bytes >= 256 and --timeout > --quiet")
    if args.command == "loopback" and args.startup_duration is not None and args.startup_log is None:
        parser.error("--startup-duration requires --startup-log")
    try:
        if args.command == "list":
            _serial_module()
            from serial.tools import list_ports

            for port in sorted(list_ports.comports(), key=lambda item: item.device):
                usb = ""
                if port.vid is not None and port.pid is not None:
                    usb = " {:04x}:{:04x}".format(port.vid, port.pid)
                print("{}{} {}".format(port.device, usb, port.description))
            return 0
        if args.command == "loopback":
            payload = make_pattern(args.byte_count, args.seed)
            with ExitStack() as resources:
                # Reserve the optional evidence file before touching the port.
                startup_output = resources.enter_context(args.startup_log.open("xb")) if (
                    args.startup_log is not None
                ) else None
                connection = open_serial(args.port)
                resources.callback(connection.close)
                startup = None
                if startup_output is not None:
                    startup = capture_raw(
                        connection, startup_output,
                        args.startup_duration if args.startup_duration is not None else 1.0,
                        STARTUP_MAX_BYTES,
                    )
                    startup_output.flush()
                    if startup.stopped_by == "byte_limit":
                        print(json.dumps({
                            "status": "fail",
                            "error": "startup capture byte limit reached; strict loopback was not started",
                            "startup_capture": asdict(startup),
                        }, sort_keys=True))
                        return 1
                if args.start_delay:
                    print("Port opened; waiting {} seconds before TX.".format(args.start_delay),
                          file=sys.stderr, flush=True)
                    time.sleep(args.start_delay)
                result = run_loopback(connection, payload, args.timeout, args.quiet, args.rate)
            report = asdict(result)
            if startup is not None:
                report["startup_capture"] = asdict(startup)
            print(json.dumps(report, sort_keys=True))
            return 0 if result.status == "pass" else 1
        # Reserve the explicit output before touching the port. Never overwrite.
        with args.output.open("xb") as output:
            if args.command == "one-shot":
                _require_luatos_port(args.port)
            connection = open_serial(args.port)
            try:
                if args.command == "one-shot":
                    print("Waiting for the literal prompt.", file=sys.stderr, flush=True)
                    result = run_one_shot(
                        connection, output, prompt=args.prompt, line=args.line, duration=args.duration,
                        reply_duration=args.reply_duration, max_bytes=args.max_bytes,
                    )
                else:
                    result = capture_raw(connection, output, args.duration, args.max_bytes)
            finally:
                connection.close()
        print(json.dumps(asdict(result), sort_keys=True))
        return 0 if result.status in ("captured", "request_written_to_bridge") else 1
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print("error: {}".format(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
