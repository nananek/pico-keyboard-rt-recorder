import contextlib
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fastapi.testclient import TestClient

from app import web
from app.recording import Recording, RecordingBuilder, RecordingStore
from app.transport import PicoTransport
from app.uart_protocol import (
    BUFFER_STATUS,
    MAGIC,
    MODE_ARMED,
    MODE_CHANGED,
    MODE_PASS,
    MODE_PLAYING,
    MODE_RECORD,
    PLAY_FINISHED,
    PLAY_METRICS,
    PLAY_READY,
    PLAY_STARTED,
    RECORD_EVENT,
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

    Mirrors test_web_api.py's fixture: lets a test push frames only after
    observing a background worker thread is where it expects it to be.
    """

    def __init__(self):
        import queue

        self._queue = queue.Queue()
        self.writes = []
        self.closed = False

    def push(self, data: bytes) -> None:
        self._queue.put(data)

    def read(self, _size: int = 1) -> bytes:
        import queue

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
def running_app(*, recordings_dir=None, record_poll_interval=0.02, status_push_interval=0.02):
    with tempfile.TemporaryDirectory() as directory:
        stream = LiveStream()
        stream.push(mode_changed(MODE_PASS))  # startup reconciliation ack
        transport = PicoTransport(stream)
        app = web.create_app(
            recordings_dir=Path(recordings_dir or directory),
            transport=transport,
            record_poll_interval=record_poll_interval,
            status_push_interval=status_push_interval,
        )
        with TestClient(app) as client:
            try:
                yield client, stream, Path(recordings_dir or directory)
            finally:
                stream.push(mode_changed(MODE_PASS))


def wait_until(predicate, *, timeout=2.0, interval=0.01):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


class StaticFrontendTests(unittest.TestCase):
    def test_index_is_served_at_root(self):
        with running_app() as (client, _stream, _directory):
            response = client.get("/")
            self.assertEqual(response.status_code, 200)
            self.assertIn("text/html", response.headers["content-type"])
            self.assertIn("Pico Keyboard Recorder", response.text)

    def test_app_js_and_style_css_are_served(self):
        with running_app() as (client, _stream, _directory):
            script = client.get("/app.js")
            self.assertEqual(script.status_code, 200)

            style = client.get("/style.css")
            self.assertEqual(style.status_code, 200)

    def test_api_routes_take_priority_over_the_static_mount(self):
        with running_app() as (client, _stream, _directory):
            response = client.get("/api/status")
            self.assertEqual(response.status_code, 200)
            self.assertTrue(response.json()["ok"])


class StatusWebSocketTests(unittest.TestCase):
    def test_connect_receives_an_idle_snapshot_with_null_diagnostics(self):
        with running_app() as (client, _stream, _directory):
            with client.websocket_connect("/api/ws") as websocket:
                snapshot = websocket.receive_json()
                self.assertEqual(snapshot["state"], "PASS")
                self.assertIsNone(snapshot["active"])
                self.assertEqual(
                    snapshot["diagnostics"], {"buffer": None, "playback": None, "recording": None}
                )

    def test_diagnostics_update_live_during_recording_and_clear_afterward(self):
        with running_app() as (client, stream, _directory):
            stream.push(mode_changed(MODE_RECORD))
            start = client.post("/api/recordings/hello/record")
            self.assertEqual(start.status_code, 200)

            with client.websocket_connect("/api/ws") as websocket:
                initial = websocket.receive_json()
                self.assertEqual(initial["diagnostics"]["recording"], {"event_count": 0})

                stream.push(record_event(100, (0,) * 8))

                def saw_one_event():
                    while True:
                        snapshot = websocket.receive_json()
                        if snapshot["diagnostics"]["recording"] == {"event_count": 1}:
                            return True

                self.assertTrue(wait_until(saw_one_event))

            result = {}

            def do_stop():
                result["response"] = client.post("/api/record/stop")

            stop_thread = threading.Thread(target=do_stop)
            stop_thread.start()
            self.assertTrue(wait_until(lambda: len(stream.writes) >= 3))
            stream.push(mode_changed(MODE_PASS))
            stop_thread.join(timeout=5.0)
            self.assertFalse(stop_thread.is_alive())
            self.assertEqual(result["response"].status_code, 200)

            with client.websocket_connect("/api/ws") as websocket:
                after = websocket.receive_json()
                self.assertIsNone(after["active"])
                self.assertEqual(
                    after["diagnostics"], {"buffer": None, "playback": None, "recording": None}
                )

    def test_status_endpoint_reports_playback_diagnostics_while_in_progress(self):
        with running_app() as (client, stream, directory):
            store = RecordingStore(directory)
            builder = RecordingBuilder("hello")
            builder.add(0, (0,) * 8)
            builder.add(2_000_000, (1,) * 8)
            store.save(builder.build())

            # duration_us=2_000_000 exceeds the 500ms default prebuffer, so
            # both events must be queued (and acked) before PLAY_START.
            stream.push(mode_changed(MODE_ARMED))
            stream.push(buffer_status(MODE_ARMED, 0, 512))
            stream.push(buffer_status(MODE_ARMED, 1, 511))
            stream.push(buffer_status(MODE_ARMED, 2, 510))
            stream.push(pico_frame(PLAY_READY))
            stream.push(buffer_status(MODE_ARMED, 2, 510))
            stream.push(mode_changed(MODE_PLAYING))
            stream.push(pico_frame(PLAY_STARTED, struct.pack("<Q", 1234)))
            # Deliberately no PLAY_FINISHED yet: playback is in progress.

            start = client.post("/api/recordings/hello/play", json={})
            self.assertEqual(start.status_code, 200)

            def in_progress_diagnostics():
                status = client.get("/api/status").json()
                return status["diagnostics"]["playback"] is not None

            self.assertTrue(wait_until(in_progress_diagnostics))
            status = client.get("/api/status").json()
            self.assertIsNotNone(status["diagnostics"]["buffer"])
            playback = status["diagnostics"]["playback"]
            self.assertEqual(playback["total_events"], 2)
            self.assertEqual(playback["duration_us"], 2_000_000)
            self.assertIsNone(status["diagnostics"]["recording"])

            # Let the run finish so shutdown reconciliation isn't racing it.
            stream.push(pico_frame(PLAY_FINISHED))
            stream.push(play_metrics(2))
            stream.push(mode_changed(MODE_ARMED, REASON_FINISHED))
            stream.push(mode_changed(MODE_PASS))
            self.assertTrue(wait_until(lambda: client.get("/api/status").json()["state"] == "PASS"))


if __name__ == "__main__":
    unittest.main()
