"""Serial transport and mode handshake for the Pico UART v2 protocol."""

from __future__ import annotations

from collections import deque
import time
from typing import Protocol

from .errors import ModeRejected, PicoError, ProtocolError, TransportTimeout
from .uart_protocol import (
    ERROR,
    MODE_CHANGED,
    MODE_ERROR,
    PICO_STATUS,
    REASON_OK,
    Frame,
    IncrementalDecoder,
    encode_frame,
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

    def set_mode(self, target_mode: int, *, timeout: float) -> None:
        if target_mode not in (0, 1, 2):
            raise ProtocolError(f"MODE_SET target must be PASS, RECORD, or ARMED; got {target_mode}")
        self._write(encode_frame(MODE_SET, bytes((target_mode,))))
        deadline = self.clock() + timeout
        deferred: list[Frame] = []
        try:
            while True:
                remaining = deadline - self.clock()
                if remaining <= 0:
                    raise TransportTimeout(f"timed out waiting for MODE_CHANGED({target_mode})")
                frame = self.require_not_error(self.receive_frame(timeout=remaining))
                if frame.message_type != MODE_CHANGED:
                    # Events can precede the acknowledgement in the same serial read.
                    # Retain them for the recording session without repeatedly reading
                    # the same queued frame while awaiting MODE_CHANGED.
                    deferred.append(frame)
                    continue
                state, reason = validate_mode_changed(frame)
                if state == target_mode and reason == REASON_OK:
                    return
                raise ModeRejected(f"Pico rejected MODE_SET({target_mode}): state={state}, reason={reason}")
        finally:
            self._pending.extendleft(reversed(deferred))

    def receive_frame(self, *, timeout: float) -> Frame:
        if self._pending:
            return self._pending.popleft()
        deadline = self.clock() + timeout
        while True:
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
            raise PicoError("Pico reported ERROR during recording")
        if frame.message_type == PICO_STATUS:
            state, faults = validate_pico_status(frame)
            if state == MODE_ERROR or any(faults):
                labels = ("rx_overflow", "hardware_error", "invalid_frame", "tx_dropped")
                details = [label for label, active in zip(labels, faults) if active]
                if state == MODE_ERROR:
                    details.insert(0, "error_state")
                raise PicoError(f"Pico status reported {', '.join(details)}")
        return frame
