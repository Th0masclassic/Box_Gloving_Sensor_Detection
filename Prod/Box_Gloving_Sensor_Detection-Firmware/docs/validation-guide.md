# Firmware validation guide

## Completed offline validation

The exact final firmware source was built for ESP32-C3 with ESP-IDF 5.5.5 using
the defaults in this repository and the isolated configuration command below.
The resulting application binary was `0x87070` bytes. This is a build
validation only; no device was flashed.

```powershell
idf.py -B build -DIDF_TARGET=esp32c3 -DSDKCONFIG=build/sdkconfig.validation reconfigure
idf.py -B build -DIDF_TARGET=esp32c3 -DSDKCONFIG=build/sdkconfig.validation build
```

Using `build/sdkconfig.validation` avoids an older local top-level `sdkconfig`
silently overriding `sdkconfig.defaults`.

Portable tests are run by `tests/run_host_tests.ps1`:

- C11 with `-Wall -Wextra -Werror`: byte codec, HELLO validation, capture
  header/record round trips, a shared C/Python capture vector, timestamp-wrap
  scheduling, and detector thresholds/rearm behavior.
- Python standard-library tests: strict session handling, duplicate rejection,
  invalid-frame sequence preservation, partial-capture bounds and expiry,
  conflicting fragment rejection, MTU-23 fallback, MTU-247 reassembly of a
  full 601-record capture, shared vector decoding, and all 23 statistics.
- Doxygen generates the public API and protocol documentation from
  `docs/Doxyfile`.

These checks validate deterministic byte handling and bounded host models.
They do not simulate FreeRTOS scheduling, NimBLE controller delivery, radio
conditions, or sensor electronics.

## Before flashing

1. Confirm the target is ESP32-C3 and review `sdkconfig.defaults`: one NimBLE
   peripheral connection, ATT MTU 247, 1 kHz FreeRTOS tick, and 2M PHY support.
2. Inspect the board wiring before applying power: I2C SDA GPIO10, SCL GPIO9,
   ADXL345 interrupt GPIO6, LED GPIO8, and FSR on ADC1 channel 0.
3. Keep an instrumented serial console open. Boot logs report each sensor's
   identity/configuration status, static capture-storage use, negotiated MTU,
   data length, PHY, and connection interval.
4. Use the reference receiver to subscribe, clear old state, write HELLO, and
   save received output. Confirm its `HELLO_ACK` rates and capabilities before
   interpreting events.

## Hardware acceptance

Run these checks on the actual GY-85/FSR glove. Record firmware revision,
phone, central implementation, connection settings, and raw logs for each run.

| Scenario | Procedure | Pass evidence |
| --- | --- | --- |
| Boot identity | Power cycle with all sensors attached, then one sensor disconnected at a time. | Logs name the failing device; acquisition continues with validity flags; no reboot loop. |
| Quiet calibration | Hold the glove still through startup calibration. | Capture header has calibrated flag; offsets are plausible; invalid MAG samples do not make calibration false-positive. |
| Cadence | Stream idle samples and statistics for at least 60 seconds. | Gap, data-ready timeout/coalescing, acquisition-duration, and freshness counters quantify rather than hide misses. |
| Force threshold | Gradually load/unload the FSR and repeat impacts. | Event ADC counts are preserved; threshold/release/retrigger behavior matches the intended sensor calibration. |
| Burst impacts | Deliver impacts faster than one 750 ms raw window. | Every eligible trigger produces an EVENT; snapshot exhaustion is explicit in event flags/statistics; firmware stays responsive. |
| BLE stress | Stream full captures during disconnect/reconnect, notification unsubscribe, MTU fallback, and radio contention. | Old-session frames are dropped, slots recover, new HELLO creates a new sequence space, and receiver reports no falsely complete capture. |
| Latency | Use a shared-clock experiment, explicit RTT protocol, or a logic analyzer from FSR/trigger to receiver. | Report trigger-to-host latency only from that measurement; do not subtract ESP uptime from host `perf_counter`. |

## Interpreting diagnostic flags

Consult [Protocol v2](PROTOCOL_V2.md) for the wire mapping. In particular,
stale sensor values remain raw values from the last valid read, but retain a
stale flag. Saturation and magnetometer overflow are recorded instead of
clipped. A nonzero capture-drop or transmit-drop counter is a data-quality
result, not a condition to silently retry at the application layer.

## Release gate

Release only after the hardware matrix completes with documented acceptance
criteria. Preserve raw captures and statistics with each recorded issue so
force calibration, transport capacity, and timing can be traced to a specific
firmware build and hardware setup.
