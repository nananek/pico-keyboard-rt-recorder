"""Recording lifecycle that always attempts a safe return to PASS."""

from __future__ import annotations

from .errors import ProtocolError, RecorderError, StorageError, TransportTimeout
from .recording import Recording, RecordingBuilder, RecordingStore, validate_name
from .transport import PicoTransport
from .uart_protocol import MODE_CHANGED, MODE_PASS, MODE_RECORD, RECORD_EVENT, validate_record_event


class RecordingSession:
    """One RECORD session; it publishes only after a confirmed PASS transition."""

    def __init__(self, transport: PicoTransport, store: RecordingStore, name: str, *, mode_timeout: float = 2.0):
        self.transport = transport
        self.store = store
        self.name = validate_name(name)
        self.mode_timeout = mode_timeout
        self.builder = RecordingBuilder(name)
        self.started = False
        self.finished = False

    def start(self) -> None:
        if self.store.exists(self.name):
            raise StorageError(f"recording already exists: {self.name}")
        self.transport.set_mode(MODE_RECORD, timeout=self.mode_timeout)
        self.started = True

    def receive_and_consume(self, *, timeout: float) -> None:
        frame = self.transport.require_not_error(self.transport.receive_frame(timeout=timeout))
        self.consume(frame)

    def consume(self, frame) -> None:
        if frame.message_type == RECORD_EVENT:
            timestamp_us, report = validate_record_event(frame)
            self.builder.add(timestamp_us, report)
        elif frame.message_type == MODE_CHANGED:
            raise ProtocolError("Pico mode changed unexpectedly while recording")
        # Other valid Pico status frames are diagnostic and cannot become events.

    def stop(self) -> Recording:
        if self.finished:
            raise RecorderError("recording session has already finished")
        try:
            self.transport.set_mode(MODE_PASS, timeout=self.mode_timeout)
        except RecorderError:
            self.finished = True
            raise
        self.finished = True
        recording = self.builder.build()
        self.store.save(recording)
        return recording

    def abort(self) -> Exception | None:
        """Best-effort safety cleanup; no partial recording is published."""
        if self.finished:
            return None
        self.finished = True
        try:
            self.transport.set_mode(MODE_PASS, timeout=self.mode_timeout)
        except Exception as error:  # Cleanup failure is reported beside the original failure.
            return error
        return None


def stop_pico(transport: PicoTransport, *, mode_timeout: float = 2.0) -> None:
    """Implement the standalone `stop` command."""
    transport.set_mode(MODE_PASS, timeout=mode_timeout)
