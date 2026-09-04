"""Command line interface for recording and inspecting Pico captures."""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
from pathlib import Path
import sys
from typing import Sequence

from .errors import ModeRejected, RecorderError, StorageError, TransportTimeout
from .playback import PlaybackSession
from .recording import RecordingStore, validate_name
from .service import RecordingSession, stop_pico
from .transport import PicoTransport


DEFAULT_RECORDINGS_DIR = Path(__file__).resolve().parents[1] / "recordings"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="zero-recorder")
    parser.add_argument("--recordings-dir", type=Path, default=DEFAULT_RECORDINGS_DIR)
    subcommands = parser.add_subparsers(dest="command", required=True)

    record = subcommands.add_parser("record", help="record until Ctrl-C, then atomically save")
    record.add_argument("name")
    _add_serial_options(record)

    stop = subcommands.add_parser("stop", help="request the Pico's safe PASS mode")
    _add_serial_options(stop)

    play = subcommands.add_parser("play", help="prebuffer and stream one saved recording")
    play.add_argument("name")
    play.add_argument("--speed", type=float, default=1.0)
    play.add_argument("--prebuffer-ms", type=float, default=500.0)
    play.add_argument(
        "--playback-timeout",
        type=float,
        default=None,
        help="whole-run timeout in seconds (default: scaled duration plus margin)",
    )
    play.add_argument(
        "--metrics-out",
        type=Path,
        default=None,
        help="append this run's PLAY_METRICS (JSON Lines) to PATH, for tools/jitter_report.py",
    )
    _add_serial_options(play)

    subcommands.add_parser("list", help="list valid saved recordings")
    dump = subcommands.add_parser("dump", help="print one recording as JSON")
    dump.add_argument("name")
    return parser


def _add_serial_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", required=True, help="serial device, for example /dev/serial0")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--mode-timeout", type=float, default=2.0)


def _json_stdout(value: object) -> None:
    print(json.dumps(value, separators=(",", ":"), ensure_ascii=False, sort_keys=True))


def _json_error(error: Exception, code: int) -> int:
    print(json.dumps({"ok": False, "error": str(error), "code": code}, separators=(",", ":")), file=sys.stderr)
    return code


def _append_metrics_jsonl(path: Path | None, value: object) -> None:
    """Append one playback result as a JSON Line, for tools/jitter_report.py.

    A no-op when metrics_out was not requested or the run produced no result
    (e.g. an abort before PLAY_START).
    """
    if path is None or value is None:
        return
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(value, separators=(",", ":"), ensure_ascii=False, sort_keys=True))
        handle.write("\n")


def _open_transport(device: str, baud: int) -> tuple[PicoTransport, object]:
    try:
        import serial
    except ImportError as error:
        raise RecorderError("pyserial is required; install with: python3 -m pip install -r requirements.txt") from error
    try:
        stream = serial.Serial(device, baudrate=baud, timeout=0.1, write_timeout=1.0)
    except Exception as error:
        raise RecorderError(f"could not open serial device {device}: {error}") from error
    return PicoTransport(stream), stream


def _run_record(args: argparse.Namespace) -> int:
    name = validate_name(args.name)
    transport, stream = _open_transport(args.device, args.baud)
    session = RecordingSession(transport, RecordingStore(args.recordings_dir), name, mode_timeout=args.mode_timeout)
    completed = False
    try:
        session.start()
        while True:
            try:
                session.receive_and_consume(timeout=0.25)
            except TransportTimeout:
                # No input is normal while the user has not pressed a key.
                continue
    except KeyboardInterrupt:
        try:
            recording = session.stop()
        except Exception as error:
            return _json_error(error, 4)
        completed = True
        _json_stdout({"ok": True, "recording": recording.to_dict()})
        return 0
    except Exception as error:
        return _json_error(error, _exit_code(error))
    finally:
        if not completed:
            cleanup_error = session.abort()
            if cleanup_error is not None:
                print(json.dumps({"ok": False, "cleanup_error": str(cleanup_error)}), file=sys.stderr)
        stream.close()


def _run_stop(args: argparse.Namespace) -> int:
    transport, stream = _open_transport(args.device, args.baud)
    try:
        stop_pico(transport, mode_timeout=args.mode_timeout)
    except Exception as error:
        return _json_error(error, _exit_code(error))
    finally:
        stream.close()
    _json_stdout({"ok": True, "mode": "PASS"})
    return 0


def _run_play(args: argparse.Namespace) -> int:
    name = validate_name(args.name)
    recording = RecordingStore(args.recordings_dir).load(name)
    transport, stream = _open_transport(args.device, args.baud)
    session = None
    completed = False
    try:
        session = PlaybackSession(
            transport,
            recording,
            speed=args.speed,
            prebuffer_ms=args.prebuffer_ms,
            mode_timeout=args.mode_timeout,
            playback_timeout=args.playback_timeout,
        )
        result = session.play(return_to_pass=True)
        completed = True
        value = asdict(result)
        _append_metrics_jsonl(args.metrics_out, value)
        _json_stdout({"ok": True, "playback": value})
        return 0
    except KeyboardInterrupt:
        try:
            result = session.abort(return_to_pass=True)
        except Exception as error:
            return _json_error(error, 4)
        completed = True
        value = None if result is None else asdict(result)
        _append_metrics_jsonl(args.metrics_out, value)
        _json_stdout({"ok": True, "playback": value})
        return 0
    except Exception as error:
        return _json_error(error, _exit_code(error))
    finally:
        if not completed and session is not None:
            cleanup_error = session.abort_best_effort()
            if cleanup_error is not None:
                print(json.dumps({"ok": False, "cleanup_error": str(cleanup_error)}), file=sys.stderr)
        stream.close()


def _exit_code(error: Exception) -> int:
    if isinstance(error, StorageError):
        return 5
    if isinstance(error, (ModeRejected, TransportTimeout)):
        return 4
    return 3


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        store = RecordingStore(args.recordings_dir)
        if args.command == "list":
            _json_stdout({"recordings": store.list_metadata()})
            return 0
        if args.command == "dump":
            _json_stdout(store.load(args.name).to_dict())
            return 0
        if args.command == "record":
            return _run_record(args)
        if args.command == "play":
            return _run_play(args)
        if args.command == "stop":
            return _run_stop(args)
        raise AssertionError(f"unhandled command {args.command}")
    except Exception as error:
        return _json_error(error, _exit_code(error))


if __name__ == "__main__":
    raise SystemExit(main())
