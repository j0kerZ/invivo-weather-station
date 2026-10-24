# Weather Station Binary Protocol — v1

This is the single contract shared by the firmware (C), the ingest Lambda, the tests and the
documentation. Anything that encodes or decodes weather data follows this document. If code and
this document disagree, that is a bug.

## 1. Design goals

| Goal | Consequence |
|---|---|
| Fit 365 days of history in on-board flash | Fixed-size 21-byte record, 1-minute summaries (§4) |
| Cloud receives new information within 1 s | Publish each record as soon as it is produced; publish threshold/change events immediately (§4) |
| Cortex-M3 has no FPU | All values are fixed-point integers; no floats on the device |
| Deterministic, portable encoding | Explicit little-endian, field-by-field serialization; never `memcpy` a C struct (padding and endianness are compiler-dependent) |
| Detect corruption | CRC-8 per record (flash bit-rot, torn writes), CRC-16 per frame (header + whole payload) |
| Detect loss and duplicates | Monotonic 32-bit sequence number per station |
| Evolve without breaking deployed devices | Version byte; decoders reject unknown versions instead of guessing |

## 2. Sampling

Assumption given for the exercise: the sensors respond to changes within milliseconds.

- **10 samples per second.** The sample interval is the latency budget: at most 100 ms of the 1 s.
- **One summary per minute** (lowest, mean, highest of every sample in the minute), the standard
  meteorological output for air temperature.
- **Report by exception:** a change record is sent as soon as a value moves at least 0.5 °C or
  0.4 %RH from the last value sent; the band then re-centres on the new value. No rate limit
  beyond the 100 ms sample interval.
- The station stores the minute summaries; change records are alerts for the 1 s delivery path.

## 3. Conventions

- **Byte order:** little-endian for every multi-byte field. Cortex-M3 parts are little-endian in
  practice and Node.js `Buffer` reads little-endian natively, so neither end byte-swaps.
- **Temperature:** `int16`, unit 0.01 °C. Valid range −5500…15000 (−55.00…150.00 °C, the LM92
  operating range). Sentinel `0x8000` (`INT16_MIN`) = no valid value.
- **Relative humidity:** `uint16`, unit 0.01 %RH. Valid range 0…10000. Sentinel `0xFFFF` = no
  valid value.
- **Time:** `uint32` Unix seconds, UTC. Unsigned 32-bit lasts until 2106.
- **Station identity is not in the payload.** It is the AWS IoT thing name, bound to the device's
  X.509 certificate and enforced by the IoT policy on the topic (§7). A payload cannot claim to be
  another station.

## 4. Record (21 bytes)

One record is either a **minute summary** or a **change**. The same layout is used in flash and
on the wire, so the device copies stored records into a frame without re-encoding them.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `ts` | Summary: start of the 60 s window. Change: time of the sample |
| 4 | 1 | u8 | `tenths` | Tenths of a second after `ts`, 0…9 (0 for a summary) |
| 5 | 2 | i16 | `t_mean` | Mean temperature over valid samples, 0.01 °C |
| 7 | 2 | i16 | `t_min` | Minimum temperature |
| 9 | 2 | i16 | `t_max` | Maximum temperature |
| 11 | 2 | u16 | `rh_mean` | Mean relative humidity, 0.01 %RH |
| 13 | 2 | u16 | `rh_min` | Minimum relative humidity |
| 15 | 2 | u16 | `rh_max` | Maximum relative humidity |
| 17 | 1 | u8 | `temp_valid_pct` | % of the window's 600 expected samples that were valid, 0…100, rounded down |
| 18 | 1 | u8 | `rh_valid_pct` | Same for humidity |
| 19 | 1 | u8 | `flags` | See below |
| 20 | 1 | u8 | `crc8` | CRC-8 over bytes 0…19 |

**Flags**

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `EVENT` | Record is a change, not a minute summary |
| 1 | `TIME_UNSYNCED` | Device clock has not been synchronised since boot; `ts` is not trustworthy |
| 2 | `TEMP_ERROR` | At least one temperature read failed during the window |
| 3 | `RH_ERROR` | At least one humidity read failed (I²C error or sensor CRC mismatch) |
| 4 | `HEATER_ON` | HDC3020 heater was active (condensation removal); humidity may be biased |
| 5–7 | reserved | Senders set 0; receivers ignore |

**Rules**

- `tenths` ≤ 9; each valid % ≤ 100.
- If a valid % is 0, that quantity's three fields are the no-data sentinel (`0x8000` for
  temperature, `0xFFFF` for humidity). Fewer than 6 of 600 samples rounds down to 0%.
- Otherwise `min ≤ mean ≤ max` and every value is inside its valid range.
- A change record has `min == mean == max`, each valid % is 0 or 100, and at least one is 100.

## 5. Frame (MQTT payload)

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 1 | u8 | `version` | `1` |
| 1 | 1 | u8 | `frame_flags` | Bit 0 `BACKFILL`: records come from storage after an outage. Others reserved (0) |
| 2 | 1 | u8 | `count` | Number of records, 1…200 |
| 3 | 1 | u8 | reserved | 0 |
| 4 | 4 | u32 | `seq_first` | Sequence number of the first record; record *i* has `seq_first + i` |
| 8 | 20 × `count` | | records | §4 |
| 8 + 20 × `count` | 2 | u16 | `crc16` | CRC-16 over every preceding byte of the frame |

- A live frame (one record) is **31 bytes**.
- The maximum frame is 4210 bytes, below AWS IoT Core's 5 KB billing unit ([AWS IoT Core pricing](https://aws.amazon.com/iot-core/pricing/)).
- Records in one frame have consecutive sequence numbers. A device never puts a gap inside a frame.

## 6. Storage layout and capacity

On-board SPI NOR flash is used as a circular log of 4096-byte sectors (the NOR erase unit):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"WSR1"` |
| 4 | 4 | `seq_first` of the first record in the sector |
| 8 | 4 | ring generation: how many times the log has wrapped (each sector is erased once per generation) |
| 12 | 2 | reserved |
| 14 | 2 | CRC-16 over bytes 0…13 |
| 16 | 4074 | 194 records × 21 bytes |

16 + 194 × 21 = 4090; the last 6 bytes of each sector are unused, so no record crosses a sector boundary.

| Sampling | Records per year | Bytes per year | Flash needed |
|---|---:|---:|---|
| 100 Hz raw (10-byte samples) | 3.15 billion | 31.5 GB | Not feasible |
| 1 Hz raw (10-byte samples) | 31.5 million | 315 MB | SD card |
| **1-minute summary (this design)** | **525,600** | **11.0 MB** | **16 MB NOR** |
| 10-minute aggregate | 52,560 | 1.05 MB | Still exceeds Cortex-M3 internal flash (≤ 1 MB, shared with firmware) |

A 16 MB part has 4096 sectors. The last 2 hold the upload cursor (§7). Of the remaining 4094,
one is always kept erased ahead, leaving 4093 × 194 = 794,042 records. A year of minute summaries
is 525,600 records, 2,710 sectors (66%).

**Write order and crash safety.** NOR flash erases a whole sector to `0xFF`, and programming can
only clear bits. Every step below leaves flash in a state the boot scan understands:

1. A record is written to flash **before** it is published, so nothing is published that a power
   cut could lose.
2. A record slot that is not all `0xFF` is used. A slot torn by a power cut fails its CRC-8, is
   skipped, and its sequence number is never reused.
3. When a sector fills, the next one is opened by writing its header. That sector was already
   erased ahead of time, so a record write never waits for an erase (tens to hundreds of ms on
   SPI NOR), which would eat into the 1 s latency budget.
4. A sector without a valid header (torn header write, or erased but not yet opened) is ignored.

**Boot scan.** Read every sector header (4096 × 16 bytes). The valid header with the highest
`seq_first` is the head; scanning its slots gives the next sequence number. Sequence numbers
therefore survive reboots without a separate counter in flash.

**Wear.** Each sector is erased once per trip around the ring: about once every 1.6 years at one
record per minute, far below the ~100,000 erase cycles NOR flash is typically rated for. The
ring is its own wear levelling.

**"Internal storage" is interpreted as on-board flash on the station's PCB**, not the MCU's own
flash, because a year of useful data does not fit in Cortex-M3 internal flash (typically ≤ 1 MB).

## 7. Transport

- **Topic:** `invivo-ws/{stationId}/telemetry`, MQTT QoS 1, published by the device.
- **Identity:** the IoT policy allows a device to publish only to
  `invivo-ws/${iot:Connection.Thing.ThingName}/telemetry`. The IoT rule takes `stationId` from the
  topic, never from the payload.
- **MQTT 5, QoS 1, clean session.** MQTT 5 PUBACKs carry a reason code: below `0x80` is success
  (`0x10` "no matching subscribers" included); `0x87` means not authorized. With MQTT 3.1.1 a refused
  publish would only show up as a dropped connection.
- **Delivery:** QoS 1 is at-least-once. The cloud must treat a repeated `(stationId, seq)` as a
  duplicate, not a new reading.
- **Flash is the retransmission buffer.** The MQTT client keeps no copies of sent frames. After
  any reconnect the device resends, from flash, everything after the upload cursor.
- **Upload cursor:** the highest sequence number up to which every record has a PUBACK, kept in two
  dedicated flash sectors. It survives reboots, so a station that loses power while offline still
  uploads its backlog. Packet IDs restart with each connection, so PUBACKs are only matched against
  publishes from the current connection.
- **Live first, then backlog:** new records are published as soon as they are stored; the backlog
  follows in frames of up to 50 records with `BACKFILL` set, at most 16 frames in flight.
- **PUBACK means IoT Core accepted the message, not that it was stored.** The device keeps every
  record in flash for 365 days regardless, so the cloud can request a resend of a sequence range
  after detecting a gap (future work: command topic `invivo-ws/{stationId}/cmd`).
- **Latency, measured** (emulator on a PC to `eu-north-1`): PUBACK round trip median 28 ms, worst
  32 ms; TLS + MQTT connect about 200 ms. Disabling Nagle's algorithm (`TCP_NODELAY`) cut the live
  round trip from 95 ms: TCP was holding the 30-byte frames back. A cellular link adds its own
  latency; LTE-M is typically tens to hundreds of ms.

## 8. Sequence numbers

- 32-bit, per station, strictly increasing, shared by aggregates and events.
- **Sequence order is publish order, not measurement order.** An aggregate is published when its
  minute ends, so an event at 13:00:34 gets a lower sequence number than the 13:00 aggregate
  published at 13:01:00. Consumers order readings by `ts` and use `seq` only for duplicate and gap
  detection.
- Survives reboots: on boot the device scans flash for the highest stored sequence number.
- At one record per minute plus events, 2³² does not wrap within the product's lifetime.

## 9. Decoder validation (both C and TypeScript)

A frame is rejected as a whole, with a reason code, if any of these fail. They are checked in this
order, so the reason code says what actually went wrong:

| # | Check | Reason code |
|---:|---|---|
| 1 | Length is at least header + CRC (10 bytes) | `LENGTH` |
| 2 | `version == 1` | `VERSION` |
| 3 | Frame CRC-16 (last two bytes) matches | `FRAME_CRC` |
| 4 | `1 ≤ count ≤ 200` | `COUNT` |
| 5 | Length equals `8 + 20 × count + 2` | `LENGTH` |
| 6 | Every record's CRC-8 matches | `RECORD_CRC` |
| 7 | Every record satisfies the rules in §4 | `RECORD_INVALID` |

The version is checked before the CRC because a future version may change everything after byte 0.
**Byte 0 is the version in every protocol version.** Checking the CRC before `count` means a
corrupted frame is reported as `FRAME_CRC`, not as a misleading `COUNT` or `LENGTH`.

Unknown `flags` and `frame_flags` bits are ignored. Timestamp plausibility (for example not far in
the future) is checked by the cloud, not the decoder, because it needs the receive time.

## 10. Checksums

| | Algorithm | Polynomial | Init | Reflected | Final XOR | Check (`"123456789"`) |
|---|---|---|---|---|---|---|
| Record | CRC-8/NRSC-5 | `0x31` | `0xFF` | no | `0x00` | `0xF7` |
| Frame and sector header | CRC-16/CCITT-FALSE | `0x1021` | `0xFFFF` | no | `0x0000` | `0x29B1` |

## 11. Worked example

A one-record live frame, sequence 1042, window starting 2026-10-23 08:00:00 UTC
(`ts = 1792742400`). Temperature 12.10 / 12.34 / 12.61 °C (min/mean/max), humidity
80.90 / 81.50 / 82.20 %RH, 100% of samples valid, no flags.

```
01 00 01 00 12 04 00 00                          header: v1, live, 1 record, seq 1042
00 14 db 6a                                      ts       1792742400
00                                               tenths   0
d2 04  ba 04  ed 04                              t_mean 1234, t_min 1210, t_max 1261
d6 1f  9a 1f  1c 20                              rh_mean 8150, rh_min 8090, rh_max 8220
64 64 00                                         100% valid, 100% valid, flags 0
61                                               crc8 0x61
04 a3                                            crc16 0xA304
```

## 12. Versioning

- A change that alters the meaning or position of any existing byte increments `version`.
- Reserved bits and bytes may be given a meaning within v1 only if old decoders can safely ignore
  them.
- The cloud must accept every version still deployed in the field; the device sends only one.

## Sources

- [TI LM92 datasheet](https://www.ti.com/lit/gpn/lm92): 13-bit two's complement, 0.0625 °C LSB,
  500 ms typical conversion time.
- [TI HDC3020 product page](https://www.ti.com/product/hdc3020): humidity response time about 4 s.
