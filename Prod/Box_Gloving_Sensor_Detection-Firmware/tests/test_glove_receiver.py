"""Protocol v2 receiver fixtures, including MTU-23 fragmentation behavior."""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from glove_receiver import (  # noqa: E402
    CAP_CAPTURES,
    CAP_EVENTS,
    CAP_STATS,
    CAPTURE_ASSEMBLY_TIMEOUT_NS,
    CAPTURE_HEADER_SIZE,
    CAPTURE_RECORD_SIZE,
    MAGIC,
    MSG_CAPTURE_DATA,
    MSG_CAPTURE_INFO,
    MSG_EVENT,
    MSG_HELLO_ACK,
    MSG_STATS,
    ProtocolError,
    STATS_CHUNK_COUNT,
    STATS_CHUNK_VALUES,
    STATS_VALUE_COUNT,
    GloveV2Receiver,
    capture_blob_size,
)

VECTOR_PATH = Path(__file__).resolve().parent / "fixtures" / "v2_capture_vector.hex"


def frame(message_type: int, session: int, sequence: int, payload: bytes) -> bytes:
    return struct.pack("<BBHH", MAGIC, message_type, session, sequence) + payload


def hello_ack(session: int, sequence: int, capabilities: int = CAP_EVENTS | CAP_CAPTURES | CAP_STATS) -> bytes:
    return frame(MSG_HELLO_ACK, session, sequence, struct.pack("<BBHH", 2, capabilities, 800, 200))


def sample(index: int) -> bytes:
    return struct.pack(
        "<I9hHH",
        0 if index == 0 else 1250,
        -100 - index,
        -1,
        100 + index,
        -200 - index,
        2,
        200 + index,
        -300 - index,
        3,
        300 + index,
        4095 - index,
        0x000F | (0x0200 if index == 1 else 0),
    )


def capture_blob(
    capture_id: int,
    count: int = 3,
    flags: int = 1,
    pretrigger: int | None = None,
    posttrigger: int | None = None,
) -> bytes:
    if pretrigger is None:
        pretrigger = count - 1
    if posttrigger is None:
        posttrigger = count - pretrigger - 1
    if pretrigger + 1 + posttrigger != count:
        raise ValueError("invalid capture pre/post relationship")
    header = struct.pack(
        "<2sBBHIIIHHHHBBH9h",
        b"GC",
        1,
        CAPTURE_HEADER_SIZE,
        capture_id,
        0xFFFFFF00,
        0xFFFFF000,
        1250,
        count,
        pretrigger,
        pretrigger,
        posttrigger,
        CAPTURE_RECORD_SIZE,
        1,
        flags,
        -11,
        22,
        -33,
        44,
        -55,
        66,
        -77,
        88,
        -99,
    )
    return header + b"".join(sample(index) for index in range(count))


def capture_info(
    capture_id: int,
    count: int = 3,
    flags: int = 1,
    pretrigger: int | None = None,
) -> bytes:
    if pretrigger is None:
        pretrigger = count - 1
    return struct.pack("<HHHHH", capture_id, capture_blob_size(count), count, flags, pretrigger)


class ReceiverTests(unittest.TestCase):
    def test_mtu23_reverse_fragment_reassembly(self) -> None:
        receiver = GloveV2Receiver()
        now = 1_000_000
        receiver.receive(hello_ack(7, 0), now)
        now += 1
        receiver.receive(frame(MSG_CAPTURE_INFO, 7, 1, capture_info(19)), now)
        blob = capture_blob(19)
        fragments = [(offset, blob[offset : offset + 8]) for offset in range(0, len(blob), 8)]
        completed = None
        for sequence, (offset, data) in enumerate(reversed(fragments), start=2):
            packet = frame(MSG_CAPTURE_DATA, 7, sequence, struct.pack("<HHH", 19, offset, len(blob)) + data)
            self.assertLessEqual(len(packet), 20, "MTU=23 permits at most a 20-byte ATT payload")
            now += 1
            result = receiver.receive(packet, now)
            if result is not None:
                completed = result
        self.assertIsNotNone(completed)
        self.assertEqual(len(receiver.completed_captures), 1)
        capture = receiver.completed_captures[0]
        self.assertEqual(capture.metadata.capture_id, 19)
        self.assertEqual(capture.metadata.pretrigger_samples, 2)
        self.assertEqual(capture.samples[1].accel_x, -101)
        self.assertEqual(capture.samples[1].validity_flags, 0x020F)
        self.assertEqual(receiver.sequence_gaps, 0)

    def test_mtu247_reassembles_normal_601_record_capture(self) -> None:
        receiver = GloveV2Receiver()
        now = 2_000_000
        receiver.receive(hello_ack(16, 0), now)
        blob = capture_blob(20, count=601, pretrigger=400, posttrigger=200)
        self.assertEqual(len(blob), 15674)
        now += 1
        receiver.receive(
            frame(MSG_CAPTURE_INFO, 16, 1, capture_info(20, 601, 1, 400)), now
        )
        sequence = 2
        for offset in range(0, len(blob), 232):
            data = blob[offset : offset + 232]
            packet = frame(MSG_CAPTURE_DATA, 16, sequence, struct.pack("<HHH", 20, offset, len(blob)) + data)
            self.assertLessEqual(len(packet), 244, "ATT MTU 247 permits a 244-byte notification")
            now += 1
            receiver.receive(packet, now)
            sequence += 1
        self.assertEqual(len(receiver.completed_captures), 1)
        capture = receiver.completed_captures[0]
        self.assertEqual(capture.metadata.sample_count, 601)
        self.assertEqual(capture.metadata.trigger_index, 400)
        self.assertEqual(capture.metadata.posttrigger_samples, 200)
        self.assertEqual(capture.samples[-1].accel_x, -700)

    def test_shared_c_python_capture_vector(self) -> None:
        vector = bytes.fromhex("".join(VECTOR_PATH.read_text(encoding="ascii").split()))
        self.assertEqual(len(vector), 100)
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(17, 0), 100)
        receiver.receive(
            frame(MSG_CAPTURE_INFO, 17, 1, capture_info(0x1234, 2, 1, 1)), 101
        )
        complete = receiver.receive(
            frame(MSG_CAPTURE_DATA, 17, 2, struct.pack("<HHH", 0x1234, 0, len(vector)) + vector),
            102,
        )
        self.assertIsNotNone(complete)
        capture = receiver.completed_captures[0]
        self.assertEqual(capture.metadata.trigger_time_us, 0x01020304)
        self.assertEqual(capture.metadata.first_sample_time_us, 0xF0E0D0C0)
        self.assertEqual(capture.samples[0].accel_x, -32768)
        self.assertEqual(capture.samples[0].validity_flags, 0x020F)
        self.assertEqual(capture.samples[1].delta_us, 1250)
        self.assertEqual(capture.samples[1].validity_flags, 0x0C7F)

    def test_duplicate_event_is_rejected_without_replay(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(8, 0), 100)
        event = frame(MSG_EVENT, 8, 1, struct.pack("<HIHHH", 4, 5000, 2048, 123, 1))
        receiver.receive(event, 101)
        with self.assertRaises(ProtocolError):
            receiver.receive(event, 102)
        self.assertEqual(len(receiver.events), 1)
        self.assertEqual(receiver.expected_sequence, 2)
        self.assertEqual(receiver.sequence_reorders, 1)

    def test_invalid_payload_does_not_advance_sequence(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(9, 0), 100)
        malformed = frame(MSG_EVENT, 9, 1, b"short")
        with self.assertRaises(ProtocolError):
            receiver.receive(malformed, 101)
        self.assertEqual(receiver.expected_sequence, 1)
        valid = frame(MSG_EVENT, 9, 1, struct.pack("<HIHHH", 5, 6000, 1000, 100, 1))
        receiver.receive(valid, 102)
        self.assertEqual(len(receiver.events), 1)
        self.assertEqual(receiver.expected_sequence, 2)

    def test_stale_hello_requires_explicit_handshake(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(10, 0), 100)
        with self.assertRaises(ProtocolError):
            receiver.receive(hello_ack(10, 0), 101)
        self.assertEqual(receiver.session_id, 10)
        receiver.begin_handshake()
        receiver.receive(hello_ack(11, 0), 102)
        self.assertEqual(receiver.session_id, 11)

    def test_partial_capture_is_bounded_and_evicted(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(12, 0), 100)
        receiver.receive(frame(MSG_CAPTURE_INFO, 12, 1, capture_info(1)), 101)
        receiver.receive(frame(MSG_CAPTURE_INFO, 12, 2, capture_info(2)), 102)
        receiver.receive(frame(MSG_CAPTURE_INFO, 12, 3, capture_info(3)), 103)
        self.assertEqual(len(receiver._assemblies), 2)
        self.assertEqual(receiver.abandoned_captures, 1)
        receiver.receive(
            frame(MSG_EVENT, 12, 4, struct.pack("<HIHHH", 1, 1, 1, 1, 1)),
            103 + CAPTURE_ASSEMBLY_TIMEOUT_NS,
        )
        self.assertEqual(len(receiver._assemblies), 0)
        self.assertEqual(receiver.abandoned_captures, 3)

    def test_conflicting_overlap_and_stale_session_are_rejected(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(13, 0), 100)
        receiver.receive(frame(MSG_CAPTURE_INFO, 13, 1, capture_info(4)), 101)
        payload = struct.pack("<HHH", 4, 0, capture_blob_size(3)) + b"A"
        receiver.receive(frame(MSG_CAPTURE_DATA, 13, 2, payload), 102)
        conflicting = struct.pack("<HHH", 4, 0, capture_blob_size(3)) + b"B"
        with self.assertRaises(ProtocolError):
            receiver.receive(frame(MSG_CAPTURE_DATA, 13, 3, conflicting), 103)
        receiver.begin_handshake()
        receiver.receive(hello_ack(14, 0), 104)
        with self.assertRaises(ProtocolError):
            receiver.receive(frame(MSG_CAPTURE_DATA, 13, 4, payload), 105)

    def test_stats_are_exactly_23_values_in_eight_chunks(self) -> None:
        receiver = GloveV2Receiver()
        receiver.receive(hello_ack(15, 0), 100)
        sequence = 1
        values = list(range(STATS_VALUE_COUNT))
        for chunk in range(STATS_CHUNK_COUNT):
            first = chunk * STATS_CHUNK_VALUES
            chunk_values = values[first : first + STATS_CHUNK_VALUES]
            payload = struct.pack("<BB", chunk, STATS_CHUNK_COUNT) + struct.pack(
                "<" + "I" * len(chunk_values), *chunk_values
            )
            receiver.receive(frame(MSG_STATS, 15, sequence, payload), 100 + sequence)
            sequence += 1
        self.assertEqual(sum(len(values) for values in receiver.stats_chunks.values()), STATS_VALUE_COUNT)
        self.assertEqual(receiver.stats_chunks[7], (21, 22))


if __name__ == "__main__":
    unittest.main()
