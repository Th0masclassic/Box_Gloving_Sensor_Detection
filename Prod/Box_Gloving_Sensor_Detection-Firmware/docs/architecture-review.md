# Firmware performance architecture review

Audit baseline: `origin/main` commit `15f4f24bfcceecf386ef48ffbeb379a6cf0f78e1`. This is the design review, not a claim that hardware performance has been measured. The implementation's protocol document is authoritative for final byte layouts.

## Hardware and SDK evidence

The user confirmed ESP32-C3 + GY-85 + FSR. Existing drivers identify the following fitted sensor configuration; initialization should check identities and configuration write results rather than assume that every board sold as GY-85 is identical.

| Device | Existing configuration | Preserve |
|---|---|---|
| ADXL345 | I2C 0x53, full resolution, +/-16 g, 100 Hz, FIFO bypass | Address, scale, INT1 GPIO6 |
| ITG-3200 | I2C 0x68, +/-2000 degrees/s, DLPF_CFG=0 | Address, axis order, 14.375 LSB/(degree/s) |
| QMC5883L | I2C 0x0D, control 0x0D, 200 Hz, +/-2 gauss | Address, scale 12000 LSB/gauss |
| FSR | ADC1 channel0, 12 dB attenuation | ADC channel and original empirical conversion coefficients |
| Bus / LED | 400 kHz I2C; SDA10, SCL9; LED8 | All wiring |

The firmware branch's tracked sdkconfig records ESP-IDF initialization version 5.5.3, esp32c3, NimBLE and a 100 Hz FreeRTOS tick. The production baseline omits sdkconfig and version pinning. A build under locally available 5.4.4 is useful compatibility validation but must be named accurately. No physical silicon revision or FSR part number has been established.

## Performance limits and sampling decision

800 Hz is the highest ADXL345 output rate recommended by Analog Devices with the existing 400 kHz I2C bus. Configure BW_RATE=0x0D and full-resolution +/-16 g. This is a design ceiling, not proof that the complete shared-bus application meets every 1.25 ms deadline. The accelerometer's +/-16 g physical clipping cannot be corrected in software; retain saturation flags for training data. [ADXL345 datasheet, serial communication and data format](https://www.analog.com/media/en/technical-documentation/data-sheets/adxl345.pdf)

ITG-3200 DLPF_FS=0x18 retains the 256 Hz filter and an 8 kHz internal rate; divider9 produces 800 Hz output. The gyro clock is independent of ADXL345, so nominally equal rates do not create simultaneous measurements. Poll readiness and preserve sample validity/freshness rather than claim synchronization. [ITG-3200 register map, registers21–22](https://invensense.tdk.com/wp-content/uploads/2015/02/ITG-3200-Register-Map.pdf)

QMC5883L stays at 200 Hz and +/-2 gauss. Read its status and complete XYZ data only at its useful rate; repeated cached values need an explicit freshness bit or age. Overflow is a validity condition, not a new valid orientation. [QMC5883L datasheet, status and control registers](https://www.qstcorp.com/upload/pdf/202512/13-52-04%20QMC5883L%20Datasheet%20Rev.%20B.pdf)

One six-byte I2C register burst consumes roughly81 SCL clocks before controller/software overhead: about203 us at400 kHz. Two such reads per800 Hz sample use roughly32% of the bus;200 Hz magnetometer reads add4%. Status reads, starts/stops, ADC, task scheduling and error recovery consume additional time. This calculation only establishes plausibility. Measure worst case acquisition time with BLE congested. Keep a400 Hz fallback profile if observed deadlines require it.

## Bounded acquisition and detection

Use one task as I2C/ADC owner. An ADXL data-ready ISR wakes it; the ISR performs no bus transactions, logging, allocation or packet encoding. Timestamp as near acquisition/interrupt as practical and document whether it represents read time or inferred sensor time. Do not use a one-millisecond FreeRTOS delay at100 Hz tick. A bounded recovery timeout must clear a latched data-ready condition and diagnose a dead sensor instead of waiting forever.

Read ADC and preserve raw counts independently of IMU errors. Return error codes instead of resetting on transient ADC failure. Keep the original empirical kilogram estimate only as a derived signal; it is not a validated impact-force calibration. Fixed raw records contain sensor integers, acquisition timing, ADC, freshness/validity and overrun indicators. Calibration offsets remain metadata rather than destructively overwriting those records.

An explicit detector states armed/contact/release-debounce. Retain1 kg onset and0.25 kg release as initial project-specific thresholds. Candidate release debounce5 ms and minimum retrigger20 ms are tunable initial values requiring recorded punch/chatter validation. Evaluate ADC validity before state transitions. A held contact generates one onset. A new valid release/recontact must not be blocked merely because the previous raw window is still being collected or transmitted.

Separate a small high-priority event queue from bounded raw capture storage. On detection, enqueue an immediate event and capture the ring position; complete pre/post data asynchronously. If raw slots are exhausted, account for a dropped capture without blocking acquisition or suppressing subsequent events. If events overflow, report that too. Preserve durations in milliseconds when changing ODR: the old200-sample ring and80-sample tail represented2 seconds and800 ms at100 Hz. Deliberately choose/document the new window rather than silently shrinking it eightfold.

A raw ring plus a fixed snapshot pool or bounded descriptors must have explicit ownership: acquisition owns mutable current data, transmitter owns completed immutable data. Never overwrite a snapshot being sent. Fixed storage and queue indexes avoid large FreeRTOS queue copies and allocation on the acquisition path.

## BLE and wire protocol

The user authorizes a clean protocol redesign. Implement one documented versioned protocol plus a reference decoder, avoiding an unnecessary duplicate legacy pipeline. Keep familiar device identity/service discovery when useful. Every message needs unambiguous type/version, session identity, ordering, and bounded length. Reset negotiation/session state on disconnect, unsubscribe and host reset; delayed packets or ACKs must not complete a newer session.

Immediate event messages should fit20 notification bytes, allowing ATT MTU23. Bulk records fragment according to the actual negotiated MTU minus3, with capture identity, offset and total length so missing/incomplete captures can be rejected. Include time units, endian convention, scaling, profile/ODR and per-sensor freshness in protocol metadata. Receiver validation must check lengths, bounds, sequence/session and record completeness. Lossless integer compression can follow correctness; physical raw values must round-trip exactly.

Request ATT MTU247, data length251 and2M PHY where supported, and low connection interval with zero slave latency. These are negotiated preferences; log actual values and tolerate rejected requests. A single transmitter owns all application sends and prioritizes immediate events between raw fragments. Bounded credits/congestion notifications or bounded retry deadlines replace arbitrary sleeps. NimBLE accepting a notification does not prove the application received it; define whether captures are best effort or application-acknowledged. [Espressif BLE connection guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/ble/get-started/ble-connection.html)

`ble_gatts_notify_custom` consumes its mbuf on both success and failure; retry by constructing a fresh mbuf from still-owned bytes, not by reusing or double freeing it. Synchronize connection handle, subscription, session and queue state; volatile globals alone are insufficient. GATT write callbacks validate the complete packet before publishing it to an application queue. [NimBLE GATT server reference](https://mynewt.apache.org/latest/network/ble_hs/ble_gatts.html)

## Evidence and acceptance checks

The baseline's80 post-trigger samples force approximately800 ms before enqueueing a punch. Its3 ms packet delay becomes zero ticks under the branch's100 Hz tick, so no fixed66 ms delay saving should be advertised. Device event enqueue latency and app receipt latency must be measured separately. The central's radio scheduling means7.5 ms requested interval is not a universal end-to-end guarantee.

Counters should cover acquisition samples and timing extrema, IRQ/timeouts, bus/ADC errors, fresh sensor samples, overruns/gaps, detected events, raw/event queue overflow, notification retry/failure and maximum queue depth. Log aggregate diagnostics infrequently. Measure memory and stack high-water marks during long congested transfers and reconnect loops.

Host tests should cover detector chatter/held force/quick successive contacts, timestamp wrap, ring boundary and full-pool behavior, all raw integer extrema, malformed control writes, MTU23/247 fragmentation, truncated/reordered captures and stale session rejection. Build against the documented ESP-IDF target; Doxygen should explain timing, ownership, units, return errors and callback/task context at public interfaces.

Hardware acceptance requires actual ESP32-C3/GY-85/FSR and a BLE central: verify sensor identities/register readback, I2C pullups/rise time,800 Hz timing under load, sensor failure recovery, ADC saturation, repeated punches and reconnect/congestion. Report median/p95/max trigger-to-host latency and capture completeness over a stated duration. Do not claim guaranteed losslessness or maximum sustainable throughput before this run.

`origin/App:app.py` is a matplotlib mock dataset visualizer, not a BLE receiver or wire decoder. Its existence provides no end-to-end compatibility evidence; a reference receiver and actual link test are required.
