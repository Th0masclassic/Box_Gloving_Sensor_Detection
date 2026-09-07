# Smart Boxing Glove BLE protocol v2

This document is the authoritative byte layout for the firmware in this
repository. All multi-byte values are unsigned or two's-complement **little
endian** unless a table says otherwise. The protocol is byte-oriented; do not
cast a received buffer to a native C structure.

## GATT discovery and session setup

The peripheral advertises this service and one bidirectional characteristic:

| Item | UUID |
| --- | --- |
| Service | `12345678-1234-5678-1234-56789abcdef0` |
| Read / write / notify characteristic | `12345678-1234-5678-1234-56789abcdef1` |

The central must enable notifications first, then write this exact four-byte
HELLO to the characteristic:

| Byte | Value | Meaning |
| ---: | --- | --- |
| 0 | `0x47` | ASCII `G` |
| 1 | `0x42` | ASCII `B` |
| 2 | `0x02` | protocol version |
| 3 | capability mask | one or more supported capability bits |

Supported capability bits are `0x01` events, `0x02` captures, and `0x04`
statistics. Zero and unknown bits are rejected. Each accepted HELLO creates a
new session and causes a `HELLO_ACK` notification. A disconnect, notification
unsubscribe, host reset, or another accepted HELLO invalidates the prior
session. A client must discard all in-progress captures when the session
changes.

A client must clear its local receive state immediately before each HELLO and
accept `HELLO_ACK` only while that HELLO is pending. The protocol has no client
nonce, so this contract prevents a duplicate or delayed acknowledgement from
silently resetting a live receiver session.

The firmware requests a 247-byte ATT MTU and caps a notification at 244 bytes
so a full notification fits in one 251-octet data-length-extension link-layer
PDU. The client must still accept the negotiated MTU. At the mandatory ATT MTU
of 23, an ATT notification carries 20 bytes and capture fragments have only 8
bytes of raw blob data.

## Notification frame

Every notification begins with this six-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | magic `0xB2` |
| 1 | 1 | message type |
| 2 | 2 | session ID |
| 4 | 2 | sequence number |

The sequence starts at zero after HELLO and increases after each notification
accepted by the local NimBLE stack. It detects missing or reordered
notifications but is not an application acknowledgement: a successful BLE API
call does not prove that the central received the data. Sequence arithmetic is
modulo 65536.

| Type | Name | Payload length |
| ---: | --- | ---: |
| `0x01` | `HELLO_ACK` | 6 |
| `0x10` | `EVENT` | 12 |
| `0x11` | `CAPTURE_INFO` | 10 |
| `0x12` | `CAPTURE_DATA` | 6–238 |
| `0x13` | `STATS` | 2–14 |
| `0x7F` | `ERROR` | reserved; not emitted by this firmware release |

Receivers should reject an unknown type, a bad magic value, a short fixed-size
payload, or a frame for a session other than the active `HELLO_ACK` session.

### HELLO_ACK (`0x01`)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | protocol version (`2`) |
| 1 | 1 | accepted capability mask |
| 2 | 2 | ADXL345 nominal rate, Hz (`800`) |
| 4 | 2 | QMC5883L nominal rate, Hz (`200`) |

### EVENT (`0x10`)

An event is queued immediately after the FSR threshold detector runs. It is
sent ahead of raw capture fragments and should remain useful even when a raw
snapshot is dropped.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | punch ID |
| 2 | 4 | trigger timestamp, device microseconds modulo 2^32 |
| 6 | 2 | unmodified FSR ADC count |
| 8 | 2 | empirical force estimate in centi-kilograms |
| 10 | 2 | flags |

Event flags: bit 0 is FSR valid; bit 1 means a capture slot was reserved; bit
2 means a requested capture slot was unavailable. The force estimate preserves
the project's existing empirical conversion and is not a calibrated load-cell
measurement.

### CAPTURE_INFO (`0x11`)

This precedes every raw capture.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | capture ID |
| 2 | 2 | total capture-blob bytes |
| 4 | 2 | raw record count |
| 6 | 2 | capture flags |
| 8 | 2 | trigger record index |

### CAPTURE_DATA (`0x12`)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | capture ID |
| 2 | 2 | byte offset in the capture blob |
| 4 | 2 | total capture-blob bytes |
| 6 | variable | immutable blob bytes |

Fragments may be split at any blob offset. A receiver must verify that the
capture ID and total match `CAPTURE_INFO`, bounds-check `offset + data length`,
and retain a received-byte map. It may accept identical overlapping bytes but
must reject conflicting overlap. The capture is complete only when every byte
from zero through `total - 1` is present and its blob header validates. There
is no CRC in v2; completeness, bounds, count, and metadata validation are the
integrity checks.

## Capture blob

`CAPTURE_INFO.total_bytes` must equal `48 + 26 * sample_count`. A normal
capture has 401 pre-context records including the trigger and 200 post-trigger
records: 601 records and 15,674 bytes. Early after boot, the pre-context may be
shorter; flag bit 1 records that condition.

### Header (48 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | ASCII `GC` |
| 2 | 1 | blob version (`1`) |
| 3 | 1 | header bytes (`48`) |
| 4 | 2 | capture ID |
| 6 | 4 | trigger timestamp, device microseconds modulo 2^32 |
| 10 | 4 | first-record timestamp, device microseconds modulo 2^32 |
| 14 | 4 | nominal period in microseconds (`1250`) |
| 18 | 2 | sample count |
| 20 | 2 | trigger index |
| 22 | 2 | pretrigger count |
| 24 | 2 | posttrigger count |
| 26 | 1 | record bytes (`26`) |
| 27 | 1 | sensor representation (`1`, native signed counts) |
| 28 | 2 | capture flags |
| 30 | 6 | ADXL345 X/Y/Z rest offsets, signed counts |
| 36 | 6 | ITG-3200 X/Y/Z rest offsets, signed counts |
| 42 | 6 | QMC5883L X/Y/Z calibration reference, signed counts |

The relationships `trigger_index == pretrigger_count`,
`pretrigger_count + 1 + posttrigger_count == sample_count`, a nonzero period,
and the total byte count are mandatory validation checks.

Capture flags: bit 0 means the stillness calibration completed; bit 1 means
the pre-trigger history was short. Bit 2 is reserved in this implementation.
The calibration offsets are metadata only; raw records remain unmodified.

### Raw record (26 bytes)

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | elapsed microseconds from preceding record; zero for record 0 |
| 4 | 2 | ADXL345 X, signed raw count |
| 6 | 2 | ADXL345 Y, signed raw count |
| 8 | 2 | ADXL345 Z, signed raw count |
| 10 | 2 | ITG-3200 X, signed raw count |
| 12 | 2 | ITG-3200 Y, signed raw count |
| 14 | 2 | ITG-3200 Z, signed raw count |
| 16 | 2 | QMC5883L X, signed raw count |
| 18 | 2 | QMC5883L Y, signed raw count |
| 20 | 2 | QMC5883L Z, signed raw count |
| 22 | 2 | FSR ADC count |
| 24 | 2 | validity and quality flags |

The device timestamp and deltas wrap naturally as unsigned 32-bit values.
They are useful to reconstruct device-local time, but cannot be subtracted from
the receiver clock to claim end-to-end latency.

Raw scales are ADXL345 full-resolution ±16 g: 0.0039 g/count; ITG-3200 ±2000
degrees/s: 14.375 counts/(degree/s); QMC5883L ±2 G: 12,000 counts/G. The
ADXL345 is triggered at 800 Hz. Gyro and magnetometer clocks are independent;
freshness flags describe their status instead of claiming synchronized samples.

Record flag bits:

| Bit | Meaning |
| ---: | --- |
| 0–3 | valid ACCEL, GYRO, MAG, FSR respectively |
| 4–6 | cached/stale ACCEL, GYRO, MAG respectively |
| 7 | ADXL data-ready wait timed out |
| 8 | reserved (v2 uses a 32-bit delta) |
| 9 | ADXL345 value near ±16 g range |
| 10 | ITG-3200 value near ±2000 degrees/s range |
| 11 | QMC5883L overflow or data-overrun status |

A stale sample can remain valid because it contains the last raw value from
that sensor. Saturated and overflow values are preserved rather than clipped.

## STATS (`0x13`)

Statistics are sent in chunks after HELLO and then approximately every second
when there is no event or raw capture traffic. A stats payload starts with
`chunk_index` and `total_chunks`, followed by one to three little-endian
`uint32` values. This firmware has 23 values, so `total_chunks` is 8. A stats
snapshot is diagnostic telemetry; counters can change while chunks are sent.

| Index | Counter |
| ---: | --- |
| 0 | acquired sample cycles |
| 1 | data-ready wait timeouts |
| 2 | coalesced data-ready notifications |
| 3–6 | ACCEL, GYRO, MAG, FSR read errors |
| 7 | raw capture slots unavailable or initialization failed |
| 8 | event queue drops |
| 9 | completed-capture queue drops |
| 10 | non-session transmit failures |
| 11 | session-change transmit drops |
| 12 | event transmit retry/deadline drops |
| 13 | capture transmit retry/deadline drops |
| 14 | threshold detector trigger count |
| 15 | sample timestamp gaps above 1.5 nominal periods |
| 16 | largest observed sample timestamp gap, microseconds |
| 17 | acquisition work durations above one nominal period |
| 18 | largest acquisition work duration through sensor reads, microseconds |
| 19–22 | fresh ACCEL, GYRO, MAG, FSR-valid sample counts |

The work-duration measurement ends after sensor reads; it intentionally does
not include ring storage or raw snapshot copying. The following timestamp gap
counter exposes the cadence effect of those later operations.

## Reliability and timing boundaries

The transport retries a pending event for up to five attempts or 50 ms and a
stalled capture for up to 20 consecutive failures or 3 seconds. It frees a
capture on session change, retry expiry, or queue failure. The two capture
slots are bounded; a new event is still emitted if all snapshot slots are busy.

Host receipt timestamps may measure notification inter-arrival, receiver
throughput, sequence gaps, and capture-completion time. They are not
synchronized with ESP uptime, so they must not be described as trigger-to-host
latency. Measuring that latency requires a shared clock, explicit round-trip
protocol, or external instrumentation.

The supplied Python receiver retains at most two partial captures, matching the
firmware's two snapshot slots, and evicts an incomplete capture after five
seconds without a fragment. It reports those evictions as abandoned captures.
