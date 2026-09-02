import struct
import unittest

from app.errors import ProtocolError
from app.uart_protocol import (
    MAGIC,
    MODE_CHANGED,
    MODE_PASS,
    MODE_SET,
    RECORD_EVENT,
    VERSION,
    Frame,
    IncrementalDecoder,
    crc16_ccitt_false,
    encode_frame,
    validate_record_event,
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


if __name__ == "__main__":
    unittest.main()
