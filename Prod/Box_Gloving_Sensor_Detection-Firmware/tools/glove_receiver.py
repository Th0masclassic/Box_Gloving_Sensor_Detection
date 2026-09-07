#!/usr/bin/env python3
"""Strict reference receiver and decoder for Smart Boxing Glove BLE protocol v2.

The decoder uses only the Python standard library.  BLE collection is optional
and uses Bleak when ``--address`` or ``--name`` is supplied.  It deliberately
reports receiver-side inter-arrival and completion metrics, never an absolute
device-to-host latency: ESP uptime and ``perf_counter_ns`` have no shared clock.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import struct
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
CHARACTERISTIC_UUID = "12345678-1234-5678-1234-56789abcdef1"

MAGIC = 0xB2
VERSION = 2
FRAME_HEADER_SIZE = 6
HELLO = bytes((ord("G"), ord("B"), VERSION))
CAP_EVENTS = 0x01
CAP_CAPTURES = 0x02
CAP_STATS = 0x04
SUPPORTED_CAPABILITIES = CAP_EVENTS | CAP_CAPTURES | CAP_STATS

MSG_HELLO_ACK = 0x01
MSG_EVENT = 0x10
MSG_CAPTURE_INFO = 0x11
MSG_CAPTURE_DATA = 0x12
MSG_STATS = 0x13
MSG_ERROR = 0x7F

CAPTURE_HEADER_SIZE = 48
CAPTURE_RECORD_SIZE = 26
STATS_VALUE_COUNT = 23
STATS_CHUNK_VALUES = 3
STATS_CHUNK_COUNT = (STATS_VALUE_COUNT + STATS_CHUNK_VALUES - 1) // STATS_CHUNK_VALUES
MAX_ACTIVE_CAPTURES = 2
CAPTURE_ASSEMBLY_TIMEOUT_NS = 5_000_000_000

_FRAME_HEADER = struct.Struct("<BBHH")
_CAPTURE_HEADER = struct.Struct("<2sBBHIIIHHHHBBH9h")
_CAPTURE_RECORD = struct.Struct("<I9hHH")


class ProtocolError(ValueError):
    """A malformed, stale-session, or internally inconsistent v2 frame."""


@dataclass(frozen=True)
class Frame:
    message_type: int
    session_id: int
    sequence: int
    payload: bytes
    received_monotonic_ns: int


@dataclass(frozen=True)
class HelloAck:
    session_id: int
    sequence: int
    capabilities: int
    accel_rate_hz: int
    mag_rate_hz: int


@dataclass(frozen=True)
class PunchEvent:
    session_id: int
    sequence: int
    punch_id: int
    trigger_time_us: int
    fsr_adc: int
    force_centi_kg: int
    flags: int
    received_monotonic_ns: int


@dataclass(frozen=True)
class CaptureInfo:
    capture_id: int
    total_bytes: int
    sample_count: int
    capture_flags: int
    trigger_index: int


@dataclass(frozen=True)
class CaptureMetadata:
    capture_id: int
    trigger_time_us: int
    first_sample_time_us: int
    nominal_sample_period_us: int
    sample_count: int
    trigger_index: int
    pretrigger_samples: int
    posttrigger_samples: int
    capture_flags: int
    accel_offset: Tuple[int, int, int]
    gyro_offset: Tuple[int, int, int]
    mag_reference: Tuple[int, int, int]


@dataclass(frozen=True)
class RawSample:
    delta_us: int
    accel_x: int
    accel_y: int
    accel_z: int
    gyro_x: int
    gyro_y: int
    gyro_z: int
    mag_x: int
    mag_y: int
    mag_z: int
    fsr_adc: int
    validity_flags: int


@dataclass(frozen=True)
class CompletedCapture:
    session_id: int
    info: CaptureInfo
    metadata: CaptureMetadata
    samples: Tuple[RawSample, ...]
    completed_monotonic_ns: int


@dataclass
class _CaptureAssembly:
    info: CaptureInfo
    created_monotonic_ns: int
    blob: bytearray = field(init=False)
    received: bytearray = field(init=False)
    received_count: int = 0
    last_update_monotonic_ns: int = field(init=False)

    def __post_init__(self) -> None:
        self.blob = bytearray(self.info.total_bytes)
        self.received = bytearray(self.info.total_bytes)
        self.last_update_monotonic_ns = self.created_monotonic_ns

    def add(self, offset: int, data: bytes, received_monotonic_ns: int) -> None:
        if not data:
            raise ProtocolError("capture data fragment is empty")
        if offset < 0 or offset + len(data) > len(self.blob):
            raise ProtocolError("capture data fragment exceeds declared blob size")
        for index, value in enumerate(data, start=offset):
            if self.received[index]:
                if self.blob[index] != value:
                    raise ProtocolError("capture data fragment conflicts with an earlier byte")
                continue
            self.blob[index] = value
            self.received[index] = 1
            self.received_count += 1
        self.last_update_monotonic_ns = received_monotonic_ns

    @property
    def complete(self) -> bool:
        return self.received_count == len(self.blob)


def capture_blob_size(sample_count: int) -> int:
    if not 1 <= sample_count <= 0xFFFF:
        raise ProtocolError("capture sample count is outside the u16 protocol range")
    return CAPTURE_HEADER_SIZE + CAPTURE_RECORD_SIZE * sample_count


def decode_capture_blob(blob: bytes, info: CaptureInfo) -> Tuple[CaptureMetadata, Tuple[RawSample, ...]]:
    """Decode and cross-check a fully reassembled capture blob."""
    if len(blob) < CAPTURE_HEADER_SIZE:
        raise ProtocolError("capture blob is shorter than its fixed header")
    unpacked = _CAPTURE_HEADER.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        capture_id,
        trigger_time_us,
        first_sample_time_us,
        nominal_period_us,
        sample_count,
        trigger_index,
        pretrigger_samples,
        posttrigger_samples,
        record_size,
        representation,
        capture_flags,
        accel_x,
        accel_y,
        accel_z,
        gyro_x,
        gyro_y,
        gyro_z,
        mag_x,
        mag_y,
        mag_z,
    ) = unpacked
    if magic != b"GC" or version != 1 or header_size != CAPTURE_HEADER_SIZE:
        raise ProtocolError("unsupported capture blob header")
    if record_size != CAPTURE_RECORD_SIZE or representation != 1:
        raise ProtocolError("unsupported capture record representation")
    if nominal_period_us == 0 or sample_count == 0:
        raise ProtocolError("capture metadata has a zero period or count")
    if trigger_index != pretrigger_samples or trigger_index >= sample_count:
        raise ProtocolError("capture trigger index is inconsistent")
    if pretrigger_samples + 1 + posttrigger_samples != sample_count:
        raise ProtocolError("capture pre/post counts are inconsistent")
    if capture_blob_size(sample_count) != len(blob):
        raise ProtocolError("capture blob length does not match its sample count")
    if (
        capture_id != info.capture_id
        or sample_count != info.sample_count
        or trigger_index != info.trigger_index
        or capture_flags != info.capture_flags
    ):
        raise ProtocolError("capture blob metadata disagrees with CAPTURE_INFO")

    metadata = CaptureMetadata(
        capture_id=capture_id,
        trigger_time_us=trigger_time_us,
        first_sample_time_us=first_sample_time_us,
        nominal_sample_period_us=nominal_period_us,
        sample_count=sample_count,
        trigger_index=trigger_index,
        pretrigger_samples=pretrigger_samples,
        posttrigger_samples=posttrigger_samples,
        capture_flags=capture_flags,
        accel_offset=(accel_x, accel_y, accel_z),
        gyro_offset=(gyro_x, gyro_y, gyro_z),
        mag_reference=(mag_x, mag_y, mag_z),
    )
    samples: List[RawSample] = []
    for index in range(sample_count):
        values = _CAPTURE_RECORD.unpack_from(blob, CAPTURE_HEADER_SIZE + index * CAPTURE_RECORD_SIZE)
        samples.append(RawSample(*values))
    return metadata, tuple(samples)


class GloveV2Receiver:
    """Session-aware v2 notification receiver with strict capture reassembly."""

    def __init__(self) -> None:
        self.session_id: Optional[int] = None
        self.awaiting_hello_ack = True
        self.capabilities = 0
        self.accel_rate_hz = 0
        self.mag_rate_hz = 0
        self.expected_sequence: Optional[int] = None
        self.frames_received = 0
        self.sequence_gaps = 0
        self.sequence_reorders = 0
        self.rejected_frames = 0
        self.abandoned_captures = 0
        self.events: List[PunchEvent] = []
        self.completed_captures: List[CompletedCapture] = []
        self.stats_chunks: Dict[int, Tuple[int, ...]] = {}
        self._assemblies: Dict[Tuple[int, int], _CaptureAssembly] = {}
        self._last_receive_ns: Optional[int] = None
        self.interarrival_ns: List[int] = []

    def begin_handshake(self) -> None:
        """Discard the old link state immediately before writing the HELLO command.

        The device assigns the session ID, so a HELLO_ACK is accepted only while
        this method's pending-handshake state is true.  Call it after a new BLE
        connection and before every HELLO write.
        """
        self.abandoned_captures += len(self._assemblies)
        self._assemblies.clear()
        self.stats_chunks.clear()
        self.session_id = None
        self.capabilities = 0
        self.accel_rate_hz = 0
        self.mag_rate_hz = 0
        self.expected_sequence = None
        self.awaiting_hello_ack = True
        self._last_receive_ns = None

    def receive(self, notification: bytes, received_monotonic_ns: Optional[int] = None) -> Optional[object]:
        """Process one notification and return a decoded event/capture/HELLO when complete."""
        now_ns = time.perf_counter_ns() if received_monotonic_ns is None else received_monotonic_ns
        if len(notification) < FRAME_HEADER_SIZE:
            raise ProtocolError("notification is shorter than the v2 frame header")
        magic, message_type, session_id, sequence = _FRAME_HEADER.unpack_from(notification)
        if magic != MAGIC:
            raise ProtocolError("unexpected notification magic")
        payload = notification[FRAME_HEADER_SIZE:]
        frame = Frame(message_type, session_id, sequence, payload, now_ns)
        self.frames_received += 1
        if self._last_receive_ns is not None and now_ns >= self._last_receive_ns:
            self.interarrival_ns.append(now_ns - self._last_receive_ns)
        self._last_receive_ns = now_ns
        self._evict_expired_captures(now_ns)

        if message_type == MSG_HELLO_ACK:
            return self._handle_hello_ack(frame)
        if self.session_id is None or frame.session_id != self.session_id:
            raise ProtocolError("frame belongs to an inactive session")
        gap = self._pending_sequence_gap(frame.sequence)
        if message_type == MSG_EVENT:
            result = self._handle_event(frame)
        elif message_type == MSG_CAPTURE_INFO:
            result = self._handle_capture_info(frame)
        elif message_type == MSG_CAPTURE_DATA:
            result = self._handle_capture_data(frame)
        elif message_type == MSG_STATS:
            result = self._handle_stats(frame)
        elif message_type == MSG_ERROR:
            raise ProtocolError("ERROR frame is reserved and not defined by v2")
        else:
            raise ProtocolError(f"unsupported v2 message type 0x{message_type:02X}")
        self._commit_sequence(frame.sequence, gap)
        return result

    def _handle_hello_ack(self, frame: Frame) -> HelloAck:
        if len(frame.payload) != 6:
            raise ProtocolError("HELLO_ACK has an invalid payload length")
        version, capabilities, accel_rate_hz, mag_rate_hz = struct.unpack("<BBHH", frame.payload)
        if version != VERSION:
            raise ProtocolError(f"unsupported protocol version {version}")
        if capabilities == 0 or capabilities & ~SUPPORTED_CAPABILITIES:
            raise ProtocolError("HELLO_ACK contains unsupported capabilities")
        if accel_rate_hz == 0 or mag_rate_hz == 0:
            raise ProtocolError("HELLO_ACK contains a zero sampling rate")
        if not self.awaiting_hello_ack:
            raise ProtocolError("unexpected or duplicate HELLO_ACK")
        self.stats_chunks.clear()
        self.session_id = frame.session_id
        self.capabilities = capabilities
        self.accel_rate_hz = accel_rate_hz
        self.mag_rate_hz = mag_rate_hz
        self.expected_sequence = (frame.sequence + 1) & 0xFFFF
        self.awaiting_hello_ack = False
        return HelloAck(frame.session_id, frame.sequence, capabilities, accel_rate_hz, mag_rate_hz)

    def _pending_sequence_gap(self, sequence: int) -> int:
        """Validate sequence without mutating state until the payload validates."""
        if self.expected_sequence is None or sequence == self.expected_sequence:
            return 0
        difference = (sequence - self.expected_sequence) & 0xFFFF
        if 0 < difference < 0x8000:
            return difference
        self.sequence_reorders += 1
        raise ProtocolError("duplicate or reordered notification")

    def _commit_sequence(self, sequence: int, gap: int) -> None:
        self.sequence_gaps += gap
        self.expected_sequence = (sequence + 1) & 0xFFFF

    def _evict_expired_captures(self, now_ns: int) -> None:
        expired = [
            key
            for key, assembly in self._assemblies.items()
            if now_ns - assembly.last_update_monotonic_ns >= CAPTURE_ASSEMBLY_TIMEOUT_NS
        ]
        for key in expired:
            del self._assemblies[key]
            self.abandoned_captures += 1

    @staticmethod
    def _require_length(frame: Frame, length: int, label: str) -> None:
        if len(frame.payload) != length:
            raise ProtocolError(f"{label} has invalid payload length {len(frame.payload)}")

    def _handle_event(self, frame: Frame) -> PunchEvent:
        self._require_length(frame, 12, "EVENT")
        punch_id, trigger_time_us, fsr_adc, force_centi_kg, flags = struct.unpack("<HIHHH", frame.payload)
        event = PunchEvent(
            frame.session_id,
            frame.sequence,
            punch_id,
            trigger_time_us,
            fsr_adc,
            force_centi_kg,
            flags,
            frame.received_monotonic_ns,
        )
        self.events.append(event)
        return event

    def _handle_capture_info(self, frame: Frame) -> CaptureInfo:
        self._require_length(frame, 10, "CAPTURE_INFO")
        capture_id, total_bytes, sample_count, capture_flags, trigger_index = struct.unpack(
            "<HHHHH", frame.payload
        )
        if total_bytes != capture_blob_size(sample_count) or trigger_index >= sample_count:
            raise ProtocolError("CAPTURE_INFO has inconsistent length or trigger index")
        info = CaptureInfo(capture_id, total_bytes, sample_count, capture_flags, trigger_index)
        key = (frame.session_id, capture_id)
        existing = self._assemblies.get(key)
        if existing is not None and existing.info != info:
            raise ProtocolError("CAPTURE_INFO changes an in-progress capture")
        if existing is None:
            if len(self._assemblies) >= MAX_ACTIVE_CAPTURES:
                oldest_key = min(
                    self._assemblies,
                    key=lambda candidate: self._assemblies[candidate].last_update_monotonic_ns,
                )
                del self._assemblies[oldest_key]
                self.abandoned_captures += 1
            self._assemblies[key] = _CaptureAssembly(info, frame.received_monotonic_ns)
        return info

    def _handle_capture_data(self, frame: Frame) -> Optional[CompletedCapture]:
        if len(frame.payload) < 7:
            raise ProtocolError("CAPTURE_DATA needs a header and at least one blob byte")
        capture_id, offset, total_bytes = struct.unpack_from("<HHH", frame.payload)
        key = (frame.session_id, capture_id)
        assembly = self._assemblies.get(key)
        if assembly is None:
            raise ProtocolError("CAPTURE_DATA arrived before CAPTURE_INFO")
        if total_bytes != assembly.info.total_bytes:
            raise ProtocolError("CAPTURE_DATA total does not match CAPTURE_INFO")
        assembly.add(offset, frame.payload[6:], frame.received_monotonic_ns)
        if not assembly.complete:
            return None
        try:
            metadata, samples = decode_capture_blob(bytes(assembly.blob), assembly.info)
        except ProtocolError:
            del self._assemblies[key]
            self.abandoned_captures += 1
            raise
        completed = CompletedCapture(
            session_id=frame.session_id,
            info=assembly.info,
            metadata=metadata,
            samples=samples,
            completed_monotonic_ns=frame.received_monotonic_ns,
        )
        del self._assemblies[key]
        self.completed_captures.append(completed)
        return completed

    def _handle_stats(self, frame: Frame) -> Tuple[int, Tuple[int, ...]]:
        if len(frame.payload) < 2 or (len(frame.payload) - 2) % 4:
            raise ProtocolError("STATS payload is not a sequence of u32 values")
        chunk_index, total_chunks = struct.unpack_from("<BB", frame.payload)
        if total_chunks != STATS_CHUNK_COUNT or chunk_index >= total_chunks:
            raise ProtocolError("STATS chunk numbering does not match protocol v2")
        values = struct.unpack_from("<" + "I" * ((len(frame.payload) - 2) // 4), frame.payload, 2)
        expected_values = min(STATS_CHUNK_VALUES, STATS_VALUE_COUNT - chunk_index * STATS_CHUNK_VALUES)
        if len(values) != expected_values:
            raise ProtocolError("STATS chunk has the wrong number of counters")
        self.stats_chunks[chunk_index] = values
        return chunk_index, values

    def export(self) -> dict:
        """Return JSON-serializable receiver state, including raw completed captures."""
        return {
            "protocol_version": VERSION,
            "session_id": self.session_id,
            "awaiting_hello_ack": self.awaiting_hello_ack,
            "capabilities": self.capabilities,
            "accel_rate_hz": self.accel_rate_hz,
            "mag_rate_hz": self.mag_rate_hz,
            "frames_received": self.frames_received,
            "sequence_gaps": self.sequence_gaps,
            "sequence_reorders": self.sequence_reorders,
            "rejected_frames": self.rejected_frames,
            "abandoned_captures": self.abandoned_captures,
            "events": [asdict(event) for event in self.events],
            "captures": [asdict(capture) for capture in self.completed_captures],
            "stats_chunks": {str(index): list(values) for index, values in self.stats_chunks.items()},
            "interarrival_ns": self.interarrival_ns,
        }

    def summary(self) -> str:
        chunks = ", ".join(str(index) for index in sorted(self.stats_chunks)) or "none"
        text = [
            f"frames={self.frames_received} session={self.session_id} events={len(self.events)} "
            f"captures={len(self.completed_captures)}",
            f"sequence_gaps={self.sequence_gaps} reorders={self.sequence_reorders} stats_chunks={chunks}",
        ]
        if self.interarrival_ns:
            values_ms = [value / 1_000_000.0 for value in self.interarrival_ns]
            text.append(
                "notification inter-arrival ms: "
                f"mean={statistics.fmean(values_ms):.3f} min={min(values_ms):.3f} max={max(values_ms):.3f}"
            )
        text.append("Device and host clocks are unsynchronized; this is not trigger-to-host latency.")
        return "\n".join(text)


def _parse_capabilities(value: str) -> int:
    parsed = int(value, 0)
    if parsed == 0 or parsed & ~SUPPORTED_CAPABILITIES:
        raise argparse.ArgumentTypeError("capabilities must be a nonzero subset of 0x07")
    return parsed


async def collect_ble(args: argparse.Namespace, receiver: GloveV2Receiver) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:  # pragma: no cover - external optional dependency
        raise RuntimeError("BLE collection requires `pip install bleak`") from exc

    address = args.address
    if address is None:
        devices = await BleakScanner.discover(timeout=args.scan_timeout)
        match = next((device for device in devices if device.name == args.name), None)
        if match is None:
            raise RuntimeError(f"could not find BLE device named {args.name!r}")
        address = match.address

    def notification_handler(_: int, data: bytearray) -> None:
        try:
            receiver.receive(bytes(data))
        except ProtocolError as exc:
            receiver.rejected_frames += 1
            print(f"Rejected notification: {exc}", file=sys.stderr)

    async with BleakClient(address) as client:
        receiver.begin_handshake()
        await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
        await client.write_gatt_char(
            CHARACTERISTIC_UUID, HELLO + bytes((args.capabilities,)), response=True
        )
        await asyncio.sleep(args.duration)
        await client.stop_notify(CHARACTERISTIC_UUID)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="BLE address or platform identifier")
    parser.add_argument("--name", default="SMART_BOXING_GLOVE", help="advertised device name to scan for")
    parser.add_argument("--scan-timeout", type=float, default=5.0, help="scan duration in seconds")
    parser.add_argument("--duration", type=float, default=15.0, help="collection duration in seconds")
    parser.add_argument("--capabilities", type=_parse_capabilities, default=SUPPORTED_CAPABILITIES)
    parser.add_argument("--output", type=Path, help="write decoded events and captures as JSON")
    parser.add_argument(
        "--hex",
        action="append",
        default=[],
        help="decode a hex notification offline; may be passed more than once",
    )
    args = parser.parse_args(argv)
    receiver = GloveV2Receiver()
    try:
        if args.hex:
            for encoded in args.hex:
                receiver.receive(bytes.fromhex(encoded))
        elif args.address or args.name:
            asyncio.run(collect_ble(args, receiver))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(receiver.export(), indent=2), encoding="utf-8")
        print(receiver.summary())
        return 0
    except (ProtocolError, RuntimeError, ValueError) as exc:
        print(f"Receiver failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
