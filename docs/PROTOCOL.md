# Weather Station Binary Protocol — v1

This is the single contract shared by the firmware (C), the ingest Lambda, the tests and the
documentation. Anything that encodes or decodes weather data follows this document. If code and
this document disagree, that is a bug.

## 1. Design goals

| Goal | Consequence |
|---|---|
| Fit 365 days of history in on-board flash | Fixed-size 20-byte record, 1-minute aggregation (§4) |
| Cloud receives new information within 1 s | Publish each record as soon as it is produced; publish threshold/change events immediately (§4) |
| Cortex-M3 has no FPU | All values are fixed-point integers; no floats on the device |
| Deterministic, portable encoding | Explicit little-endian, field-by-field serialization; never `memcpy` a C struct (padding and endianness are compiler-dependent) |
| Detect corruption | CRC-8 per record (flash bit-rot, torn writes), CRC-16 per frame (header + whole payload) |
| Detect loss and duplicates | Monotonic 32-bit sequence number per station |
| Evolve without breaking deployed devices | Version byte; decoders reject unknown versions instead of guessing |

## 2. Sampling decision

The interviewers clarified that the sensors "respond within a few milliseconds" and that the
sampling rate (up to ~100 Hz) is our choice. We choose **one sample every 2 s, aggregated to one
record per minute, plus immediate event records**:

- **The sensors do not produce new information at 100 Hz.** The LM92 datasheet gives a temperature
  conversion time of 500 ms typical and 1000 ms maximum, and states that reading faster than the
  conversion time prevents the data from being updated. Polling at 1 Hz would sit exactly on that
  limit, so we poll every 2 s. TI lists the HDC3020 humidity response time as about 4 s.
  The I²C read takes milliseconds, but the measurement itself does not.
- The protocol allows up to 60 samples per window (1 Hz), so a faster sensor can be used later
  without a protocol change.
- **Air temperature and humidity inside a radiation shield change over seconds to minutes.**
  Irrigation decisions are made on minutes-to-hours data.
- **Cost scales with message rate.** One message per second is 60× the IoT messages, Lambda
  invocations and DynamoDB writes of one per minute; 100 Hz is 6000×.
- **Storage:** raw 1 Hz data for a year is about 315 MB per station; 1-minute aggregates are about
  10.5 MB (§6).

The individual samples are not discarded silently: each minute keeps **min, mean and max** for both
quantities, so short extremes (for example a frost dip) survive aggregation.

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

## 4. Record (20 bytes)

One record is either a **1-minute aggregate** or an **event**. The same layout is used in flash and
on the wire, so the device copies stored records into a frame without re-encoding them.

| Offset | Size | Type | Field | Meaning |
|---:|---:|---|---|---|
| 0 | 4 | u32 | `ts` | Aggregate: start of the 60 s window. Event: time of the sample |
| 4 | 2 | i16 | `t_mean` | Mean temperature over valid samples, 0.01 °C |
| 6 | 2 | i16 | `t_min` | Minimum temperature |
| 8 | 2 | i16 | `t_max` | Maximum temperature |
| 10 | 2 | u16 | `rh_mean` | Mean relative humidity, 0.01 %RH |
| 12 | 2 | u16 | `rh_min` | Minimum relative humidity |
| 14 | 2 | u16 | `rh_max` | Maximum relative humidity |
| 16 | 1 | u8 | `n_temp` | Valid temperature samples in the window (0…60; 30 at the current 2 s rate; 1 for an event) |
| 17 | 1 | u8 | `n_rh` | Valid humidity samples in the window (0…60; 30 at the current 2 s rate; 1 for an event) |
| 18 | 1 | u8 | `flags` | See below |
| 19 | 1 | u8 | `crc8` | CRC-8 over bytes 0…18 |

**Flags**

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `EVENT` | Record is an event (threshold crossing or significant change), not an aggregate |
| 1 | `TIME_UNSYNCED` | Device clock has not been synchronised since boot; `ts` is not trustworthy |
| 2 | `TEMP_ERROR` | At least one temperature read failed during the window |
| 3 | `RH_ERROR` | At least one humidity read failed (I²C error or sensor CRC mismatch) |
| 4 | `HEATER_ON` | HDC3020 heater was active (condensation removal); humidity may be biased |
| 5–7 | reserved | Senders set 0; receivers ignore |

**Rules**

- If `n_temp == 0`, all three temperature fields are `0x8000`. Same for `n_rh == 0` and `0xFFFF`.
- Otherwise `min ≤ mean ≤ max` and every value is inside its valid range.
- An event record has `min == mean == max`, `n_temp, n_rh ≤ 1`, and at least one of them is 1.

**Event triggers (device configuration, not part of the wire format)**

- Frost: temperature falls to ≤ 1.00 °C, and later rises back to ≥ 1.50 °C. The 0.50 °C
  hysteresis stops a reading hovering around 1 °C from producing an event every sample.
- Temperature changes by ≥ 1.00 °C, or humidity by ≥ 5.00 %RH, since the last published value
  (the previous event, or the mean of the previous aggregate).
- At most one event per 10 s, so a noisy sensor cannot flood the link or the flash.
- Triggers are evaluated against the last *published* state on every sample. An event suppressed by
  the rate limit is therefore sent on the first sample after the limit expires, if the condition
  still holds, without a queue of pending events.

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

- A live frame (one record) is **30 bytes**. The equivalent JSON is about 150 bytes.
- The maximum frame is 4010 bytes, below AWS IoT Core's 5 KB billing increment.
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
| 16 | 4080 | 204 records × 20 bytes |

16 + 204 × 20 = 4096, so no record crosses a sector boundary.

| Sampling | Records per year | Bytes per year | Flash needed |
|---|---:|---:|---|
| 100 Hz raw (10-byte samples) | 3.15 billion | 31.5 GB | Not feasible |
| 1 Hz raw (10-byte samples) | 31.5 million | 315 MB | SD card |
| **1-minute aggregate (this design)** | **525,600** | **10.5 MB** | **16 MB NOR** |
| 10-minute aggregate | 52,560 | 1.05 MB | Still exceeds Cortex-M3 internal flash (≤ 1 MB, shared with firmware) |

A 16 MB part has 4096 sectors. The last 2 hold the upload cursor (§7). Of the remaining 4094,
one is always kept erased ahead, leaving 4093 × 204 = 834,972 records: 365 days of aggregates
plus room for about 840 events per day. An emulated year uses 2591 sectors.

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
80.90 / 81.50 / 82.20 %RH, 60 valid samples of each, no flags.

```
01 00 01 00 12 04 00 00                          header: v1, live, 1 record, seq 1042
00 14 db 6a                                      ts       1792742400
d2 04  ba 04  ed 04                              t_mean 1234, t_min 1210, t_max 1261
d6 1f  9a 1f  1c 20                              rh_mean 8150, rh_min 8090, rh_max 8220
3c 3c 00                                         n_temp 60, n_rh 60, flags 0
08                                               crc8 0x08
b7 23                                            crc16 0x23B7
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
