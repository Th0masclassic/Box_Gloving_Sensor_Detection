# Host tests

Run `./tests/run_host_tests.ps1` from the firmware directory. It compiles the
portable C protocol and detector test with strict warnings, then runs the
standard-library Python receiver tests.

`fixtures/v2_capture_vector.hex` is a shared 100-byte capture blob. The C
encoder must reproduce it byte-for-byte, while the Python receiver reassembles
and decodes it. The Python suite also covers the 20-byte ATT-payload fallback,
a normal 601-record capture with a 244-byte notification limit, session
changes, duplicate frames, sequence validation, partial-capture eviction, and
statistics chunking.
