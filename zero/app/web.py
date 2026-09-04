"""FastAPI web API exposing recording/playback control and lifecycle safety.

Translates HTTP requests into `Controller` calls; error mapping mirrors
`cli.py`'s `_exit_code`: `StorageError` -> 404/409, `ModeRejected`/
`TransportTimeout` -> 409, `ProtocolError`/`PicoError` -> 502, an unexpected
active-session conflict or other `RecorderError` -> 409, anything else -> 500.
The FastAPI lifespan opens the one persistent `PicoTransport`, reconciles the
Pico to PASS on startup, and safely stops any active session and reconciles
to PASS again on shutdown.
"""

from __future__ import annotations

from contextlib import asynccontextmanager
import os
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from .controller import Controller
from .errors import (
    ModeRejected,
    PicoError,
    ProtocolError,
    RecorderError,
    RecordingValidationError,
    StorageError,
    TransportTimeout,
)
from .recording import RecordingStore
from .transport import PicoTransport


DEFAULT_RECORDINGS_DIR = Path(__file__).resolve().parents[1] / "recordings"
DEFAULT_DEVICE = os.environ.get("ZERO_SERIAL_DEVICE", "/dev/serial0")
DEFAULT_BAUD = int(os.environ.get("ZERO_SERIAL_BAUD", "460800"))
DEFAULT_MODE_TIMEOUT = float(os.environ.get("ZERO_MODE_TIMEOUT", "2.0"))


class PlayRequest(BaseModel):
    speed: float = 1.0
    prebuffer_ms: float = 500.0
    playback_timeout: float | None = None


class RenameRequest(BaseModel):
    new_name: str


def _http_status(error: Exception) -> int:
    if isinstance(error, RecordingValidationError):
        return 400
    if isinstance(error, StorageError):
        return 404 if "not found" in str(error) else 409
    if isinstance(error, (ModeRejected, TransportTimeout)):
        return 409
    if isinstance(error, (ProtocolError, PicoError)):
        return 502
    if isinstance(error, RecorderError):
        return 409
    return 500


def create_app(
    *,
    device: str = DEFAULT_DEVICE,
    baud: int = DEFAULT_BAUD,
    recordings_dir: Path = DEFAULT_RECORDINGS_DIR,
    mode_timeout: float = DEFAULT_MODE_TIMEOUT,
    transport: PicoTransport | None = None,
    record_poll_interval: float = 0.25,
) -> FastAPI:
    """Build the FastAPI app.

    `transport` is overridable so tests can inject a `PicoTransport` wrapping
    a fake byte stream instead of opening a real serial device.
    """

    store = RecordingStore(recordings_dir)
    owns_stream = transport is None

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        nonlocal transport
        stream = None
        if transport is None:
            import serial

            stream = serial.Serial(device, baudrate=baud, timeout=0.1, write_timeout=1.0)
            transport = PicoTransport(stream)
        controller = Controller(
            transport, store, mode_timeout=mode_timeout, record_poll_interval=record_poll_interval
        )
        app.state.controller = controller
        # Reconcile to PASS at startup: a prior process may have exited
        # while the Pico was still ARMED/RECORD/PLAYING.
        controller.safe_stop()
        try:
            yield
        finally:
            controller.safe_stop()
            if owns_stream and stream is not None:
                stream.close()

    app = FastAPI(title="Pico Keyboard Recorder", lifespan=lifespan)

    def get_controller() -> Controller:
        return app.state.controller

    @app.exception_handler(RecorderError)
    async def recorder_error_handler(request: Request, exc: RecorderError) -> JSONResponse:
        return JSONResponse(status_code=_http_status(exc), content={"ok": False, "error": str(exc)})

    @app.get("/api/status")
    def status() -> dict[str, object]:
        return {"ok": True, **get_controller().status()}

    @app.get("/api/recordings")
    def list_recordings() -> dict[str, object]:
        return {"ok": True, "recordings": store.list_metadata()}

    @app.get("/api/recordings/{name}/download")
    def download(name: str) -> dict[str, object]:
        return store.load(name).to_dict()

    @app.post("/api/recordings/{name}/record")
    def start_record(name: str) -> dict[str, object]:
        return {"ok": True, **get_controller().start_recording(name)}

    @app.post("/api/record/stop")
    def stop_record() -> dict[str, object]:
        return {"ok": True, "recording": get_controller().stop_recording()}

    @app.post("/api/recordings/{name}/play")
    def start_play(name: str, body: PlayRequest) -> dict[str, object]:
        return {
            "ok": True,
            **get_controller().start_playback(
                name,
                speed=body.speed,
                prebuffer_ms=body.prebuffer_ms,
                playback_timeout=body.playback_timeout,
            ),
        }

    @app.post("/api/playback/stop")
    def stop_play() -> dict[str, object]:
        return {"ok": True, "playback": get_controller().stop_playback()}

    @app.post("/api/stop")
    def stop_all() -> dict[str, object]:
        return {"ok": True, **get_controller().safe_stop()}

    @app.post("/api/recordings/{name}/rename")
    def rename(name: str, body: RenameRequest) -> dict[str, object]:
        get_controller().rename_recording(name, body.new_name)
        return {"ok": True}

    @app.delete("/api/recordings/{name}")
    def delete(name: str) -> dict[str, object]:
        get_controller().delete_recording(name)
        return {"ok": True}

    return app


app = create_app()
