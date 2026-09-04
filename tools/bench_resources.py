"""Manual CPU/disk resource benchmark for a Zero record or playback run.

NOT part of the automated test suite (not run by `unittest discover`, not
imported by any test) and not meant for CI: it requires a real Pico 2
connected over a live serial device, per the "Zero recorder acceptance"
hardware steps in docs/testing.md. Run it by hand during hardware
acceptance to get a rough CPU-time and disk-usage delta around one
`zero-recorder record`/`play` invocation, for Issue #10's CPU/disk
reliability scope item.

Usage:
    python3 tools/bench_resources.py --device /dev/serial0 -- record demo
    python3 tools/bench_resources.py --device /dev/serial0 -- play demo
"""

from __future__ import annotations

import argparse
from pathlib import Path
import resource
import shutil
import sys
import time
from typing import Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "zero"))

from app import cli  # noqa: E402  (after the sys.path fix-up above)


def _cpu_times() -> tuple[float, float]:
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return usage.ru_utime, usage.ru_stime


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="serial device, for example /dev/serial0")
    parser.add_argument(
        "--recordings-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "zero" / "recordings",
    )
    parser.add_argument(
        "zero_args",
        nargs=argparse.REMAINDER,
        help="the zero-recorder subcommand and its arguments, e.g. 'record demo'",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.zero_args:
        print("error: pass a zero-recorder subcommand after --, e.g. -- record demo", file=sys.stderr)
        return 2
    zero_args = list(args.zero_args)
    if zero_args[0] == "--":
        zero_args = zero_args[1:]

    disk_before = shutil.disk_usage(args.recordings_dir.parent if not args.recordings_dir.exists() else args.recordings_dir)
    user_before, sys_before = _cpu_times()
    wall_before = time.monotonic()

    status = cli.main(
        ["--recordings-dir", str(args.recordings_dir), *zero_args, "--device", args.device]
    )

    wall_after = time.monotonic()
    user_after, sys_after = _cpu_times()
    disk_after = shutil.disk_usage(args.recordings_dir)

    print(f"exit status: {status}")
    print(f"wall time:   {wall_after - wall_before:.3f} s")
    print(f"cpu user:    {user_after - user_before:.3f} s")
    print(f"cpu system:  {sys_after - sys_before:.3f} s")
    print(f"disk used delta: {disk_after.used - disk_before.used} bytes")
    print(f"disk free after: {disk_after.free} bytes")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
