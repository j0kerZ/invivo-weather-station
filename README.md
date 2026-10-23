# InVivo Weather Station

Weather stations on farms measure air temperature (LM92) and relative humidity (HDC3020) with a
Cortex-M3 MCU, keep a year of history in on-board flash, and publish to AWS IoT Core within one
second. A Lambda decodes the binary payload into DynamoDB, and a React app displays it.

No hardware is used: the station is emulated on a PC by the same portable C code that would run on
the MCU, with emulated sensors, flash and clock.

## Status

| Part | Status |
|---|---|
| Binary protocol v1 ([docs/PROTOCOL.md](docs/PROTOCOL.md)) | Done |
| C encoder/decoder with tests and shared test vectors | Done |
| Emulated station: sensors, aggregation, flash, MQTT | Not started |
| Cloud ingest (Lambda, DynamoDB, IaC) | Not started |
| Web frontend | Not started |

## Firmware: build and test

Requires CMake ≥ 3.20 and gcc or clang.

```sh
cmake -S firmware -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests run under AddressSanitizer and UndefinedBehaviorSanitizer, with every compiler warning
treated as an error.

| Path | Contents |
|---|---|
| `firmware/src`, `firmware/include` | Portable firmware code: no heap, no OS, no I/O |
| `firmware/test` | Unit tests |
| `firmware/tools/gen_vectors.c` | Generates `protocol/vectors-v1.json` |
| `protocol/vectors-v1.json` | Valid and malformed frames with expected decode results, shared by every decoder |

Copyright © 2026 Khanh Pham. All rights reserved.
