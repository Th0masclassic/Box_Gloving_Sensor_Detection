"""Structural parity check for the same JSON fixture consumed by XCTest.

Run on Windows or macOS with: python Tests/verify_protocol_fixtures.py
It intentionally validates only wire structure and never changes firmware.
"""

from __future__ import annotations

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
FIXTURE_PATH = ROOT / "Fixtures" / "protocol-v2-fixtures.json"


def frame_from_hex(raw: str) -> bytes:
    frame = bytes.fromhex(raw)
    assert len(frame) >= 6, "frame is shorter than the v2 header"
    assert frame[0] == 0xB2, "wrong v2 magic"
    return frame


def parse_stats(frame: bytes) -> tuple[int, int, list[int]]:
    assert frame[1] == 0x13
    payload = frame[6:]
    assert 6 <= len(payload) <= 14 and (len(payload) - 2) % 4 == 0
    index, total = payload[:2]
    assert 0 < total <= 64 and index < total
    values = [int.from_bytes(payload[offset : offset + 4], "little") for offset in range(2, len(payload), 4)]
    assert 1 <= len(values) <= 3
    if index < total - 1:
        assert len(values) == 3
    return index, total, values


def main() -> None:
    fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    assert bytes.fromhex(fixture["hello"]) == bytes((0x47, 0x42, 0x02, 0x05))

    stats_values: list[int] = []
    for item in fixture["validFrames"]:
        frame = frame_from_hex(item["hex"])
        kind = item["kind"]
        if kind == "helloAck":
            assert frame[1] == 0x01 and len(frame) == 12
            assert frame[6:] == bytes((2, 5, 0x20, 0x03, 0xC8, 0))
        elif kind == "event":
            assert frame[1] == 0x10 and len(frame) == 18
            adc = int.from_bytes(frame[12:14], "little")
            flags = int.from_bytes(frame[16:18], "little")
            assert adc == item["adc"] <= 4095 and flags == 1
        elif kind == "stats":
            index, total, values = parse_stats(frame)
            assert (index, total, values) == (item["index"], item["total"], item["values"])
            stats_values.extend(values)
        else:
            raise AssertionError(f"unknown fixture kind: {kind}")

    assert stats_values == list(range(1, 24)), "expected the 23 v2 firmware counters in 8 chunks"

    for item in fixture["sequenceCases"]:
        previous, incoming = item["previous"], item["incoming"]
        delta = (incoming - previous) & 0xFFFF
        if item["outcome"] == "accepted":
            assert 0 < delta < 0x8000
            assert delta - 1 == item["missed"]
        elif item["outcome"] == "duplicate":
            assert delta == 0
        elif item["outcome"] == "stale":
            assert delta >= 0x8000
        else:
            raise AssertionError(f"unknown sequence outcome: {item['outcome']}")

    print("protocol-v2 fixtures: OK")


if __name__ == "__main__":
    main()
