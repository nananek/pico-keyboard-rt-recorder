import json
from pathlib import Path
import sys
import tempfile
import unittest

# Mirrors commit 85ae445's fix-up (see the other files in this directory):
# `python3 -m unittest discover -s zero/tests` does not add anything to
# sys.path, so both zero/ (for `app`) and the repo root (for the sibling
# `tools/` package this module tests) need inserting explicitly.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.jitter_report import aggregate, format_csv, format_text, load_runs


def _write_jsonl(directory: Path, name: str, entries: list[dict]) -> Path:
    path = directory / name
    with path.open("w", encoding="utf-8") as handle:
        for entry in entries:
            handle.write(json.dumps(entry))
            handle.write("\n")
    return path


def metrics(**overrides) -> dict:
    base = {
        "dispatched_count": 100,
        "underrun_count": 0,
        "max_lateness_us": 200,
        "p95_lateness_us": 150,
        "p99_lateness_us": 190,
    }
    base.update(overrides)
    return base


class JitterReportTests(unittest.TestCase):
    def test_load_runs_parses_metrics_field_across_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            first = _write_jsonl(
                path, "a.jsonl", [{"outcome": "finished", "metrics": metrics(max_lateness_us=100)}]
            )
            second = _write_jsonl(
                path, "b.jsonl", [{"outcome": "finished", "metrics": metrics(max_lateness_us=300)}]
            )

            runs = load_runs([first, second])

        self.assertEqual(len(runs), 2)
        self.assertEqual(runs[0].metrics["max_lateness_us"], 100)
        self.assertEqual(runs[1].metrics["max_lateness_us"], 300)

    def test_load_runs_skips_blank_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "a.jsonl"
            path.write_text(
                json.dumps({"metrics": metrics()}) + "\n\n" + json.dumps({"metrics": metrics()}) + "\n",
                encoding="utf-8",
            )
            runs = load_runs([path])
        self.assertEqual(len(runs), 2)

    def test_load_runs_rejects_malformed_json_with_location(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.jsonl"
            path.write_text("not json\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, r"bad\.jsonl:1"):
                load_runs([path])

    def test_load_runs_rejects_missing_metrics_field(self):
        with tempfile.TemporaryDirectory() as directory:
            path = _write_jsonl(Path(directory), "a.jsonl", [{"outcome": "finished"}])
            with self.assertRaisesRegex(ValueError, "metrics"):
                load_runs([path])

    def test_aggregate_computes_min_max_mean_across_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = _write_jsonl(
                Path(directory),
                "a.jsonl",
                [
                    {"metrics": metrics(p95_lateness_us=100, p99_lateness_us=120)},
                    {"metrics": metrics(p95_lateness_us=300, p99_lateness_us=320)},
                ],
            )
            runs = load_runs([path])
            summary = aggregate(runs)

        self.assertEqual(summary["p95_lateness_us"], {"min": 100, "max": 300, "mean": 200})
        self.assertEqual(summary["p99_lateness_us"], {"min": 120, "max": 320, "mean": 220})

    def test_aggregate_rejects_empty_run_list(self):
        with self.assertRaisesRegex(ValueError, "no runs"):
            aggregate([])

    def test_format_text_and_csv_include_every_run(self):
        with tempfile.TemporaryDirectory() as directory:
            path = _write_jsonl(
                Path(directory), "a.jsonl", [{"metrics": metrics()}, {"metrics": metrics()}]
            )
            runs = load_runs([path])
            summary = aggregate(runs)

        text = format_text(runs, summary)
        self.assertIn("2 run(s):", text)
        self.assertIn("p95_lateness_us", text)

        csv_text = format_csv(runs)
        self.assertEqual(len(csv_text.splitlines()), 3)  # header + 2 runs
        self.assertIn("dispatched_count", csv_text.splitlines()[0])


if __name__ == "__main__":
    unittest.main()
