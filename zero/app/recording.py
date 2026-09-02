"""Pico-timestamp recording model and atomic JSON v1 persistence."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Iterable

from .errors import RecordingValidationError, StorageError
from .uart_protocol import validate_boot_report


FORMAT_VERSION = 1
MAX_U64 = (1 << 64) - 1
NAME_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")


def validate_name(name: object) -> str:
    if not isinstance(name, str) or not NAME_PATTERN.fullmatch(name):
        raise RecordingValidationError(
            "recording name must be 1-64 ASCII letters, digits, '.', '_' or '-', starting with a letter or digit"
        )
    return name


def _validate_u64(value: object, field: str) -> int:
    if type(value) is not int or not 0 <= value <= MAX_U64:
        raise RecordingValidationError(f"{field} must be an unsigned 64-bit integer")
    return value


@dataclass(frozen=True)
class RecordingEvent:
    dt_us: int
    report: tuple[int, ...]

    def __post_init__(self) -> None:
        _validate_u64(self.dt_us, "dt_us")
        object.__setattr__(self, "report", validate_boot_report(self.report))

    def to_dict(self) -> dict[str, object]:
        return {"dt_us": self.dt_us, "report": list(self.report)}


@dataclass(frozen=True)
class Recording:
    name: str
    duration_us: int
    events: tuple[RecordingEvent, ...]
    version: int = FORMAT_VERSION

    def __post_init__(self) -> None:
        validate_name(self.name)
        if type(self.version) is not int or self.version != FORMAT_VERSION:
            raise RecordingValidationError(f"unsupported recording version {self.version}")
        _validate_u64(self.duration_us, "duration_us")
        events = tuple(self.events)
        if events and events[0].dt_us != 0:
            raise RecordingValidationError("the first event dt_us must be zero")
        if sum(event.dt_us for event in events) != self.duration_us:
            raise RecordingValidationError("duration_us must equal the sum of event dt_us values")
        object.__setattr__(self, "events", events)

    def to_dict(self) -> dict[str, object]:
        return {
            "version": FORMAT_VERSION,
            "name": self.name,
            "duration_us": self.duration_us,
            "events": [event.to_dict() for event in self.events],
        }


class RecordingBuilder:
    """Convert monotonic Pico capture timestamps into persisted deltas."""

    def __init__(self, name: str):
        self.name = validate_name(name)
        self._epoch: int | None = None
        self._last_timestamp: int | None = None
        self._events: list[RecordingEvent] = []

    def add(self, timestamp_us: int, report: Iterable[int]) -> None:
        timestamp_us = _validate_u64(timestamp_us, "timestamp_us")
        validated_report = validate_boot_report(report)
        if self._last_timestamp is not None and timestamp_us < self._last_timestamp:
            raise RecordingValidationError("Pico timestamp moved backwards")
        if self._epoch is None:
            self._epoch = timestamp_us
            dt_us = 0
        else:
            assert self._last_timestamp is not None
            dt_us = timestamp_us - self._last_timestamp
        self._last_timestamp = timestamp_us
        self._events.append(RecordingEvent(dt_us, validated_report))

    def build(self) -> Recording:
        duration_us = 0 if self._epoch is None or self._last_timestamp is None else self._last_timestamp - self._epoch
        return Recording(self.name, duration_us, tuple(self._events))


def parse_recording(value: Any, *, expected_name: str | None = None) -> Recording:
    if not isinstance(value, dict) or set(value) != {"version", "name", "duration_us", "events"}:
        raise RecordingValidationError("recording JSON must contain exactly version, name, duration_us, and events")
    name = validate_name(value["name"])
    if expected_name is not None and name != validate_name(expected_name):
        raise RecordingValidationError("recording name does not match its filename")
    if type(value["version"]) is not int or value["version"] != FORMAT_VERSION:
        raise RecordingValidationError(f"unsupported recording version {value['version']!r}")
    if not isinstance(value["events"], list):
        raise RecordingValidationError("events must be an array")
    events: list[RecordingEvent] = []
    for index, event in enumerate(value["events"]):
        if not isinstance(event, dict) or set(event) != {"dt_us", "report"}:
            raise RecordingValidationError(f"event {index} must contain exactly dt_us and report")
        if not isinstance(event["report"], list):
            raise RecordingValidationError(f"event {index} report must be an array")
        events.append(RecordingEvent(event["dt_us"], tuple(event["report"])))
    return Recording(name, value["duration_us"], tuple(events), value["version"])


class RecordingStore:
    """A directory of JSON v1 recordings, published with atomic replacement."""

    def __init__(self, directory: str | Path):
        self.directory = Path(directory)

    def path_for(self, name: str) -> Path:
        return self.directory / f"{validate_name(name)}.json"

    def exists(self, name: str) -> bool:
        return self.path_for(name).exists()

    def save(self, recording: Recording) -> Path:
        path = self.path_for(recording.name)
        if path.exists():
            raise StorageError(f"recording already exists: {recording.name}")
        try:
            self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
            file_descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{recording.name}-", suffix=".tmp", dir=self.directory
            )
            temporary_path = Path(temporary_name)
            try:
                with os.fdopen(file_descriptor, "w", encoding="utf-8") as handle:
                    json.dump(recording.to_dict(), handle, separators=(",", ":"), ensure_ascii=False)
                    handle.write("\n")
                    handle.flush()
                    os.fsync(handle.fileno())
                # The temporary file is created in self.directory, so replace is same-filesystem.
                os.replace(temporary_path, path)
            except Exception:
                temporary_path.unlink(missing_ok=True)
                raise
        except StorageError:
            raise
        except OSError as error:
            raise StorageError(f"could not atomically save recording {recording.name}: {error}") from error
        return path

    def load(self, name: str) -> Recording:
        path = self.path_for(name)
        try:
            with path.open(encoding="utf-8") as handle:
                value = json.load(handle)
        except FileNotFoundError as error:
            raise StorageError(f"recording not found: {name}") from error
        except (OSError, json.JSONDecodeError) as error:
            raise StorageError(f"invalid recording file {path.name}: {error}") from error
        try:
            return parse_recording(value, expected_name=name)
        except RecordingValidationError as error:
            raise StorageError(f"invalid recording file {path.name}: {error}") from error

    def list_metadata(self) -> list[dict[str, object]]:
        try:
            paths = sorted(self.directory.glob("*.json"), key=lambda candidate: candidate.name)
        except OSError as error:
            raise StorageError(f"could not list recordings: {error}") from error
        metadata: list[dict[str, object]] = []
        for path in paths:
            try:
                recording = self.load(path.stem)
            except RecordingValidationError as error:
                raise StorageError(f"invalid recording filename {path.name}: {error}") from error
            metadata.append(
                {
                    "name": recording.name,
                    "duration_us": recording.duration_us,
                    "event_count": len(recording.events),
                }
            )
        return metadata
