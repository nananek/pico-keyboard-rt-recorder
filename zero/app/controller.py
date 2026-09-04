"""Single-owner controller mediating web requests against one PicoTransport.

At most one recording or playback session is active at a time. Starting a
session runs its blocking mode handshake synchronously (matching the CLI),
then hands the long-running receive loop to a dedicated background thread so
web requests never touch the transport while a session owns it. Stopping a
recording is naturally responsive because its loop already re-polls every
`record_poll_interval` seconds; stopping a playback mid-stream instead uses
`PicoTransport.cancel_event` to interrupt whatever wait it is blocked in.
"""

from __future__ import annotations

from dataclasses import asdict
import threading

from .errors import RecorderError, StorageError, TransportTimeout
from .playback import PlaybackSession
from .recording import RecordingStore
from .service import RecordingSession
from .transport import PicoTransport
from .uart_protocol import MODE_PASS


STATE_PASS = "PASS"
STATE_RECORD = "RECORD"
STATE_ARMED = "ARMED"
STATE_PLAYING = "PLAYING"
STATE_ERROR = "ERROR"

DEFAULT_STOP_TIMEOUT = 10.0


class Controller:
    """Owns one PicoTransport and the at-most-one active recording/playback."""

    def __init__(
        self,
        transport: PicoTransport,
        store: RecordingStore,
        *,
        mode_timeout: float = 2.0,
        record_poll_interval: float = 0.25,
    ):
        self.transport = transport
        self.store = store
        self.mode_timeout = mode_timeout
        self.record_poll_interval = record_poll_interval

        self._meta_lock = threading.Lock()
        self._state = STATE_PASS
        self._active_kind: str | None = None  # None | "record" | "playback" | "idle"
        self._active_name: str | None = None
        self._session: RecordingSession | PlaybackSession | None = None
        self._worker: threading.Thread | None = None
        self._stop_event: threading.Event | None = None
        self._last_error: Exception | None = None
        self._last_result: dict[str, object] | None = None
        # Bumped by every _begin(); lets _request_stop detect that the
        # session it captured a worker/stop_event for has already finished
        # on its own (e.g. a spontaneous ProtocolError) *and* a new session
        # has since started, so self._last_error/_last_result now belong to
        # that new session rather than the one being stopped.
        self._generation = 0

    # -- status -----------------------------------------------------------

    def status(self) -> dict[str, object]:
        with self._meta_lock:
            return {
                "state": self._state,
                "active": self._active_kind if self._active_kind != "idle" else None,
                "name": self._active_name,
                "last_error": None if self._last_error is None else str(self._last_error),
                "last_result": self._last_result,
            }

    # -- recording ----------------------------------------------------------

    def start_recording(self, name: str) -> dict[str, object]:
        self._begin("record", name)
        session: RecordingSession | None = None
        try:
            session = RecordingSession(self.transport, self.store, name, mode_timeout=self.mode_timeout)
            # Reflect intent before the blocking handshake, same as
            # start_playback does for ARMED: a concurrent status() poll
            # during the handshake should see RECORD, not a stale PASS.
            with self._meta_lock:
                self._session = session
                self._state = STATE_RECORD
            session.start()
        except Exception:
            # Mirror the CLI: always attempt the idempotent MODE_SET(PASS)
            # safety net, even when the RECORD handshake itself failed.
            cleanup_error = None if session is None else session.abort()
            self._end(state=STATE_ERROR if cleanup_error is not None else STATE_PASS)
            raise
        stop_event = threading.Event()
        with self._meta_lock:
            self._session = session
            self._stop_event = stop_event
            self._state = STATE_RECORD
            worker = threading.Thread(target=self._run_recording, args=(session, stop_event), daemon=True)
            self._worker = worker
        # Snapshot the response before starting the worker: it may finish
        # arbitrarily fast (in tests, immediately) and must not be able to
        # overwrite this state before it is read.
        response = self.status()
        worker.start()
        return response

    def stop_recording(self, *, timeout: float = DEFAULT_STOP_TIMEOUT) -> dict[str, object]:
        error, result = self._request_stop("record", timeout)
        if error is not None:
            raise error
        return result

    def _run_recording(self, session: RecordingSession, stop_event: threading.Event) -> None:
        error: Exception | None = None
        try:
            while not stop_event.is_set():
                try:
                    session.receive_and_consume(timeout=self.record_poll_interval)
                except TransportTimeout:
                    continue
        except Exception as caught:
            error = caught

        if error is None:
            try:
                recording = session.stop()
            except Exception as caught:
                error = caught
            else:
                self._end(state=STATE_PASS, result=recording.to_dict())
                return

        cleanup_error = session.abort()
        self._end(state=STATE_ERROR if cleanup_error is not None else STATE_PASS, error=error)

    # -- playback -------------------------------------------------------

    def start_playback(
        self,
        name: str,
        *,
        speed: float = 1.0,
        prebuffer_ms: float = 500.0,
        playback_timeout: float | None = None,
    ) -> dict[str, object]:
        recording = self.store.load(name)
        self._begin("playback", name)
        session: PlaybackSession | None = None
        try:
            session = PlaybackSession(
                self.transport,
                recording,
                speed=speed,
                prebuffer_ms=prebuffer_ms,
                mode_timeout=self.mode_timeout,
                playback_timeout=playback_timeout,
            )
            with self._meta_lock:
                self._session = session
                self._state = STATE_ARMED
            session.start()
        except Exception:
            # Mirror the CLI: always attempt best-effort cleanup, even when
            # the ARMED handshake itself failed.
            cleanup_error = None if session is None else session.abort_best_effort()
            self._end(state=STATE_ERROR if cleanup_error is not None else STATE_PASS)
            raise
        stop_event = threading.Event()
        with self._meta_lock:
            self._stop_event = stop_event
            self._state = STATE_PLAYING
            worker = threading.Thread(target=self._run_playback, args=(session, stop_event), daemon=True)
            self._worker = worker
        # Snapshot the response before starting the worker: it may finish
        # arbitrarily fast (in tests, immediately) and must not be able to
        # overwrite this state before it is read.
        response = self.status()
        worker.start()
        return response

    def stop_playback(self, *, timeout: float = DEFAULT_STOP_TIMEOUT) -> dict[str, object]:
        error, result = self._request_stop("playback", timeout)
        if error is not None:
            raise error
        return result

    def _run_playback(self, session: PlaybackSession, stop_event: threading.Event) -> None:
        self.transport.cancel_event = stop_event
        try:
            result = session.finish(return_to_pass=True)
        except TransportTimeout as error:
            self.transport.cancel_event = None
            if not stop_event.is_set():
                # A genuine playback_timeout expiry, not a stop request:
                # still attempt best-effort cleanup so the Pico is not left
                # ARMED/PLAYING with a key possibly held (mirrors the CLI's
                # `finally: session.abort_best_effort()`).
                cleanup_error = session.abort_best_effort()
                self._end(state=STATE_ERROR if cleanup_error is not None else STATE_PASS, error=error)
                return
            try:
                aborted = session.abort(return_to_pass=True)
            except Exception as abort_error:
                self._end(state=STATE_ERROR, error=abort_error)
                return
            self._end(state=STATE_PASS, result=None if aborted is None else asdict(aborted))
            return
        except Exception as error:
            self.transport.cancel_event = None
            cleanup_error = session.abort_best_effort()
            self._end(state=STATE_ERROR if cleanup_error is not None else STATE_PASS, error=error)
            return
        self.transport.cancel_event = None
        self._end(state=STATE_PASS, result=asdict(result))

    # -- recording management -------------------------------------------

    def rename_recording(self, old: str, new: str) -> None:
        # Held for the whole check-then-act: otherwise a start_recording/
        # start_playback for `old` could slip in between the active check
        # and the file operation.
        with self._meta_lock:
            if self._active_kind in ("record", "playback") and self._active_name == old:
                raise StorageError(f"cannot rename recording while it is active: {old}")
            self.store.rename(old, new)

    def delete_recording(self, name: str) -> None:
        with self._meta_lock:
            if self._active_kind in ("record", "playback") and self._active_name == name:
                raise StorageError(f"cannot delete recording while it is active: {name}")
            self.store.delete(name)

    # -- shutdown / safety -------------------------------------------------

    def safe_stop(self, *, timeout: float = DEFAULT_STOP_TIMEOUT) -> dict[str, object]:
        """Abort whatever is active, then idempotently reconcile to PASS.

        Used both for the general-purpose stop endpoint and for service
        startup/shutdown, covering a Zero restart while the Pico is still
        ARMED/RECORD/PLAYING.
        """
        with self._meta_lock:
            kind = self._active_kind
        if kind == "record":
            try:
                self.stop_recording(timeout=timeout)
            except RecorderError:
                pass
            return self.status()
        if kind == "playback":
            try:
                self.stop_playback(timeout=timeout)
            except RecorderError:
                pass
            return self.status()

        with self._meta_lock:
            if self._active_kind is not None:
                # Lost a race with a session that just started; leave it be.
                return self.status()
            self._active_kind = "idle"
        try:
            self.transport.set_mode(MODE_PASS, timeout=self.mode_timeout)
        except Exception as error:
            self._end(state=STATE_ERROR, error=error)
        else:
            self._end(state=STATE_PASS)
        return self.status()

    # -- shared bookkeeping -------------------------------------------------

    def _begin(self, kind: str, name: str) -> None:
        with self._meta_lock:
            if self._active_kind is not None:
                raise RecorderError("a recording or playback session is already active")
            self._active_kind = kind
            self._active_name = name
            self._last_error = None
            self._last_result = None
            self._generation += 1

    def _end(
        self,
        *,
        state: str,
        error: Exception | None = None,
        result: dict[str, object] | None = None,
    ) -> None:
        with self._meta_lock:
            self._active_kind = None
            self._active_name = None
            self._session = None
            self._worker = None
            self._stop_event = None
            self._state = state
            self._last_error = error
            self._last_result = result

    def _request_stop(self, kind: str, timeout: float) -> tuple[Exception | None, dict[str, object] | None]:
        with self._meta_lock:
            if self._active_kind != kind:
                raise RecorderError(f"no {kind} session is active")
            stop_event = self._stop_event
            worker = self._worker
            generation = self._generation
        if stop_event is None or worker is None:
            # _begin() has claimed active_kind but the session's synchronous
            # mode handshake (between _begin() and the worker thread being
            # created) hasn't finished yet: there is nothing to signal.
            raise RecorderError(f"{kind} session is still starting; try again")
        stop_event.set()
        worker.join(timeout=timeout)
        if worker.is_alive():
            raise RecorderError(f"{kind} session did not stop within {timeout:g}s")
        with self._meta_lock:
            if self._generation != generation:
                # The session finished on its own (e.g. a spontaneous
                # ProtocolError) and a new session has since started, all
                # while this call was blocked between reading stop_event/
                # worker above and joining it. self._last_error/_last_result
                # now belong to that new session, not the one we meant to
                # stop -- returning them would silently misreport it.
                raise RecorderError(
                    f"{kind} session ended on its own before it could be stopped; check status"
                )
            return self._last_error, self._last_result
