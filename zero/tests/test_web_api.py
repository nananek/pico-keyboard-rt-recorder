import contextlib
from pathlib import Path
import queue
import struct
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient

from app import web
from app.controller import Controller
from app.errors import RecorderError
from app.recording import Recording, RecordingBuilder, RecordingStore
from app.transport import PicoTransport
from app.uart_protocol import (
    BUFFER_STATUS,
    MODE_ARMED,
    MODE_CHANGED,
    MODE_PASS,
    MODE_PLAYING,
    MODE_RECORD,
    MAGIC,
    PLAY_ABORTED,
    PLAY_FINISHED,
    PLAY_METRICS,
    PLAY_READY,
    PLAY_STARTED,
    RECORD_EVENT,
    REASON_ABORTED,
    REASON_FINISHED,
    VERSION,
    crc16_ccitt_false,
)


def pico_frame(message_type, payload=b""):
    covered = bytes((VERSION, message_type)) + struct.pack("<H", len(payload)) + payload
    return bytes((MAGIC,)) + covered + struct.pack("<H", crc16_ccitt_false(covered))


def record_event(timestamp, report):
    return pico_frame(RECORD_EVENT, struct.pack("<QB8s", timestamp, 8, bytes(report)))


def mode_changed(state, reason=0):
    return pico_frame(MODE_CHANGED, bytes((state, reason)))


def buffer_status(state, queued_count, free_capacity):
    return pico_frame(BUFFER_STATUS, struct.pack("<BHH", state, queued_count, free_capacity))


def play_metrics(dispatched_count=1):
    return pico_frame(PLAY_METRICS, struct.pack("<IIiiqiiB", dispatched_count, 0, 0, 0, 0, 0, 0, 0))


class LiveStream:
    """A serial-like stream fed from a queue, so a reader genuinely blocks.

    Unlike a canned list of reads, this lets a test push frames only after
    observing that a background worker thread is where it expects it to be
    (e.g. blocked waiting for a frame it will never get, mid-playback).
    """

    def __init__(self):
        self._queue: queue.Queue = queue.Queue()
        self.writes = []
        self.closed = False

    def push(self, data: bytes) -> None:
        self._queue.put(data)

    def read(self, _size: int = 1) -> bytes:
        try:
            return self._queue.get(timeout=0.02)
        except queue.Empty:
            return b""

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        return len(data)

    def close(self) -> None:
        self.closed = True


@contextlib.contextmanager
def running_app(*, recordings_dir=None, record_poll_interval=0.02):
    with tempfile.TemporaryDirectory() as directory:
        stream = LiveStream()
        stream.push(mode_changed(MODE_PASS))  # startup reconciliation ack
        transport = PicoTransport(stream)
        app = web.create_app(
            recordings_dir=Path(recordings_dir or directory),
            transport=transport,
            record_poll_interval=record_poll_interval,
        )
        with TestClient(app) as client:
            try:
                yield client, stream, Path(recordings_dir or directory)
            finally:
                # Pushed only now (not up front) so an earlier in-flight
                # request can never consume it before shutdown needs it.
                stream.push(mode_changed(MODE_PASS))


def wait_until(predicate, *, timeout=2.0, interval=0.01):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


class WebApiTests(unittest.TestCase):
    def test_status_reports_pass_after_startup_and_shutdown_reconciliation(self):
        with running_app() as (client, stream, _directory):
            response = client.get("/api/status")
            self.assertEqual(response.status_code, 200)
            body = response.json()
            self.assertTrue(body["ok"])
            self.assertEqual(body["state"], "PASS")
            self.assertIsNone(body["active"])

    def test_list_and_download_recordings(self):
        with running_app() as (client, _stream, directory):
            builder = RecordingBuilder("hello")
            builder.add(1, (0,) * 8)
            RecordingStore(directory).save(builder.build())

            listing = client.get("/api/recordings")
            self.assertEqual(listing.status_code, 200)
            self.assertEqual([item["name"] for item in listing.json()["recordings"]], ["hello"])

            download = client.get("/api/recordings/hello/download")
            self.assertEqual(download.status_code, 200)
            self.assertEqual(download.json()["name"], "hello")

    def test_download_of_missing_recording_is_404(self):
        with running_app() as (client, _stream, _directory):
            response = client.get("/api/recordings/missing/download")
            self.assertEqual(response.status_code, 404)
            self.assertFalse(response.json()["ok"])

    def test_invalid_name_is_400(self):
        with running_app() as (client, _stream, _directory):
            response = client.get("/api/recordings/.hidden/download")
            self.assertEqual(response.status_code, 400)

    def test_record_lifecycle_persists_and_returns_to_pass(self):
        with running_app() as (client, stream, directory):
            stream.push(mode_changed(MODE_RECORD))
            start = client.post("/api/recordings/hello/record")
            self.assertEqual(start.status_code, 200)
            self.assertEqual(start.json()["state"], "RECORD")

            stream.push(record_event(100, (0,) * 8) + record_event(140, (1,) * 8))
            # 1 write for startup's own MODE_SET(PASS) reconciliation, plus
            # MODE_SET(RECORD) from this session's start().
            self.assertEqual(len(stream.writes), 2)

            # The background record loop keeps polling until stop is
            # requested, so the PASS ack must not be queued until
            # session.stop() has actually written MODE_SET(PASS) --
            # otherwise the still-running loop's own receive call could pick
            # it up first and treat it as an unexpected mid-recording mode
            # change (RecordingSession.consume), instead of session.stop()
            # consuming it via its own explicit MODE_CHANGED wait.
            result = {}

            def do_stop():
                result["response"] = client.post("/api/record/stop")

            stop_thread = threading.Thread(target=do_stop)
            stop_thread.start()
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 3))  # MODE_SET(PASS)
            stream.push(mode_changed(MODE_PASS))
            stop_thread.join(timeout=5.0)
            self.assertFalse(stop_thread.is_alive())

            stop = result["response"]
            self.assertEqual(stop.status_code, 200)
            recording = stop.json()["recording"]
            self.assertEqual([event["dt_us"] for event in recording["events"]], [0, 40])

            status = client.get("/api/status").json()
            self.assertEqual(status["state"], "PASS")
            self.assertIsNone(status["active"])
            self.assertEqual(RecordingStore(directory).load("hello").name, "hello")

    def test_second_session_is_rejected_with_409(self):
        with running_app() as (client, stream, _directory):
            stream.push(mode_changed(MODE_RECORD))
            first = client.post("/api/recordings/a/record")
            self.assertEqual(first.status_code, 200)

            second = client.post("/api/recordings/b/record")
            self.assertEqual(second.status_code, 409)
            # The still-active "a" recording is stopped at shutdown by the
            # single MODE_PASS ack `running_app` already queued for that.

    def test_mode_rejected_start_is_409_and_recovers_to_pass(self):
        with running_app() as (client, stream, _directory):
            stream.push(mode_changed(MODE_PASS, 1))  # Pico rejects MODE_SET(RECORD)
            stream.push(mode_changed(MODE_PASS))  # abort()'s cleanup MODE_SET(PASS) ack
            response = client.post("/api/recordings/hello/record")
            self.assertEqual(response.status_code, 409)
            self.assertIn("rejected", response.json()["error"])

            status = client.get("/api/status").json()
            self.assertEqual(status["state"], "PASS")

    def test_rename_and_delete(self):
        with running_app() as (client, _stream, directory):
            store = RecordingStore(directory)
            builder = RecordingBuilder("old")
            builder.add(1, (0,) * 8)
            store.save(builder.build())

            rename = client.post("/api/recordings/old/rename", json={"new_name": "new"})
            self.assertEqual(rename.status_code, 200)
            self.assertFalse(store.exists("old"))
            self.assertTrue(store.exists("new"))

            delete = client.delete("/api/recordings/new")
            self.assertEqual(delete.status_code, 200)
            self.assertFalse(store.exists("new"))

            missing = client.delete("/api/recordings/new")
            self.assertEqual(missing.status_code, 404)

    def test_cannot_rename_or_delete_the_active_recording(self):
        with running_app() as (client, stream, directory):
            stream.push(mode_changed(MODE_RECORD))
            client.post("/api/recordings/hello/record")

            rename = client.post("/api/recordings/hello/rename", json={"new_name": "other"})
            self.assertEqual(rename.status_code, 409)
            delete = client.delete("/api/recordings/hello")
            self.assertEqual(delete.status_code, 409)
            # The still-active recording is stopped at shutdown by the single
            # MODE_PASS ack `running_app` already queued for that.

    def test_playback_completes_normally_and_returns_to_pass(self):
        with running_app() as (client, stream, directory):
            store = RecordingStore(directory)
            store.save(Recording("hello", 0, ()))

            stream.push(mode_changed(MODE_ARMED))
            stream.push(buffer_status(MODE_ARMED, 0, 512))
            stream.push(pico_frame(PLAY_READY))
            stream.push(buffer_status(MODE_ARMED, 0, 512))
            stream.push(mode_changed(MODE_PLAYING))
            stream.push(pico_frame(PLAY_STARTED, struct.pack("<Q", 1234)))
            stream.push(pico_frame(PLAY_FINISHED))
            stream.push(play_metrics(0))
            stream.push(mode_changed(MODE_ARMED, REASON_FINISHED))
            stream.push(mode_changed(MODE_PASS))

            start = client.post("/api/recordings/hello/play", json={})
            self.assertEqual(start.status_code, 200)
            self.assertEqual(start.json()["state"], "PLAYING")

            self.assertTrue(wait_until(lambda: client.get("/api/status").json()["state"] == "PASS"))

    def test_playback_stop_aborts_mid_stream_and_returns_to_pass(self):
        with running_app() as (client, stream, directory):
            store = RecordingStore(directory)
            builder = RecordingBuilder("hello")
            builder.add(0, (0,) * 8)
            store.save(builder.build())

            stream.push(mode_changed(MODE_ARMED))
            stream.push(buffer_status(MODE_ARMED, 0, 512))
            stream.push(buffer_status(MODE_ARMED, 1, 511))
            stream.push(pico_frame(PLAY_READY))
            stream.push(buffer_status(MODE_ARMED, 1, 511))
            stream.push(mode_changed(MODE_PLAYING))
            stream.push(pico_frame(PLAY_STARTED, struct.pack("<Q", 1234)))
            # Deliberately no PLAY_FINISHED: finish() blocks awaiting it.

            start = client.post("/api/recordings/hello/play", json={})
            self.assertEqual(start.status_code, 200)
            self.assertEqual(start.json()["state"], "PLAYING")

            result = {}

            def do_stop():
                result["response"] = client.post("/api/playback/stop")

            # 1 write for startup's own MODE_SET(PASS) reconciliation, plus 5
            # from this session's start() (MODE_SET(ARMED), QUEUE_CLEAR,
            # QUEUE_EVENT, QUEUE_END, PLAY_START).
            self.assertEqual(len(stream.writes), 6)

            stop_thread = threading.Thread(target=do_stop)
            stop_thread.start()
            # Response frames must not be queued until the matching command
            # has actually been written: PlaybackSession.finish() is still
            # scanning for PLAY_FINISHED and defers any wrong-type frame it
            # reads rather than blocking, so anything queued too early gets
            # drained (and discarded as a stray "diagnostic" frame) before
            # cancellation ever gets a chance to see an empty queue and fire.
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 7))  # PLAY_ABORT
            stream.push(pico_frame(PLAY_ABORTED))
            stream.push(play_metrics(0))
            stream.push(mode_changed(MODE_ARMED, REASON_ABORTED))
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 8))  # MODE_SET(PASS)
            stream.push(mode_changed(MODE_PASS))
            stop_thread.join(timeout=5.0)
            self.assertFalse(stop_thread.is_alive())

            response = result["response"]
            self.assertEqual(response.status_code, 200)

            status = client.get("/api/status").json()
            self.assertEqual(status["state"], "PASS")


class ControllerConcurrencyTests(unittest.TestCase):
    def test_stop_during_start_handshake_is_a_clean_conflict_not_a_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            stream = LiveStream()
            transport = PicoTransport(stream)
            controller = Controller(transport, RecordingStore(directory), record_poll_interval=0.02)

            result = {}

            def do_start():
                result["response"] = controller.start_recording("hello")

            start_thread = threading.Thread(target=do_start)
            start_thread.start()
            # start_recording's mode handshake blocks here waiting for
            # MODE_CHANGED(RECORD): active_kind is already "record" but the
            # worker thread and stop_event don't exist yet.
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 1))  # MODE_SET(RECORD)

            with self.assertRaisesRegex(RecorderError, "still starting"):
                controller.stop_recording(timeout=1.0)

            stream.push(mode_changed(MODE_RECORD))
            start_thread.join(timeout=5.0)
            self.assertFalse(start_thread.is_alive())
            self.assertEqual(result["response"]["state"], "RECORD")

            # Clean up the now-running recording.
            stop_result = {}

            def do_stop():
                stop_result["response"] = controller.stop_recording(timeout=5.0)

            stop_thread = threading.Thread(target=do_stop)
            stop_thread.start()
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 2))  # MODE_SET(PASS)
            stream.push(mode_changed(MODE_PASS))
            stop_thread.join(timeout=5.0)
            self.assertFalse(stop_thread.is_alive())


if __name__ == "__main__":
    unittest.main()
