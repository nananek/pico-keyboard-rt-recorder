"""Aggregate PLAY_METRICS across playback runs for jitter analysis.

Reads one or more JSON Lines files produced by
`zero-recorder play ... --metrics-out PATH` (see `zero/app/cli.py`), each
line a JSON object with a `metrics` field shaped like the Pico's
`PLAY_METRICS` frame (`docs/protocol.md`), and prints per-run and
across-run min/max/mean for the fields that matter for real-time
acceptance: p95/p99/max lateness, underrun count, and dispatched count.

Standard library only -- no new dependency on top of zero/requirements.txt.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import statistics
import sys
from typing import NamedTuple, Sequence

FIELDS = (
    "dispatched_count",
    "underrun_count",
    "max_lateness_us",
    "p95_lateness_us",
    "p99_lateness_us",
)


class Run(NamedTuple):
    source: str
    line_number: int
    metrics: dict[str, float]


def load_runs(paths: Sequence[Path]) -> list[Run]:
    """Parse every JSONL line across `paths` into a `Run`.

    Raises ValueError (naming the offending file/line) on malformed input,
    rather than silently skipping it -- a jitter report over partial data
    would be misleading.
    """
    runs: list[Run] = []
    for path in paths:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                line = line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {error}") from error
                metrics = entry.get("metrics")
                if not isinstance(metrics, dict):
                    raise ValueError(f"{path}:{line_number}: missing or invalid 'metrics' object")
                runs.append(Run(str(path), line_number, metrics))
    return runs


def _stats(values: Sequence[float]) -> dict[str, float]:
    return {
        "min": min(values),
        "max": max(values),
        "mean": statistics.fmean(values),
    }


def aggregate(runs: Sequence[Run]) -> dict[str, dict[str, float]]:
    """Across-run min/max/mean for each field in FIELDS."""
    if not runs:
        raise ValueError("no runs to aggregate")
    return {
        field: _stats([run.metrics.get(field, 0) for run in runs])
        for field in FIELDS
    }


def format_text(runs: Sequence[Run], summary: dict[str, dict[str, float]]) -> str:
    lines = [f"{len(runs)} run(s):"]
    for run in runs:
        values = ", ".join(f"{field}={run.metrics.get(field, 0)}" for field in FIELDS)
        lines.append(f"  {run.source}:{run.line_number}  {values}")
    lines.append("")
    lines.append("across-run summary (min / mean / max):")
    for field in FIELDS:
        stat = summary[field]
        lines.append(f"  {field:>18}: {stat['min']:g} / {stat['mean']:g} / {stat['max']:g}")
    return "\n".join(lines)


def format_csv(runs: Sequence[Run]) -> str:
    header = ["source", "line"] + list(FIELDS)
    rows = [",".join(header)]
    for run in runs:
        row = [run.source, str(run.line_number)] + [
            str(run.metrics.get(field, 0)) for field in FIELDS
        ]
        rows.append(",".join(row))
    return "\n".join(rows)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Aggregate lateness/underrun jitter across PLAY_METRICS JSONL runs."
    )
    parser.add_argument("metrics_files", nargs="+", type=Path, help="JSONL files from --metrics-out")
    parser.add_argument("--format", choices=("text", "csv"), default="text")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        runs = load_runs(args.metrics_files)
        summary = aggregate(runs)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if args.format == "csv":
        print(format_csv(runs))
    else:
        print(format_text(runs, summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
