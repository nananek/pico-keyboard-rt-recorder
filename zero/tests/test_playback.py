from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.errors import RecorderError
from app.playback import PlaybackSession, expand_offsets
from app.recording import Recording, RecordingEvent
from app.transport import PicoTransport
from app.uart_protocol import (
    BUFFER_STATUS,
    MAGIC,
    MODE_ARMED,
    MODE_CHANGED,
    MODE_PASS,
    MODE_PLAYING,
    MODE_SET,
    PLAY_ABORT,
    PLAY_ABORTED,
    PLAY_FINISHED,
    PLAY_METRICS,
    PLAY_READY,
    PLAY_START,
    PLAY_STARTED,
    PLAY_UNDERRUN,
    QUEUE_CLEAR,
    QUEUE_END,
    QUEUE_EVENT,
    REASON_ABORTED,
    REASON_FINISHED,
    VERSION,
    crc16_ccitt_false,
)


def pico_frame(message_type, payload=b""):
    covered = bytes((VERSION, message_type)) + struct.pack("<H", len(payload)) + payload
    return bytes((MAGIC,)) + covered + struct.pack("<H", crc16_ccitt_false(covered))


def recording_from_deltas(deltas):
    events = tuple(
        RecordingEvent(delta, (0, 0, index + 1, 0, 0, 0, 0, 0))
        for index, delta in enumerate(deltas)
    )
    return Recording("demo", sum(deltas), events)


class PlaybackPicoStream:
    """Protocol-level Pico fake with a small streaming queue."""

    def __init__(self, *, capacity=8, underrun_credit=False):
        self.capacity = capacity
        self.underrun_credit = underrun_credit
        self.mode = MODE_PASS
        self.queue_count = 0
        self.total_events = 0
        self.ended = False
        self.reads = []
        self.writes = []
        self.actions = []
        self.closed = False
        self._sent_underrun = False

    def _append(self, message_type, payload=b""):
        self.reads.append(pico_frame(message_type, payload))

    def _buffer_status(self):
        self._append(
            BUFFER_STATUS,
            struct.pack("<BHH", self.mode, self.queue_count, self.capacity - self.queue_count),
        )

    def _metrics(self, underruns=0):
        return struct.pack(
            "<IIiiqiiB",
            self.total_events,
            underruns,
            0,
            0,
            0,
            0,
            0,
            0,
        )

    def _finish(self):
        self._append(PLAY_FINISHED)
        self._append(PLAY_METRICS, self._metrics(int(self._sent_underrun)))
        self.mode = MODE_ARMED
        self._append(MODE_CHANGED, bytes((MODE_ARMED, REASON_FINISHED)))

    def read(self, _size=1):
        self.actions.append("R")
        if self.reads:
            return self.reads.pop(0)
        # A full streaming queue grants credit when its first event is
        # dispatched. This is the new asynchronous BUFFER_STATUS edge.
        if self.mode == MODE_PLAYING and not self.ended and self.queue_count == self.capacity:
            if self.underrun_credit and not self._sent_underrun:
                self.queue_count = 0
                self._sent_underrun = True
                return pico_frame(
                    PLAY_UNDERRUN,
                    struct.pack("<QH", 750_000, self.capacity),
                )
            self.queue_count -= 1
            return pico_frame(
                BUFFER_STATUS,
                struct.pack("<BHH", self.mode, self.queue_count, self.capacity - self.queue_count),
            )
        return b""

    def write(self, data):
        self.writes.append(data)
        self.actions.append(f"W{data[2]:02x}")
        message_type = data[2]
        payload_len = struct.unpack_from("<H", data, 3)[0]
        payload = data[5 : 5 + payload_len]

        if message_type == MODE_SET:
            self.mode = payload[0]
            self._append(MODE_CHANGED, bytes((self.mode, 0)))
        elif message_type == QUEUE_CLEAR:
            self.queue_count = 0
            self.ended = False
            self._buffer_status()
        elif message_type == QUEUE_EVENT:
            if self.queue_count >= self.capacity:
                raise AssertionError("Zero exceeded advertised queue credit")
            self.queue_count += 1
            self.total_events += 1
            self._buffer_status()
        elif message_type == QUEUE_END:
            self.ended = True
            if self.mode == MODE_ARMED:
                self._append(PLAY_READY)
                self._buffer_status()
            else:
                self._buffer_status()
                self._finish()
        elif message_type == PLAY_START:
            self.mode = MODE_PLAYING
            self._append(MODE_CHANGED, bytes((MODE_PLAYING, 0)))
            self._append(PLAY_STARTED, struct.pack("<Q", 1_000_000))
            if self.ended:
                self._finish()
        elif message_type == PLAY_ABORT:
            self._append(PLAY_ABORTED)
            self._append(PLAY_METRICS, self._metrics())
            self.mode = MODE_ARMED
            self._append(MODE_CHANGED, bytes((MODE_ARMED, REASON_ABORTED)))
        return len(data)

    def close(self):
        self.closed = True


class PlaybackTests(unittest.TestCase):
    def test_expand_offsets_and_speed_scaling(self):
        events = recording_from_deltas([0, 100, 300]).events
        self.assertEqual([item[0] for item in expand_offsets(events)], [0, 100, 400])
        self.assertEqual([item[0] for item in expand_offsets(events, 2.0)], [0, 50, 200])
        self.assertEqual([item[0] for item in expand_offsets(events, 0.5)], [0, 200, 800])
        self.assertEqual(expand_offsets([], 1.0), [])
        for speed in (0, -1, float("inf"), float("nan")):
            with self.subTest(speed=speed), self.assertRaises(RecorderError):
                expand_offsets(events, speed)

    def test_prebuffers_500ms_then_pipelines_streaming_and_returns_pass(self):
        stream = PlaybackPicoStream(capacity=8)
        session = PlaybackSession(
            PicoTransport(stream),
            recording_from_deltas([0, 250_000, 250_000, 100_000, 100_000, 100_000, 100_000]),
        )

        session.start()
        types_at_start = [frame[2] for frame in stream.writes]
        play_start_at = types_at_start.index(PLAY_START)
        self.assertEqual(types_at_start[:play_start_at].count(QUEUE_EVENT), 3)
        self.assertNotIn(QUEUE_END, types_at_start[:play_start_at])

        result = session.finish()
        self.assertEqual(result.outcome, "finished")
        self.assertEqual(result.playback_start_us, 1_000_000)
        self.assertEqual(result.metrics.dispatched_count, 7)
        types = [frame[2] for frame in stream.writes]
        self.assertEqual(types.count(QUEUE_END), 1)
        self.assertGreater(types.index(QUEUE_END), types.index(PLAY_START))
        self.assertEqual(types[-1], MODE_SET)
        self.assertEqual(stream.mode, MODE_PASS)

        # Four remaining QUEUE_EVENT frames fit the local pipeline window and
        # are written consecutively before their BUFFER_STATUS replies drain.
        self.assertIn(
            ["W81", "W81", "W81", "W81"],
            [stream.actions[index : index + 4] for index in range(len(stream.actions) - 3)],
        )

    def test_recording_shorter_than_prebuffer_closes_before_play_start(self):
        stream = PlaybackPicoStream(capacity=4)
        result = PlaybackSession(
            PicoTransport(stream), recording_from_deltas([0, 100_000])
        ).play()

        types = [frame[2] for frame in stream.writes]
        self.assertLess(types.index(QUEUE_END), types.index(PLAY_START))
        self.assertEqual(types.count(QUEUE_END), 1)
        self.assertEqual(result.outcome, "finished")

    def test_streams_beyond_capacity_from_async_credit(self):
        stream = PlaybackPicoStream(capacity=4)
        result = PlaybackSession(
            PicoTransport(stream),
            recording_from_deltas([0, 500_000, 100_000, 100_000, 100_000, 100_000, 100_000]),
        ).play()

        self.assertEqual(result.metrics.dispatched_count, 7)
        self.assertEqual(sum(frame[2] == QUEUE_EVENT for frame in stream.writes), 7)

    def test_underrun_notice_supplies_credit_and_is_reported(self):
        stream = PlaybackPicoStream(capacity=3, underrun_credit=True)
        result = PlaybackSession(
            PicoTransport(stream),
            recording_from_deltas([0, 500_000, 100_000, 100_000, 100_000]),
        ).play()

        self.assertEqual(len(result.underruns), 1)
        self.assertEqual(result.underruns[0].elapsed_offset_us, 750_000)
        self.assertEqual(result.underruns[0].free_capacity, 3)
        self.assertEqual(result.metrics.underrun_count, 1)

    def test_abort_consumes_outcome_metrics_and_returns_pass(self):
        stream = PlaybackPicoStream(capacity=4)
        session = PlaybackSession(
            PicoTransport(stream),
            recording_from_deltas([0, 500_000, 100_000]),
        )
        session.start()
        result = session.abort()

        self.assertIsNotNone(result)
        self.assertEqual(result.outcome, "aborted")
        self.assertIn(PLAY_ABORT, [frame[2] for frame in stream.writes])
        self.assertEqual(stream.mode, MODE_PASS)

    def test_prebuffer_density_beyond_capacity_fails_before_play_start(self):
        stream = PlaybackPicoStream(capacity=2)
        session = PlaybackSession(
            PicoTransport(stream),
            recording_from_deltas([0, 100_000, 100_000, 300_000, 100_000]),
        )
        with self.assertRaisesRegex(RecorderError, "cannot prebuffer"):
            session.start()
        self.assertNotIn(PLAY_START, [frame[2] for frame in stream.writes])


if __name__ == "__main__":
    unittest.main()
