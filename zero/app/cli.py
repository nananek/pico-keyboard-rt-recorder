"""Command line interface for recording and inspecting Pico captures."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Sequence

from .errors import ModeRejected, RecorderError, StorageError, TransportTimeout
from .recording import RecordingStore
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

    subcommands.add_parser("list", help="list valid saved recordings")
    dump = subcommands.add_parser("dump", help="print one recording as JSON")
    dump.add_argument("name")
    return parser


def _add_serial_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", required=True, help="serial device, for example /dev/serial0")
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--mode-timeout", type=float, default=2.0)


def _json_stdout(value: object) -> None:
    print(json.dumps(value, separators=(",", ":"), ensure_ascii=False, sort_keys=True))


def _json_error(error: Exception, code: int) -> int:
    print(json.dumps({"ok": False, "error": str(error), "code": code}, separators=(",", ":")), file=sys.stderr)
    return code


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
    transport, stream = _open_transport(args.device, args.baud)
    session = RecordingSession(transport, RecordingStore(args.recordings_dir), args.name, mode_timeout=args.mode_timeout)
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
        if args.command == "stop":
            return _run_stop(args)
        raise AssertionError(f"unhandled command {args.command}")
    except Exception as error:
        return _json_error(error, _exit_code(error))


if __name__ == "__main__":
    raise SystemExit(main())
