# Smart Boxing Glove firmware

This ESP-IDF application targets an ESP32-C3 connected to a GY-85 board and
one FSR divider. It acquires native sensor counts, detects force-threshold
events, and publishes versioned binary BLE notifications for a companion app
or training-data receiver.

The authoritative wire format is [Protocol v2](docs/PROTOCOL_V2.md). The
architecture review is [here](docs/architecture-review.md), and the
hardware/release procedure is the [validation guide](docs/validation-guide.md).

## Hardware and runtime profile

| Item | Configuration |
| --- | --- |
| Board target | ESP32-C3 |
| I2C | SDA GPIO10, SCL GPIO9, 400 kHz |
| ADXL345 | address `0x53`, ±16 g full resolution, 800 Hz, DATA_READY GPIO6 |
| ITG-3200 | address `0x68`, ±2000 degrees/s, nominal 800 Hz |
| QMC5883L | address `0x0D`, ±2 G, nominal 200 Hz |
| FSR | ADC1 channel 0 |
| LED | GPIO8 |
| Raw context | 500 ms pre-trigger + trigger + 250 ms post-trigger; normally 601 records |
| BLE link | one NimBLE peripheral connection, preferred ATT MTU 247, 244-byte notification cap |

The ADXL345 interrupt is the acquisition cadence. The gyro and magnetometer
have independent clocks, so raw records include freshness, stale, saturation,
and overflow flags instead of claiming synchronized sampling. FSR ADC counts
are retained unmodified; the event's centi-kilogram value is an empirical
estimate retained from the original project.

An FSR trigger queues the immediate event before I2C sensor reads or raw
snapshot copying. Raw snapshots use two bounded slots, so a new event can still
be emitted when a prior capture is transmitting. Runtime statistics expose
sensor errors, queue/retry drops, timing gaps, acquisition work duration, and
freshness counts.

## Build

Use ESP-IDF 5.5.5 or a compatible 5.4/5.5 installation with ESP32-C3 support.
From this directory in PowerShell:

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
. C:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1
idf.py -B build -DIDF_TARGET=esp32c3 -DSDKCONFIG=build/sdkconfig.validation reconfigure
idf.py -B build -DIDF_TARGET=esp32c3 -DSDKCONFIG=build/sdkconfig.validation build
```

`sdkconfig.defaults` selects ESP32-C3, a 1 kHz FreeRTOS tick for sub-10 ms
timeouts, NimBLE peripheral-only operation, one connection, ATT MTU 247, and
2M PHY support. The latest clean ESP-IDF 5.5.5 C3 build completed successfully
with a `0x87070` application binary. The explicit `SDKCONFIG` path prevents an
old local top-level `sdkconfig` from overriding those defaults. Building does
not flash a device.

## Host validation

Run the portable codec, detector, and receiver tests:

```powershell
.\tests\run_host_tests.ps1
```

The test suite uses strict C11 warnings and Python's standard-library
`unittest`; it does not require hardware. It covers the byte codec, HELLO
validation, timestamp-wrap scheduling, detector rearm behavior, a shared C and
Python capture vector, ATT-MTU 23 fragmentation, MTU-247 601-record
reassembly, session transitions, duplicate rejection, bounded partial capture
retention, and telemetry chunk validation.

Generate API documentation from this directory with:

```powershell
& 'C:\Users\tomas\Documents\ChatGPT\Smart box Boxing Glove\.tools\doxygen\doxygen.exe' docs\Doxyfile
```

The generated HTML is written to `docs/generated/html/index.html`.

## Reference receiver

`tools/glove_receiver.py` strictly decodes offline hex frames and can collect
BLE notifications with the optional [`bleak`](https://github.com/hbldh/bleak)
package:

```powershell
python -m pip install bleak
python tools\glove_receiver.py --name SMART_BOXING_GLOVE --duration 30 --output captures.json
```

It subscribes, clears old receiver state, writes HELLO with all v2
capabilities, validates sessions and fragments, and records events and complete
raw captures. Its timing output is receiver-side notification inter-arrival and
completion data only. Device uptime and host clocks are not synchronized, so it
does not report trigger-to-host latency.

## Hardware validation still required

No firmware was flashed as part of this work. Validate timing under BLE load,
sensor-fault recovery, force calibration, repeated impacts, reconnects, and
real trigger-to-host latency on the intended glove hardware before release.
