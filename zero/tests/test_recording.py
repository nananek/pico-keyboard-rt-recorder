import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.errors import RecordingValidationError, StorageError
from app.recording import RecordingBuilder, RecordingStore, parse_recording, validate_name


class RecordingTests(unittest.TestCase):
    def test_pico_timestamps_become_deltas_and_keep_duplicate_release(self):
        builder = RecordingBuilder("hello")
        release = (0,) * 8
        press = (2, 0, 4, 0, 0, 0, 0, 0)
        builder.add(100, press)
        builder.add(130, press)
        builder.add(180, release)
        recording = builder.build()
        self.assertEqual(recording.duration_us, 80)
        self.assertEqual([event.dt_us for event in recording.events], [0, 30, 50])
        self.assertEqual(recording.events[1].report, press)
        self.assertEqual(recording.events[2].report, release)

    def test_backwards_timestamp_is_rejected(self):
        builder = RecordingBuilder("hello")
        builder.add(10, (0,) * 8)
        with self.assertRaisesRegex(RecordingValidationError, "backwards"):
            builder.add(9, (0,) * 8)

    def test_json_round_trip_and_schema_validation(self):
        builder = RecordingBuilder("hello")
        builder.add(123, (0,) * 8)
        builder.add(456, (1,) * 8)
        original = builder.build()
        self.assertEqual(parse_recording(original.to_dict()), original)
        bad = original.to_dict()
        bad["events"][0]["dt_us"] = 1
        with self.assertRaisesRegex(RecordingValidationError, "first event"):
            parse_recording(bad)

    def test_name_allowlist_rejects_paths(self):
        for name in ("", ".hidden", "../escape", "has/slash", "a b", "a" * 65):
            with self.subTest(name=name):
                with self.assertRaises(RecordingValidationError):
                    validate_name(name)
        self.assertEqual(validate_name("good-name_1.2"), "good-name_1.2")

    def test_store_does_not_overwrite_and_lists_stably(self):
        with tempfile.TemporaryDirectory() as directory:
            store = RecordingStore(directory)
            for name in ("zeta", "alpha"):
                builder = RecordingBuilder(name)
                builder.add(1, (0,) * 8)
                store.save(builder.build())
            with self.assertRaisesRegex(StorageError, "already exists"):
                store.save(RecordingBuilder("alpha").build())
            self.assertEqual([item["name"] for item in store.list_metadata()], ["alpha", "zeta"])
            self.assertEqual(store.load("alpha").to_dict()["events"][0]["dt_us"], 0)

    def test_atomic_publication_failure_never_publishes_partial_file(self):
        with tempfile.TemporaryDirectory() as directory:
            store = RecordingStore(directory)
            builder = RecordingBuilder("hello")
            builder.add(1, (0,) * 8)
            with patch("app.recording.os.link", side_effect=OSError("disk failed")):
                with self.assertRaisesRegex(StorageError, "could not atomically save"):
                    store.save(builder.build())
            self.assertFalse((Path(directory) / "hello.json").exists())
            self.assertEqual(list(Path(directory).glob("*.tmp")), [])

    def test_atomic_publication_does_not_clobber_an_existing_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            store = RecordingStore(directory)
            winner = RecordingBuilder("hello")
            winner.add(1, (0,) * 8)
            winning_recording = winner.build()
            store.save(winning_recording)

            contender = RecordingBuilder("hello")
            contender.add(2, (1,) * 8)
            with self.assertRaisesRegex(StorageError, "already exists"):
                store.save(contender.build())

            self.assertEqual(store.load("hello"), winning_recording)
            self.assertEqual(list(Path(directory).glob("*.tmp")), [])

    def test_invalid_file_is_an_identifiable_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hello.json"
            path.write_text("{not json", encoding="utf-8")
            with self.assertRaisesRegex(StorageError, "invalid recording file"):
                RecordingStore(directory).load("hello")


if __name__ == "__main__":
    unittest.main()
