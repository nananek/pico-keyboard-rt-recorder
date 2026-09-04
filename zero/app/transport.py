"""Serial transport and mode handshake for the Pico UART v2 protocol."""

from __future__ import annotations

from collections import deque
import time
from typing import Iterable, Protocol

from .errors import ModeRejected, PicoError, ProtocolError, TransportTimeout
from .uart_protocol import (
    BUFFER_STATUS,
    ERROR,
    MODE_CHANGED,
    MODE_ERROR,
    PICO_STATUS,
    PLAY_READY,
    REASON_OK,
    Frame,
    IncrementalDecoder,
    encode_frame,
    encode_play_abort,
    encode_play_start,
    encode_queue_clear,
    encode_queue_end,
    encode_queue_event,
    validate_buffer_status,
    validate_mode_changed,
    validate_pico_status,
    MODE_SET,
)


class ByteStream(Protocol):
    def read(self, size: int = 1) -> bytes: ...

    def write(self, data: bytes) -> int | None: ...

    def close(self) -> None: ...


class PicoTransport:
    """Validated Pico UART I/O with idempotent MODE_SET acknowledgement waits."""

    def __init__(self, stream: ByteStream, *, clock=time.monotonic, read_size: int = 256):
        self.stream = stream
        self.clock = clock
        self.read_size = read_size
        self.decoder = IncrementalDecoder()
        self._pending: deque[Frame] = deque()
        # Optional cooperative-cancellation hook: a controller running a
        # session's blocking receive loop on a background thread sets this to
        # abort a long wait (e.g. mid-playback) without another thread
        # touching the stream. None (the default) never affects behavior.
        self.cancel_event = None

    def set_mode(self, target_mode: int, *, timeout: float, retries: int = 1) -> None:
        if target_mode not in (0, 1, 2):
            raise ProtocolError(f"MODE_SET target must be PASS, RECORD, or ARMED; got {target_mode}")
        if retries < 0:
            raise ProtocolError("MODE_SET retries must not be negative")
        for attempt in range(retries + 1):
            self._write(encode_frame(MODE_SET, bytes((target_mode,))))
            try:
                frame = self._await(MODE_CHANGED, timeout=timeout)
            except TransportTimeout:
                if attempt == retries:
                    raise
                continue
            state, reason = validate_mode_changed(frame)
            if state == target_mode and reason == REASON_OK:
                return
            raise ModeRejected(f"Pico rejected MODE_SET({target_mode}): state={state}, reason={reason}")

    def queue_events(self, events: Iterable[tuple[int, bytes]], *, timeout: float) -> None:
        """Load the Pico playback queue, respecting Pico-advertised capacity.

        Sends QUEUE_CLEAR, then one QUEUE_EVENT per (offset_us, report) pair
        while staying within the free_capacity most recently reported by
        BUFFER_STATUS, then QUEUE_END and waits for PLAY_READY followed by
        the BUFFER_STATUS Pico always sends right after it -- both are
        consumed here so neither leaks into a later call's pending frames.
        Valid only while Pico is ARMED. This does not prebuffer or stream
        events during PLAYING -- that is the Zero playback feeder (a later
        phase). timeout bounds the whole call, not each individual wait.
        """
        events = tuple(events)
        deadline = self.clock() + timeout

        def remaining() -> float:
            left = deadline - self.clock()
            if left <= 0:
                raise TransportTimeout("timed out loading the Pico playback queue")
            return left

        self._write(encode_queue_clear())
        _, _, free_capacity = validate_buffer_status(self._await(BUFFER_STATUS, timeout=remaining()))
        for offset_us, report in events:
            if free_capacity <= 0:
                raise ProtocolError("Pico playback queue is full; cannot queue more events")
            self._write(encode_queue_event(offset_us, report))
            _, _, free_capacity = validate_buffer_status(self._await(BUFFER_STATUS, timeout=remaining()))
        self._write(encode_queue_end())
        self._await(PLAY_READY, timeout=remaining())
        validate_buffer_status(self._await(BUFFER_STATUS, timeout=remaining()))

    def send_queue_clear(self) -> None:
        self._write(encode_queue_clear())

    def send_queue_event(self, offset_us: int, report: bytes) -> None:
        self._write(encode_queue_event(offset_us, report))

    def send_queue_end(self) -> None:
        self._write(encode_queue_end())

    def send_play_start(self) -> None:
        self._write(encode_play_start())

    def send_play_abort(self) -> None:
        self._write(encode_play_abort())

    def await_frame(self, message_type: int, *, timeout: float) -> Frame:
        """Wait for one message type while preserving interleaved frames."""
        return self._await(message_type, timeout=timeout)

    def preserve_frames(self, frames: Iterable[Frame]) -> None:
        """Put already-consumed frames back at the front in wire order."""
        self._pending.extendleft(reversed(tuple(frames)))

    def _await(self, message_type: int, *, timeout: float) -> Frame:
        """Wait for a specific message type, deferring any other frames.

        Used by both set_mode and queue_events: frames of a different type
        (e.g. a RECORD_EVENT interleaved with a queue-loading exchange) are
        retained in arrival order for later callers instead of being
        discarded.
        """
        deadline = self.clock() + timeout
        deferred: list[Frame] = []
        try:
            while True:
                remaining = deadline - self.clock()
                if remaining <= 0:
                    raise TransportTimeout(f"timed out waiting for message 0x{message_type:02x}")
                frame = self.require_not_error(self.receive_frame(timeout=remaining))
                if frame.message_type != message_type:
                    deferred.append(frame)
                    continue
                return frame
        finally:
            self._pending.extendleft(reversed(deferred))

    def receive_frame(self, *, timeout: float) -> Frame:
        if self._pending:
            return self._pending.popleft()
        deadline = self.clock() + timeout
        while True:
            if self.cancel_event is not None and self.cancel_event.is_set():
                raise TransportTimeout("cancelled")
            try:
                data = self.stream.read(self.read_size)
            except OSError as error:
                raise ProtocolError(f"could not read from Pico UART: {error}") from error
            if data:
                frames = self.decoder.feed(data)
                errors = self.decoder.pop_errors()
                if errors:
                    raise ProtocolError(f"invalid UART frame: {errors[0]}")
                self._pending.extend(frames)
                if self._pending:
                    return self._pending.popleft()
            if self.clock() >= deadline:
                raise TransportTimeout("timed out waiting for Pico UART data")

    def drain_pending_frames(self) -> list[Frame]:
        """Return frames already read from UART, preserving wire order.

        A mode acknowledgement can share a serial read with RECORD_EVENT
        frames that preceded it.  Callers completing a recording consume
        those frames before publishing the result.
        """
        frames = list(self._pending)
        self._pending.clear()
        return frames

    def _write(self, data: bytes) -> None:
        try:
            written = self.stream.write(data)
        except OSError as error:
            raise ProtocolError(f"could not write to Pico UART: {error}") from error
        if written is not None and written != len(data):
            raise ProtocolError(f"short Pico UART write: {written} of {len(data)} bytes")

    @staticmethod
    def require_not_error(frame: Frame) -> Frame:
        if frame.message_type == ERROR:
            raise PicoError("Pico reported ERROR")
        if frame.message_type == PICO_STATUS:
            state, faults = validate_pico_status(frame)
            if state == MODE_ERROR or any(faults):
                labels = ("rx_overflow", "hardware_error", "invalid_frame", "tx_dropped")
                details = [label for label, active in zip(labels, faults) if active]
                if state == MODE_ERROR:
                    details.insert(0, "error_state")
                raise PicoError(f"Pico status reported {', '.join(details)}")
        if frame.message_type == MODE_CHANGED:
            state, reason = validate_mode_changed(frame)
            if state == MODE_ERROR:
                raise PicoError(f"Pico entered ERROR state: reason={reason}")
        return frame
