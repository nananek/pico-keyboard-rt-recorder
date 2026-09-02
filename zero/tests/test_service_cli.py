import contextlib
import io
import struct
import tempfile
import unittest
from unittest.mock import patch

from app import cli
from app.errors import ModeRejected, PicoError, ProtocolError, RecorderError, TransportTimeout
from app.recording import RecordingStore
from app.service import RecordingSession
from app.transport import PicoTransport
from app.uart_protocol import ERROR, MAGIC, MODE_CHANGED, MODE_PASS, MODE_RECORD, PICO_STATUS, RECORD_EVENT, VERSION, crc16_ccitt_false


def pico_frame(message_type, payload=b""):
    covered = bytes((VERSION, message_type)) + struct.pack("<H", len(payload)) + payload
    return bytes((MAGIC,)) + covered + struct.pack("<H", crc16_ccitt_false(covered))


def record_event(timestamp, report):
    return pico_frame(RECORD_EVENT, struct.pack("<QB8s", timestamp, 8, bytes(report)))


def mode_changed(state, reason=0):
    return pico_frame(MODE_CHANGED, bytes((state, reason)))


class FakeStream:
    def __init__(self, reads):
        self.reads = list(reads)
        self.writes = []
        self.closed = False

    def read(self, _size=1):
        if not self.reads:
            return b""
        next_read = self.reads.pop(0)
        if isinstance(next_read, BaseException):
            raise next_read
        return next_read

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def close(self):
        self.closed = True


class ManualClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class TimeoutThenFrameStream(FakeStream):
    def __init__(self, frame, clock):
        super().__init__([])
        self.frame = frame
        self.clock = clock
        self.read_count = 0

    def read(self, _size=1):
        self.read_count += 1
        if self.read_count == 1:
            self.clock.now += 1.0
            return b""
        return self.frame


class ServiceAndCliTests(unittest.TestCase):
    def test_mode_handshake_retries_when_acknowledgement_is_missing(self):
        clock = ManualClock()
        stream = TimeoutThenFrameStream(mode_changed(MODE_RECORD), clock)
        transport = PicoTransport(stream, clock=clock)

        transport.set_mode(MODE_RECORD, timeout=1.0)

        self.assertEqual(len(stream.writes), 2)

    def test_stop_requires_a_successful_record_start(self):
        stream = FakeStream([])
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            with self.assertRaisesRegex(RecorderError, "has not started"):
                session.stop()
        self.assertEqual(stream.writes, [])

    def test_mode_handshake_preserves_event_before_ack_and_persists_after_pass(self):
        stream = FakeStream(
            [
                record_event(100, (0,) * 8) + mode_changed(MODE_RECORD),
                record_event(140, (1,) * 8) + mode_changed(MODE_PASS),
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            session.start()
            session.receive_and_consume(timeout=0.1)
            session.receive_and_consume(timeout=0.1)
            recording = session.stop()
            self.assertEqual([event.dt_us for event in recording.events], [0, 40])
            self.assertEqual(RecordingStore(directory).load("hello"), recording)
        self.assertEqual(len(stream.writes), 2)

    def test_stop_persists_reports_queued_before_pass_ack(self):
        stream = FakeStream(
            [
                mode_changed(MODE_RECORD),
                record_event(100, (0,) * 8) + record_event(160, (1,) * 8) + mode_changed(MODE_PASS),
            ]
        )
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            session.start()
            recording = session.stop()
            self.assertEqual([event.dt_us for event in recording.events], [0, 60])
            self.assertEqual(RecordingStore(directory).load("hello"), recording)
        self.assertEqual(len(stream.writes), 2)

    def test_invalid_frame_aborts_without_publishing_and_attempts_pass(self):
        corrupt = bytearray(record_event(100, (0,) * 8))
        corrupt[-1] ^= 0xFF
        stream = FakeStream([mode_changed(MODE_RECORD), bytes(corrupt), mode_changed(MODE_PASS)])
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            session.start()
            with self.assertRaises(ProtocolError):
                session.receive_and_consume(timeout=0.1)
            self.assertIsNone(session.abort())
            self.assertFalse(RecordingStore(directory).exists("hello"))
        self.assertEqual(len(stream.writes), 2)

    def test_rejection_timeout_and_pico_error_do_not_publish(self):
        cases = [
            (
                "rejected",
                FakeStream([mode_changed(MODE_PASS, 1), mode_changed(MODE_PASS)]),
                ModeRejected,
                0.1,
            ),
            (
                "timeout",
                FakeStream([]),
                TransportTimeout,
                0.001,
            ),
            (
                "pico-error",
                FakeStream([mode_changed(MODE_RECORD), pico_frame(ERROR), mode_changed(MODE_PASS)]),
                PicoError,
                0.1,
            ),
        ]
        for label, stream, expected_error, timeout in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello", mode_timeout=timeout)
                if label == "pico-error":
                    session.start()
                    with self.assertRaises(expected_error):
                        session.receive_and_consume(timeout=timeout)
                else:
                    with self.assertRaises(expected_error):
                        session.start()
                session.abort()
                self.assertFalse(RecordingStore(directory).exists("hello"))

    def test_serial_disconnect_does_not_publish_and_still_attempts_pass(self):
        stream = FakeStream([mode_changed(MODE_RECORD), OSError("disconnected"), mode_changed(MODE_PASS)])
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            session.start()
            with self.assertRaisesRegex(ProtocolError, "could not read"):
                session.receive_and_consume(timeout=0.1)
            self.assertIsNone(session.abort())
            self.assertFalse(RecordingStore(directory).exists("hello"))
        self.assertEqual(len(stream.writes), 2)

    def test_pico_receive_overflow_status_does_not_publish(self):
        stream = FakeStream([mode_changed(MODE_RECORD), pico_frame(PICO_STATUS, bytes((MODE_RECORD, 1, 0, 0, 0))), mode_changed(MODE_PASS)])
        with tempfile.TemporaryDirectory() as directory:
            session = RecordingSession(PicoTransport(stream), RecordingStore(directory), "hello")
            session.start()
            with self.assertRaisesRegex(PicoError, "rx_overflow"):
                session.receive_and_consume(timeout=0.1)
            self.assertIsNone(session.abort())
            self.assertFalse(RecordingStore(directory).exists("hello"))

    def test_cli_record_writes_machine_readable_json(self):
        stream = FakeStream(
            [
                record_event(100, (0,) * 8) + mode_changed(MODE_RECORD),
                record_event(120, (0,) * 8),
                KeyboardInterrupt(),
                mode_changed(MODE_PASS),
            ]
        )
        with tempfile.TemporaryDirectory() as directory, patch("app.cli._open_transport", return_value=(PicoTransport(stream), stream)):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                status = cli.main(["--recordings-dir", directory, "record", "hello", "--device", "fake"])
            self.assertEqual(status, 0)
            self.assertIn('"ok":true', stdout.getvalue())
            stored = RecordingStore(directory).load("hello")
            self.assertEqual([event.dt_us for event in stored.events], [0, 20])
        self.assertTrue(stream.closed)

    def test_cli_rejects_invalid_name_before_opening_serial(self):
        with patch("app.cli._open_transport") as open_transport:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                status = cli.main(["record", "../escape", "--device", "fake"])
        self.assertEqual(status, 3)
        open_transport.assert_not_called()
        self.assertIn('"ok":false', stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
