# InVivo Weather Station

Weather stations on farms measure air temperature (LM92) and relative humidity (HDC3020) with a
Cortex-M3 MCU, keep a year of history in on-board flash, and publish to AWS IoT Core within one
second. A Lambda decodes the binary payload into DynamoDB, and a React app displays it.

No hardware is used: the station is emulated on a PC by the same portable C code that would run on
the MCU, with emulated sensors, flash and clock.

## Status

| Part | Status |
|---|---|
| Binary protocol v1 ([docs/PROTOCOL.md](docs/PROTOCOL.md)) | Draft for review |
| Emulated station (C) | Not started |
| Cloud ingest (Lambda, DynamoDB, IaC) | Not started |
| Web frontend | Not started |

Copyright © 2026 Khanh Pham. All rights reserved.
