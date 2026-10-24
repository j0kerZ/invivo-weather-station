# Weather Station Binary Protocol — v1

The contract between the station firmware (C) and every component that decodes station data. If an
implementation and this document disagree, the implementation is wrong.

## 1. Conventions

- **Byte order:** little-endian for every multi-byte field. Fields are serialized one by one; no
  structure is copied as a block of memory.
- **Temperature:** `int16`, unit 0.01 °C, valid range −5500…15000. `0x8000` = no data.
- **Relative humidity:** `uint16`, unit 0.01 %RH, valid range 0…10000. `0xFFFF` = no data.
- **Time:** `uint32` Unix seconds (UTC), plus a `uint8` count of tenths of a second where present.
- **Station identity** is the AWS IoT thing name of the station's certificate. It is never part of
  the payload (§6).

## 2. Sampling and records

- The station samples both sensors 10 times per second.
- **Minute summary:** at the end of each wall-clock minute, one record with the lowest, mean and
  highest of that minute's valid samples. Windows start on whole minutes (`ts % 60 == 0`).
- **Change:** a record sent as soon as a sample differs from the last value sent by at least the
  change band (default 0.5 °C or 0.4 %RH). The band is then centred on the sample just sent.
- Minute summaries are stored on the station (§5) and numbered (§7). Changes are sent live only
  (§6).

## 3. Record (21 bytes)

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `ts` | Summary: start of the minute. Change: time of the sample |
| 4 | 1 | u8 | `tenths` | Tenths of a second after `ts`, 0…9 (0 for a summary) |
| 5 | 2 | i16 | `t_mean` | Mean temperature of the valid samples |
| 7 | 2 | i16 | `t_min` | Lowest temperature |
| 9 | 2 | i16 | `t_max` | Highest temperature |
| 11 | 2 | u16 | `rh_mean` | Mean relative humidity of the valid samples |
| 13 | 2 | u16 | `rh_min` | Lowest relative humidity |
| 15 | 2 | u16 | `rh_max` | Highest relative humidity |
| 17 | 1 | u8 | `temp_valid_pct` | % of the minute's 600 expected samples that were valid, 0…100, rounded down |
| 18 | 1 | u8 | `rh_valid_pct` | Same for humidity |
| 19 | 1 | u8 | `flags` | See below |
| 20 | 1 | u8 | `crc8` | CRC-8 over bytes 0…19 (§9) |

| Flag bit | Name | Meaning |
|---:|---|---|
| 0 | `EVENT` | The record is a change, not a minute summary |
| 1 | `TIME_UNSYNCED` | The station clock was not synchronised; `ts` is not trustworthy |
| 2 | `TEMP_ERROR` | At least one temperature read failed in the window |
| 3 | `RH_ERROR` | At least one humidity read failed in the window |
| 4 | `HEATER_ON` | The humidity sensor's heater was active; humidity may be biased |
| 5–7 | reserved | Sent as 0, ignored when received |

**Rules**

- `tenths` ≤ 9; each valid % ≤ 100.
- When a valid % is 0, that quantity's three values are its no-data value. Fewer than 6 valid
  samples out of 600 is 0%.
- Otherwise `min ≤ mean ≤ max`, and every value is inside its valid range.
- A change has `min == mean == max`; each valid % is 0 or 100, and at least one is 100.

## 4. Frame (MQTT payload)

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 1 | u8 | `version` | `1` |
| 1 | 1 | u8 | `frame_flags` | See below |
| 2 | 1 | u8 | `count` | Number of records, 1…200 |
| 3 | 1 | u8 | reserved | 0 |
| 4 | 4 | u32 | `seq_first` | Sequence number of the first record; record *i* is `seq_first + i`. 0 in a `LIVE_ONLY` frame |
| 8 | 21 × `count` | | records | §3 |
| 8 + 21 × `count` | 2 | u16 | `crc16` | CRC-16 over all preceding bytes of the frame (§9) |

| Frame flag bit | Name | Meaning |
|---:|---|---|
| 0 | `BACKFILL` | Records sent from storage after an outage |
| 1 | `LIVE_ONLY` | Change records, not stored on the station; `seq_first` is 0 |
| 2–7 | reserved | Sent as 0, ignored when received |

A one-record frame is 31 bytes; the largest frame is 4210 bytes. Records in one frame have
consecutive sequence numbers.

## 5. Storage on the station

SPI NOR flash, 4096-byte sectors. All sectors but the last two form a circular log of minute
summaries; the last two hold the upload cursor.

**Log sector**

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"WSR1"` |
| 4 | 4 | `seq_first` of the sector's first record |
| 8 | 4 | ring generation (number of times the log has wrapped) |
| 12 | 2 | reserved |
| 14 | 2 | CRC-16 over bytes 0…13 |
| 16 | 4074 | 194 records × 21 bytes |
| 4090 | 6 | unused |

- A summary is written to flash before it is published.
- A record slot that is not all `0xFF` is used. A slot that fails its CRC-8 is skipped; its
  sequence number is not reused.
- The sector after the one being filled is erased ahead of use. A full sector is followed by the
  next one, whose header is written first. The oldest sector is overwritten when the log wraps.
- A sector without a valid header is not part of the log.
- **Boot:** the valid header with the highest `seq_first` marks the newest sector; the first free
  slot after its last used slot gives the next sequence number.

**Upload cursor** (two sectors)

- Entries of 8 bytes: the value (u32) followed by its bitwise complement (u32). An entry whose two
  halves do not match is ignored.
- The cursor is the highest valid entry: every summary up to that sequence number has been
  acknowledged by AWS IoT Core.
- New entries are appended; when a sector is full, the other sector is erased and used.

## 6. Transport

- **Topic:** `invivo-ws/{stationId}/telemetry`, published by the station with MQTT 5, QoS 1 and a
  clean session. The client ID is the station's thing name.
- **Authorisation:** a station may connect only as its own thing and publish only to its own topic.
  The cloud takes `stationId` from the topic.
- **Minute summaries** are published as soon as they are stored. After a (re)connection, every
  stored summary after the upload cursor is sent again, in `BACKFILL` frames of up to 50 records,
  with at most 16 frames awaiting a PUBACK.
- **A PUBACK reason code below `0x80`** acknowledges the frame; the upload cursor moves over
  contiguous acknowledged summaries. A code of `0x80` or above is a refusal: the station
  reconnects and resends from the cursor.
- **Changes** are published in `LIVE_ONLY` frames while connected, and are not stored or resent.
- **Duplicates:** delivery is at-least-once. A receiver identifies a minute summary by
  `(stationId, seq)` and a change by `(stationId, ts, tenths)`.

## 7. Sequence numbers

- 32-bit, per station, strictly increasing, assigned to minute summaries only; changes carry 0.
- A summary's sequence number is its position in the log, so it continues across reboots.
- Sequence numbers follow publish order; readings are ordered by `ts` and `tenths`.

## 8. Decoder validation

A frame is accepted only if all checks pass, in this order; the first failure is reported:

| # | Check | Reason code |
|---:|---|---|
| 1 | Length is at least 10 bytes | `LENGTH` |
| 2 | `version == 1` | `VERSION` |
| 3 | Frame CRC-16 matches | `FRAME_CRC` |
| 4 | `1 ≤ count ≤ 200` | `COUNT` |
| 5 | Length equals `8 + 21 × count + 2` | `LENGTH` |
| 6 | Every record's CRC-8 matches | `RECORD_CRC` |
| 7 | Every record satisfies the rules of §3 | `RECORD_INVALID` |

Byte 0 is the version in every protocol version. Unknown flag bits are ignored. Every decoder is
tested against `protocol/vectors-v1.json`.

## 9. Checksums

| Use | Algorithm | Polynomial | Init | Reflected | Final XOR | Check value of `"123456789"` |
|---|---|---|---|---|---|---|
| Record | CRC-8/NRSC-5 | `0x31` | `0xFF` | no | `0x00` | `0xF7` |
| Frame, sector header | CRC-16/CCITT-FALSE | `0x1021` | `0xFFFF` | no | `0x0000` | `0x29B1` |

## 10. Worked example

A one-record frame with sequence number 1042: the minute starting 2026-10-23 08:00:00 UTC
(`ts = 1792742400`), temperature 12.10 / 12.34 / 12.61 °C (lowest / mean / highest), humidity
80.90 / 81.50 / 82.20 %RH, all samples valid, no flags.

```
01 00 01 00 12 04 00 00                          header: version 1, no flags, 1 record, seq 1042
00 14 db 6a                                      ts       1792742400
00                                               tenths   0
d2 04  ba 04  ed 04                              t_mean 1234, t_min 1210, t_max 1261
d6 1f  9a 1f  1c 20                              rh_mean 8150, rh_min 8090, rh_max 8220
64 64 00                                         100% valid, 100% valid, flags 0
61                                               crc8 0x61
04 a3                                            crc16 0xA304
```

## 11. Versioning

- A change to the meaning or position of any existing byte increments `version`.
- Reserved bits and bytes may gain a meaning within version 1 only if decoders that ignore them
  stay correct.
- Receivers accept every version in use by deployed stations.
