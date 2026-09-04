import struct
import unittest

from app.errors import ProtocolError
from app.uart_protocol import (
    BUFFER_STATUS,
    MAGIC,
    MODE_ARMED,
    MODE_CHANGED,
    MODE_PASS,
    MODE_SET,
    PLAY_ABORT,
    PLAY_METRICS,
    PLAY_START,
    PLAY_UNDERRUN,
    QUEUE_CLEAR,
    QUEUE_END,
    QUEUE_EVENT,
    RECORD_EVENT,
    VERSION,
    Frame,
    IncrementalDecoder,
    crc16_ccitt_false,
    encode_frame,
    encode_play_abort,
    encode_play_start,
    encode_queue_clear,
    encode_queue_end,
    encode_queue_event,
    validate_buffer_status,
    validate_record_event,
    validate_play_metrics,
    validate_play_underrun,
)


def pico_frame(message_type, payload=b""):
    covered = bytes((VERSION, message_type)) + struct.pack("<H", len(payload)) + payload
    return bytes((MAGIC,)) + covered + struct.pack("<H", crc16_ccitt_false(covered))


class UartProtocolTests(unittest.TestCase):
    def test_crc16_ccitt_false_reference_vector(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_encode_mode_set(self):
        frame = encode_frame(MODE_SET, b"\x01")
        self.assertEqual(frame[:6], bytes((MAGIC, VERSION, MODE_SET, 1, 0, 1)))
        self.assertEqual(struct.unpack("<H", frame[-2:])[0], crc16_ccitt_false(frame[1:-2]))
        with self.assertRaises(ProtocolError):
            encode_frame(RECORD_EVENT)

    def test_decodes_byte_by_byte_and_multiple_frames(self):
        first = pico_frame(MODE_CHANGED, b"\x01\x00")
        second = pico_frame(RECORD_EVENT, struct.pack("<QB8s", 42, 8, b"\0" * 8))
        decoder = IncrementalDecoder()
        frames = []
        for byte in first + second:
            frames.extend(decoder.feed(bytes((byte,))))
        self.assertEqual(frames, [Frame(MODE_CHANGED, b"\x01\x00"), Frame(RECORD_EVENT, struct.pack("<QB8s", 42, 8, b"\0" * 8))])
        self.assertEqual(decoder.pop_errors(), [])

    def test_bad_frames_are_reported_and_next_magic_resynchronises(self):
        corrupt = bytearray(pico_frame(MODE_CHANGED, b"\x00\x00"))
        corrupt[-1] ^= 0xFF
        valid = pico_frame(MODE_CHANGED, b"\x01\x00")
        decoder = IncrementalDecoder()
        frames = decoder.feed(bytes(corrupt) + valid)
        self.assertEqual(frames, [Frame(MODE_CHANGED, b"\x01\x00")])
        self.assertIn("CRC", decoder.pop_errors()[0])

    def test_rejects_version_length_and_direction(self):
        decoder = IncrementalDecoder(max_payload=16)
        wrong_version = bytes((MAGIC, 1, MODE_CHANGED, 0, 0, 0, 0))
        oversized = bytes((MAGIC, VERSION, MODE_CHANGED, 17, 0))
        wrong_direction = bytes((MAGIC, VERSION, MODE_SET, 0, 0, 0, 0))
        decoder.feed(wrong_version + oversized + wrong_direction)
        errors = decoder.pop_errors()
        self.assertTrue(any("version" in error for error in errors))
        self.assertTrue(any("payload length" in error for error in errors))
        self.assertTrue(any("message type" in error for error in errors))

    def test_record_event_requires_exact_payload_and_report_length(self):
        with self.assertRaises(ProtocolError):
            validate_record_event(Frame(RECORD_EVENT, b"\0" * 16))
        payload = struct.pack("<QB8s", 7, 7, b"\0" * 8)
        with self.assertRaises(ProtocolError):
            validate_record_event(Frame(RECORD_EVENT, payload))
        self.assertEqual(validate_record_event(Frame(RECORD_EVENT, struct.pack("<QB8s", 7, 8, bytes(range(8))))), (7, tuple(range(8))))

    def test_rejects_invalid_pico_message_payload_shape(self):
        decoder = IncrementalDecoder()
        decoder.feed(pico_frame(MODE_CHANGED, b"\x01"))
        self.assertIn("invalid payload", decoder.pop_errors()[0])

    def test_encode_queue_clear_and_end_have_no_payload(self):
        for encoder, message_type in ((encode_queue_clear, QUEUE_CLEAR), (encode_queue_end, QUEUE_END)):
            frame = encoder()
            self.assertEqual(frame[:5], bytes((MAGIC, VERSION, message_type, 0, 0)))
            self.assertEqual(len(frame), 7)
            self.assertEqual(struct.unpack("<H", frame[-2:])[0], crc16_ccitt_false(frame[1:-2]))

    def test_encode_queue_event_matches_record_event_wire_shape(self):
        report = bytes(range(8))
        frame = encode_queue_event(0x1122334455667788, report)
        self.assertEqual(frame[:3], bytes((MAGIC, VERSION, QUEUE_EVENT)))
        self.assertEqual(struct.unpack("<H", frame[3:5])[0], 17)
        payload = frame[5:22]
        self.assertEqual(payload, struct.pack("<QB8s", 0x1122334455667788, 8, report))
        with self.assertRaises(ProtocolError):
            encode_queue_event(0, b"\0" * 7)

    def test_encode_queue_event_rejects_out_of_range_offset(self):
        report = bytes(range(8))
        with self.assertRaisesRegex(ProtocolError, "offset_us"):
            encode_queue_event(-1, report)
        with self.assertRaisesRegex(ProtocolError, "offset_us"):
            encode_queue_event(2**64, report)
        with self.assertRaisesRegex(ProtocolError, "offset_us"):
            encode_queue_event(1.5, report)

    def test_validate_buffer_status_round_trip_and_rejections(self):
        payload = struct.pack("<BHH", MODE_ARMED, 3, 509)
        self.assertEqual(validate_buffer_status(Frame(BUFFER_STATUS, payload)), (MODE_ARMED, 3, 509))
        with self.assertRaises(ProtocolError):
            validate_buffer_status(Frame(MODE_CHANGED, payload))
        with self.assertRaises(ProtocolError):
            validate_buffer_status(Frame(BUFFER_STATUS, payload[:-1]))
        with self.assertRaises(ProtocolError):
            validate_buffer_status(Frame(BUFFER_STATUS, struct.pack("<BHH", 99, 0, 0)))

    def test_encode_play_commands_have_no_payload(self):
        for encoder, message_type in (
            (encode_play_start, PLAY_START),
            (encode_play_abort, PLAY_ABORT),
        ):
            frame = encoder()
            self.assertEqual(frame[:5], bytes((MAGIC, VERSION, message_type, 0, 0)))
            self.assertEqual(len(frame), 7)

    def test_validate_play_underrun_and_metrics(self):
        underrun = Frame(PLAY_UNDERRUN, struct.pack("<QH", 123_456, 512))
        self.assertEqual(validate_play_underrun(underrun), (123_456, 512))
        with self.assertRaises(ProtocolError):
            validate_play_underrun(Frame(PLAY_UNDERRUN, b"\0" * 9))

        payload = struct.pack("<IIiiqiiB", 10, 2, 1, 9, 42, 8, 9, 1)
        metrics = validate_play_metrics(Frame(PLAY_METRICS, payload))
        self.assertEqual(metrics.dispatched_count, 10)
        self.assertEqual(metrics.underrun_count, 2)
        self.assertTrue(metrics.samples_truncated)
        with self.assertRaisesRegex(ProtocolError, "zero or one"):
            validate_play_metrics(
                Frame(PLAY_METRICS, struct.pack("<IIiiqiiB", 0, 0, 0, 0, 0, 0, 0, 2))
            )


if __name__ == "__main__":
    unittest.main()
