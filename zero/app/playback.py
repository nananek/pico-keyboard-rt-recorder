"""Prebuffered, credit-driven playback feeding for the Pico."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
import math
import threading

from .errors import ProtocolError, RecorderError, TransportTimeout
from .recording import MAX_U64, Recording, RecordingEvent
from .transport import PicoTransport
from .uart_protocol import (
    BUFFER_STATUS,
    MODE_ARMED,
    MODE_CHANGED,
    MODE_PASS,
    MODE_PLAYING,
    PLAY_ABORTED,
    PLAY_FINISHED,
    PLAY_METRICS,
    PLAY_READY,
    PLAY_STARTED,
    PLAY_UNDERRUN,
    REASON_ABORTED,
    REASON_FINISHED,
    REASON_OK,
    Frame,
    PlayMetrics,
    validate_buffer_status,
    validate_mode_changed,
    validate_play_metrics,
    validate_play_started,
    validate_play_underrun,
)


DEFAULT_PREBUFFER_MS = 500.0
# Pico's command ring holds eight entries. Four pipelined QUEUE_EVENT writes
# leave headroom for commands already in flight while eliminating the old
# stop-and-wait round trip per event.
PIPELINE_WINDOW = 4


@dataclass(frozen=True)
class UnderrunNotice:
    elapsed_offset_us: int
    free_capacity: int


@dataclass(frozen=True)
class PlaybackResult:
    outcome: str
    playback_start_us: int
    metrics: PlayMetrics
    underruns: tuple[UnderrunNotice, ...]


def expand_offsets(
    events: Sequence[RecordingEvent],
    speed: float = 1.0,
) -> list[tuple[int, bytes]]:
    """Expand relative event deltas and apply playback speed.

    Offsets remain anchored to the one Pico epoch sampled at PLAY_START;
    Linux never sleeps until an event deadline or schedules HID output.
    """
    if isinstance(speed, bool) or not isinstance(speed, (int, float)):
        raise RecorderError("playback speed must be a finite number greater than zero")
    speed = float(speed)
    if not math.isfinite(speed) or speed <= 0.0:
        raise RecorderError("playback speed must be a finite number greater than zero")

    cumulative_us = 0
    expanded: list[tuple[int, bytes]] = []
    for event in events:
        cumulative_us += event.dt_us
        if cumulative_us > MAX_U64:
            raise RecorderError("cumulative playback offset exceeds unsigned 64-bit range")
        try:
            offset_us = round(cumulative_us / speed)
        except OverflowError as error:
            raise RecorderError("scaled playback offset exceeds unsigned 64-bit range") from error
        if not 0 <= offset_us <= MAX_U64:
            raise RecorderError("scaled playback offset exceeds unsigned 64-bit range")
        expanded.append((offset_us, bytes(event.report)))
    return expanded


class PlaybackSession:
    """One prebuffered playback run with streaming queue refill."""

    def __init__(
        self,
        transport: PicoTransport,
        recording: Recording,
        *,
        speed: float = 1.0,
        prebuffer_ms: float = DEFAULT_PREBUFFER_MS,
        mode_timeout: float = 2.0,
        playback_timeout: float | None = None,
    ):
        if isinstance(prebuffer_ms, bool) or not isinstance(prebuffer_ms, (int, float)):
            raise RecorderError("prebuffer must be at least 500 ms")
        prebuffer_ms = float(prebuffer_ms)
        if not math.isfinite(prebuffer_ms) or prebuffer_ms < 500.0:
            raise RecorderError("prebuffer must be at least 500 ms")
        if (
            isinstance(mode_timeout, bool)
            or not isinstance(mode_timeout, (int, float))
            or not math.isfinite(mode_timeout)
            or mode_timeout <= 0
        ):
            raise RecorderError("mode timeout must be greater than zero")
        if playback_timeout is not None and (
            isinstance(playback_timeout, bool)
            or not isinstance(playback_timeout, (int, float))
            or not math.isfinite(playback_timeout)
            or playback_timeout <= 0
        ):
            raise RecorderError("playback timeout must be greater than zero")

        self.transport = transport
        self.recording = recording
        self.speed = float(speed)
        self.events = expand_offsets(recording.events, self.speed)
        self.prebuffer_us = round(prebuffer_ms * 1000.0)
        self.mode_timeout = float(mode_timeout)
        self.playback_timeout = (
            None if playback_timeout is None else float(playback_timeout)
        )

        self.started = False
        self.playing = False
        self.finished = False
        self._armed = False
        self._next_event = 0
        self._free_capacity = 0
        self._deadline = 0.0
        self.playback_start_us = 0
        self._underruns: list[UnderrunNotice] = []
        # Guards the fields below, which are written by the background
        # worker thread running start()/finish() but read by progress()
        # from a web request or WebSocket-push thread while playback is
        # in flight.
        self._progress_lock = threading.Lock()
        self._queued_count = 0
        self._reported_next_event = 0
        self._underrun_count = 0
        self._start_wallclock: float | None = None

    def start(self) -> int:
        """Enter ARMED, prebuffer at least 500 ms, and start the Pico epoch."""
        if self.started or self.finished:
            raise RecorderError("playback session has already started")

        self.transport.set_mode(MODE_ARMED, timeout=self.mode_timeout)
        self._armed = True
        self._deadline = self.transport.clock() + self._effective_playback_timeout()

        self.transport.send_queue_clear()
        state, queued_count, free_capacity = validate_buffer_status(
            self.transport.await_frame(BUFFER_STATUS, timeout=self._remaining())
        )
        if state != MODE_ARMED:
            raise ProtocolError(f"QUEUE_CLEAR acknowledged in unexpected state {state}")
        with self._progress_lock:
            self._queued_count = queued_count
            self._free_capacity = free_capacity

        prebuffer_count = self._prebuffer_count()
        if prebuffer_count > self._free_capacity:
            raise RecorderError(
                f"cannot prebuffer {self.prebuffer_us / 1000:g} ms within "
                f"the Pico's {self._free_capacity}-event free capacity"
            )
        self._send_through(prebuffer_count, expected_state=MODE_ARMED)

        if self._next_event == len(self.events):
            self.transport.send_queue_end()
            self.transport.await_frame(PLAY_READY, timeout=self._remaining())
            state, queued_count, free_capacity = validate_buffer_status(
                self.transport.await_frame(BUFFER_STATUS, timeout=self._remaining())
            )
            if state != MODE_ARMED:
                raise ProtocolError(f"QUEUE_END acknowledged in unexpected state {state}")
            with self._progress_lock:
                self._queued_count = queued_count
                self._free_capacity = free_capacity

        self.transport.send_play_start()
        state, reason = validate_mode_changed(
            self.transport.await_frame(MODE_CHANGED, timeout=self._remaining())
        )
        if state != MODE_PLAYING or reason != REASON_OK:
            raise ProtocolError(
                f"PLAY_START was not accepted: state={state}, reason={reason}"
            )
        self.playback_start_us = validate_play_started(
            self.transport.await_frame(PLAY_STARTED, timeout=self._remaining())
        )
        with self._progress_lock:
            self._start_wallclock = self.transport.clock()
        self.started = True
        self.playing = True
        return self.playback_start_us

    def finish(self, *, return_to_pass: bool = True) -> PlaybackResult:
        """Feed the remaining events, close the sequence, and await metrics."""
        if not self.started:
            raise RecorderError("playback session has not started")
        if self.finished:
            raise RecorderError("playback session has already finished")

        if self._next_event < len(self.events):
            self._send_through(len(self.events), expected_state=MODE_PLAYING)
            # PLAYING QUEUE_END has only BUFFER_STATUS; do not wait for
            # PLAY_READY, which is intentionally an ARMED-only handshake.
            self.transport.send_queue_end()

        result = self._await_outcome(
            PLAY_FINISHED, REASON_FINISHED, "finished"
        )
        self.playing = False
        self._armed = True
        if return_to_pass:
            self.transport.set_mode(MODE_PASS, timeout=self.mode_timeout)
            self._armed = False
        self.finished = True
        return result

    def play(self, *, return_to_pass: bool = True) -> PlaybackResult:
        self.start()
        return self.finish(return_to_pass=return_to_pass)

    def abort(self, *, return_to_pass: bool = True) -> PlaybackResult | None:
        """Abort a started run and consume its ordered outcome and metrics."""
        if self.finished:
            return None

        result: PlaybackResult | None = None
        outcome_error: Exception | None = None
        if self.playing:
            try:
                self.transport.send_play_abort()
                result = self._await_outcome(
                    PLAY_ABORTED, REASON_ABORTED, "aborted",
                    timeout=self.mode_timeout,
                )
            except Exception as error:
                outcome_error = error
            self.playing = False
            self._armed = True
        if return_to_pass and self._armed:
            # If the run ended just before PLAY_ABORT arrived, Pico answers
            # that command only with MODE_CHANGED(ARMED, ABORTED). Discard
            # such stale decoded frames before the independent PASS request.
            if outcome_error is not None:
                self.transport.drain_pending_frames()
            try:
                self.transport.set_mode(MODE_PASS, timeout=self.mode_timeout)
                self._armed = False
            except Exception as cleanup_error:
                if outcome_error is None:
                    outcome_error = cleanup_error
                else:
                    outcome_error = RecorderError(
                        f"{outcome_error}; additionally could not return Pico to PASS: "
                        f"{cleanup_error}"
                    )
        self.finished = True
        if outcome_error is not None:
            raise outcome_error
        return result

    def abort_best_effort(self) -> Exception | None:
        try:
            self.abort()
        except Exception as error:
            return error
        return None

    def _effective_playback_timeout(self) -> float:
        if self.playback_timeout is not None:
            return self.playback_timeout
        scaled_duration_seconds = (
            self.events[-1][0] / 1_000_000.0 if self.events else 0.0
        )
        return max(5.0, scaled_duration_seconds + self.mode_timeout + 3.0)

    def _remaining(self, timeout: float | None = None) -> float:
        if timeout is not None:
            return timeout
        remaining = self._deadline - self.transport.clock()
        if remaining <= 0:
            raise TransportTimeout("timed out during playback")
        return remaining

    def _prebuffer_count(self) -> int:
        for index, (offset_us, _) in enumerate(self.events):
            if offset_us >= self.prebuffer_us:
                return index + 1
        return len(self.events)

    def _send_through(self, stop: int, *, expected_state: int) -> None:
        while self._next_event < stop:
            if self._free_capacity == 0:
                self._free_capacity = self._await_credit(expected_state)
            batch_count = min(
                PIPELINE_WINDOW,
                self._free_capacity,
                stop - self._next_event,
            )
            for offset_us, report in self.events[
                self._next_event : self._next_event + batch_count
            ]:
                self.transport.send_queue_event(offset_us, report)

            # Every accepted event has one ordered BUFFER_STATUS ack. Consume
            # the whole batch before opening the next command window.
            for _ in range(batch_count):
                state, queued_count, free_capacity = validate_buffer_status(
                    self.transport.await_frame(BUFFER_STATUS, timeout=self._remaining())
                )
                if state != expected_state:
                    raise ProtocolError(
                        f"QUEUE_EVENT acknowledged in unexpected state {state}"
                    )
                self._free_capacity = free_capacity
                with self._progress_lock:
                    self._queued_count = queued_count
                    self._free_capacity = free_capacity
            self._next_event += batch_count
            with self._progress_lock:
                self._reported_next_event = self._next_event

    def _await_credit(self, expected_state: int) -> int:
        deferred: list[Frame] = []
        try:
            while True:
                frame = self.transport.require_not_error(
                    self.transport.receive_frame(timeout=self._remaining())
                )
                if frame.message_type == BUFFER_STATUS:
                    state, queued_count, free_capacity = validate_buffer_status(frame)
                    if state != expected_state:
                        raise ProtocolError(
                            f"BUFFER_STATUS has unexpected state {state}"
                        )
                    with self._progress_lock:
                        self._queued_count = queued_count
                    if free_capacity > 0:
                        return free_capacity
                    continue
                if frame.message_type == PLAY_UNDERRUN:
                    elapsed, free_capacity = validate_play_underrun(frame)
                    self._underruns.append(UnderrunNotice(elapsed, free_capacity))
                    with self._progress_lock:
                        self._underrun_count = len(self._underruns)
                    if free_capacity > 0:
                        return free_capacity
                    continue
                deferred.append(frame)
        finally:
            self.transport.preserve_frames(deferred)

    def _await_outcome(
        self,
        outcome_type: int,
        expected_reason: int,
        outcome: str,
        *,
        timeout: float | None = None,
    ) -> PlaybackResult:
        wait = self._remaining(timeout)
        self.transport.await_frame(outcome_type, timeout=wait)
        metrics = validate_play_metrics(
            self.transport.await_frame(PLAY_METRICS, timeout=self._remaining(timeout))
        )
        state, reason = validate_mode_changed(
            self.transport.await_frame(MODE_CHANGED, timeout=self._remaining(timeout))
        )
        if state != MODE_ARMED or reason != expected_reason:
            raise ProtocolError(
                f"playback ended with unexpected state={state}, reason={reason}"
            )
        self._collect_diagnostics()
        return PlaybackResult(
            outcome,
            self.playback_start_us,
            metrics,
            tuple(self._underruns),
        )

    def _collect_diagnostics(self) -> None:
        for frame in self.transport.drain_pending_frames():
            self.transport.require_not_error(frame)
            if frame.message_type == PLAY_UNDERRUN:
                elapsed, free_capacity = validate_play_underrun(frame)
                self._underruns.append(UnderrunNotice(elapsed, free_capacity))
                with self._progress_lock:
                    self._underrun_count = len(self._underruns)
            elif frame.message_type == BUFFER_STATUS:
                validate_buffer_status(frame)

    def progress(self) -> dict[str, object]:
        """Return a diagnostic-only snapshot for `Controller.status()`.

        `elapsed_us_estimate` is a Zero-side wall-clock approximation of
        playback position for a UI progress bar; it is never fed back into
        scheduling and does not touch Pico timing decisions, so it does not
        conflict with the Pico being the sole HID scheduler.
        """
        with self._progress_lock:
            queued_count = self._queued_count
            free_capacity = self._free_capacity
            queued_events = self._reported_next_event
            underrun_count = self._underrun_count
            start_wallclock = self._start_wallclock

        duration_us = self.events[-1][0] if self.events else 0
        if start_wallclock is None:
            elapsed_us_estimate = 0
        else:
            elapsed_us_estimate = max(
                0,
                min(
                    round((self.transport.clock() - start_wallclock) * 1_000_000.0),
                    duration_us,
                ),
            )

        return {
            "buffer": {"queued_count": queued_count, "free_capacity": free_capacity},
            "playback": {
                "queued_events": queued_events,
                "total_events": len(self.events),
                "underrun_count": underrun_count,
                "elapsed_us_estimate": elapsed_us_estimate,
                "duration_us": duration_us,
            },
        }
