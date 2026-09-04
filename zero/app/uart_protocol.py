"""Version-2 UART framing shared by the Pico and Pi Zero.

The decoder is deliberately incremental: callers may pass one byte at a time
or arbitrary serial chunks.  Bad candidate frames discard only their magic
byte, allowing the next valid magic marker to resynchronise the stream.
"""

from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterable

from .errors import ProtocolError, RecordingValidationError


MAGIC = 0xA5
VERSION = 0x02
# pico/include/uart_protocol.h fixes the v2 maximum at 64 bytes.
MAX_PAYLOAD = 64

RECORD_EVENT = 0x01
PICO_STATUS = 0x02
BUFFER_STATUS = 0x03
PLAY_READY = 0x04
PLAY_STARTED = 0x05
PLAY_FINISHED = 0x06
PLAY_ABORTED = 0x07
PLAY_UNDERRUN = 0x08
ERROR = 0x09
PONG = 0x0A
MODE_CHANGED = 0x0B
PLAY_METRICS = 0x0C

QUEUE_CLEAR = 0x80
QUEUE_EVENT = 0x81
QUEUE_END = 0x82
PLAY_START = 0x83
PLAY_ABORT = 0x84
STATUS_REQUEST = 0x85
PING = 0x86
MODE_SET = 0x87

PICO_TO_ZERO_TYPES = frozenset(range(RECORD_EVENT, PLAY_METRICS + 1))
ZERO_TO_PICO_TYPES = frozenset(range(0x80, MODE_SET + 1))

MODE_PASS = 0
MODE_RECORD = 1
MODE_ARMED = 2
MODE_PLAYING = 3
MODE_ERROR = 4
VALID_MODES = frozenset((MODE_PASS, MODE_RECORD, MODE_ARMED, MODE_PLAYING, MODE_ERROR))

REASON_OK = 0
REASON_ABORTED = 5
REASON_UNDERRUN = 6
REASON_FINISHED = 7
VALID_REASONS = frozenset(range(0, 8))


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    """Return CRC-16/CCITT-FALSE (poly 0x1021, initial value 0xffff)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True)
class Frame:
    message_type: int
    payload: bytes


@dataclass(frozen=True)
class PlayMetrics:
    dispatched_count: int
    underrun_count: int
    min_lateness_us: int
    max_lateness_us: int
    sum_lateness_us: int
    p95_lateness_us: int
    p99_lateness_us: int
    samples_truncated: bool


def encode_frame(message_type: int, payload: bytes = b"") -> bytes:
    """Encode a frame after validating that it travels Zero -> Pico."""
    if message_type not in ZERO_TO_PICO_TYPES:
        raise ProtocolError(f"0x{message_type:02x} is not a Zero-to-Pico message type")
    if not isinstance(payload, bytes):
        raise ProtocolError("frame payload must be bytes")
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"frame payload exceeds {MAX_PAYLOAD} bytes")
    covered = bytes((VERSION, message_type)) + struct.pack("<H", len(payload)) + payload
    return bytes((MAGIC,)) + covered + struct.pack("<H", crc16_ccitt_false(covered))


def encode_queue_clear() -> bytes:
    """Encode QUEUE_CLEAR, discarding the Pico playback queue. Valid in ARMED."""
    return encode_frame(QUEUE_CLEAR)


def encode_queue_event(offset_us: int, report: bytes) -> bytes:
    """Encode one QUEUE_EVENT: an absolute Pico-epoch offset plus its report.

    Mirrors RECORD_EVENT's wire shape (offset_us u64 LE, report_len u8 (8),
    then the report) but travels Zero -> Pico.
    """
    if type(offset_us) is not int or not 0 <= offset_us <= 0xFFFFFFFFFFFFFFFF:
        raise ProtocolError("QUEUE_EVENT offset_us must be an integer in 0..2**64-1")
    if not isinstance(report, (bytes, bytearray)) or len(report) != 8:
        raise ProtocolError("QUEUE_EVENT report must be exactly 8 bytes")
    payload = struct.pack("<QB", offset_us, 8) + bytes(report)
    return encode_frame(QUEUE_EVENT, payload)


def encode_queue_end() -> bytes:
    """Encode QUEUE_END, marking the loaded/streamed sequence complete."""
    return encode_frame(QUEUE_END)


def encode_play_start() -> bytes:
    """Encode PLAY_START. Valid after prebuffering in ARMED."""
    return encode_frame(PLAY_START)


def encode_play_abort() -> bytes:
    """Encode PLAY_ABORT. Valid in ARMED or PLAYING."""
    return encode_frame(PLAY_ABORT)


class IncrementalDecoder:
    """Decode and validate Pico -> Zero frames from a byte stream."""

    def __init__(self, *, max_payload: int = MAX_PAYLOAD, accepted_types: Iterable[int] = PICO_TO_ZERO_TYPES):
        self.max_payload = max_payload
        self.accepted_types = frozenset(accepted_types)
        self._buffer = bytearray()
        self._errors: list[str] = []

    def pop_errors(self) -> list[str]:
        errors, self._errors = self._errors, []
        return errors

    def feed(self, data: bytes | bytearray | memoryview) -> list[Frame]:
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise TypeError("decoder input must be bytes-like")
        self._buffer.extend(data)
        frames: list[Frame] = []

        while True:
            magic_at = self._buffer.find(bytes((MAGIC,)))
            if magic_at < 0:
                # Retain no bytes: MAGIC is a single byte and cannot straddle chunks.
                self._buffer.clear()
                break
            if magic_at:
                del self._buffer[:magic_at]
            if len(self._buffer) < 5:
                break

            version = self._buffer[1]
            message_type = self._buffer[2]
            payload_len = self._buffer[3] | (self._buffer[4] << 8)
            if version != VERSION:
                self._reject(f"unsupported UART version {version}")
                continue
            if message_type not in self.accepted_types:
                self._reject(f"unexpected Pico-to-Zero message type 0x{message_type:02x}")
                continue
            if payload_len > self.max_payload:
                self._reject(f"payload length {payload_len} exceeds {self.max_payload}")
                continue

            frame_len = 7 + payload_len
            if len(self._buffer) < frame_len:
                break
            covered = bytes(self._buffer[1 : 5 + payload_len])
            received_crc = self._buffer[5 + payload_len] | (self._buffer[6 + payload_len] << 8)
            expected_crc = crc16_ccitt_false(covered)
            if received_crc != expected_crc:
                self._reject("CRC16-CCITT-FALSE mismatch")
                continue
            payload = bytes(self._buffer[5 : 5 + payload_len])
            if not pico_payload_is_valid(message_type, payload):
                self._reject(f"invalid payload for Pico-to-Zero message type 0x{message_type:02x}")
                continue
            frames.append(Frame(message_type, payload))
            del self._buffer[:frame_len]
        return frames

    def _reject(self, detail: str) -> None:
        self._errors.append(detail)
        del self._buffer[0]


def validate_record_event(frame: Frame) -> tuple[int, tuple[int, ...]]:
    """Validate and unpack a Pico RECORD_EVENT payload."""
    if frame.message_type != RECORD_EVENT:
        raise ProtocolError("expected RECORD_EVENT")
    if len(frame.payload) != 17:
        raise ProtocolError(f"RECORD_EVENT payload must be 17 bytes, got {len(frame.payload)}")
    timestamp_us, report_len = struct.unpack_from("<QB", frame.payload)
    if report_len != 8:
        raise ProtocolError(f"RECORD_EVENT report_len must be 8, got {report_len}")
    report = tuple(frame.payload[9:])
    try:
        validate_boot_report(report)
    except RecordingValidationError as error:
        raise ProtocolError(str(error)) from error
    return timestamp_us, report


def validate_mode_changed(frame: Frame) -> tuple[int, int]:
    if frame.message_type != MODE_CHANGED:
        raise ProtocolError("expected MODE_CHANGED")
    if len(frame.payload) != 2:
        raise ProtocolError(f"MODE_CHANGED payload must be 2 bytes, got {len(frame.payload)}")
    state, reason = frame.payload
    if state not in VALID_MODES:
        raise ProtocolError(f"MODE_CHANGED has invalid state {state}")
    if reason not in VALID_REASONS:
        raise ProtocolError(f"MODE_CHANGED has invalid reason {reason}")
    return state, reason


def validate_pico_status(frame: Frame) -> tuple[int, tuple[bool, bool, bool, bool]]:
    """Validate PICO_STATUS and expose its RX/hardware/frame/TX fault flags."""
    if frame.message_type != PICO_STATUS:
        raise ProtocolError("expected PICO_STATUS")
    if len(frame.payload) != 5:
        raise ProtocolError(f"PICO_STATUS payload must be 5 bytes, got {len(frame.payload)}")
    state = frame.payload[0]
    if state not in VALID_MODES:
        raise ProtocolError(f"PICO_STATUS has invalid state {state}")
    flags = frame.payload[1:]
    if any(flag not in (0, 1) for flag in flags):
        raise ProtocolError("PICO_STATUS flags must be zero or one")
    return state, tuple(bool(flag) for flag in flags)  # type: ignore[return-value]


def validate_buffer_status(frame: Frame) -> tuple[int, int, int]:
    """Validate BUFFER_STATUS and expose (state, queued_count, free_capacity)."""
    if frame.message_type != BUFFER_STATUS:
        raise ProtocolError("expected BUFFER_STATUS")
    if len(frame.payload) != 5:
        raise ProtocolError(f"BUFFER_STATUS payload must be 5 bytes, got {len(frame.payload)}")
    state, queued_count, free_capacity = struct.unpack("<BHH", frame.payload)
    if state not in VALID_MODES:
        raise ProtocolError(f"BUFFER_STATUS has invalid state {state}")
    return state, queued_count, free_capacity


def validate_play_started(frame: Frame) -> int:
    """Validate PLAY_STARTED and return the Pico playback epoch."""
    if frame.message_type != PLAY_STARTED:
        raise ProtocolError("expected PLAY_STARTED")
    if len(frame.payload) != 8:
        raise ProtocolError(f"PLAY_STARTED payload must be 8 bytes, got {len(frame.payload)}")
    return struct.unpack("<Q", frame.payload)[0]


def validate_play_underrun(frame: Frame) -> tuple[int, int]:
    """Return (elapsed_offset_us, free_capacity) for PLAY_UNDERRUN."""
    if frame.message_type != PLAY_UNDERRUN:
        raise ProtocolError("expected PLAY_UNDERRUN")
    if len(frame.payload) != 10:
        raise ProtocolError(f"PLAY_UNDERRUN payload must be 10 bytes, got {len(frame.payload)}")
    return struct.unpack("<QH", frame.payload)


def validate_play_metrics(frame: Frame) -> PlayMetrics:
    """Validate and unpack one end-of-run PLAY_METRICS frame."""
    if frame.message_type != PLAY_METRICS:
        raise ProtocolError("expected PLAY_METRICS")
    if len(frame.payload) != 33:
        raise ProtocolError(f"PLAY_METRICS payload must be 33 bytes, got {len(frame.payload)}")
    values = struct.unpack("<IIiiqiiB", frame.payload)
    if values[-1] not in (0, 1):
        raise ProtocolError("PLAY_METRICS samples_truncated must be zero or one")
    return PlayMetrics(*values[:-1], bool(values[-1]))


def pico_payload_is_valid(message_type: int, payload: bytes) -> bool:
    """Mirror the fixed v2 Pico-to-Zero payload lengths before delivery."""
    if message_type == RECORD_EVENT:
        return len(payload) == 17 and payload[8] == 8
    if message_type in (PICO_STATUS, BUFFER_STATUS):
        return len(payload) == 5
    if message_type == PLAY_STARTED:
        return len(payload) == 8
    if message_type == PLAY_UNDERRUN:
        return len(payload) == 10
    if message_type == MODE_CHANGED:
        return len(payload) == 2
    if message_type == PLAY_METRICS:
        # dispatched_count u32, underrun_count u32, min/max_lateness_us i32,
        # sum_lateness_us i64, p95/p99_lateness_us i32, samples_truncated u8.
        return len(payload) == 33
    return len(payload) == 0


def validate_boot_report(report: Iterable[int]) -> tuple[int, ...]:
    """Validate the exact eight unsigned bytes of a Boot Keyboard report."""
    values = tuple(report)
    if len(values) != 8:
        raise RecordingValidationError("Boot Keyboard report must contain exactly 8 bytes")
    if any(type(value) is not int or not 0 <= value <= 0xFF for value in values):
        raise RecordingValidationError("Boot Keyboard report bytes must be integers in 0..255")
    return values
