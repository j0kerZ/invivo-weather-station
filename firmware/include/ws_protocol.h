#ifndef WS_PROTOCOL_H
#define WS_PROTOCOL_H

/*
 * Weather station binary protocol v1. The specification is docs/PROTOCOL.md; section numbers
 * below refer to it.
 *
 * No dynamic memory: every function works on caller-provided buffers, so the same code runs on
 * the Cortex-M3 and in the PC emulator.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_PROTOCOL_VERSION 1u

/* §4 Record */
#define WS_RECORD_SIZE 21u
#define WS_RECORD_CRC_OFFSET 20u
#define WS_VALID_PCT_MAX 100u /* "% of samples valid": whole percent, rounded down */

#define WS_TEMP_MIN (-5500)          /* -55.00 °C */
#define WS_TEMP_MAX 15000            /* 150.00 °C */
#define WS_TEMP_INVALID INT16_MIN    /* no valid temperature in the window */
#define WS_RH_MAX 10000u             /* 100.00 %RH */
#define WS_RH_INVALID UINT16_MAX     /* no valid humidity in the window */

#define WS_FLAG_EVENT (1u << 0)
#define WS_FLAG_TIME_UNSYNCED (1u << 1)
#define WS_FLAG_TEMP_ERROR (1u << 2)
#define WS_FLAG_RH_ERROR (1u << 3)
#define WS_FLAG_HEATER_ON (1u << 4)

/* §5 Frame */
#define WS_FRAME_HEADER_SIZE 8u
#define WS_FRAME_CRC_SIZE 2u
#define WS_FRAME_MAX_RECORDS 200u
#define WS_FRAME_SIZE(count) (WS_FRAME_HEADER_SIZE + WS_RECORD_SIZE * (count) + WS_FRAME_CRC_SIZE)
#define WS_FRAME_MAX_SIZE WS_FRAME_SIZE(WS_FRAME_MAX_RECORDS)

#define WS_FRAME_FLAG_BACKFILL (1u << 0)

typedef enum {
    WS_OK = 0,
    WS_ERR_BUFFER,         /* caller's output buffer is missing or too small */
    WS_ERR_LENGTH,
    WS_ERR_VERSION,
    WS_ERR_FRAME_CRC,
    WS_ERR_COUNT,
    WS_ERR_RECORD_CRC,
    WS_ERR_RECORD_INVALID,
} ws_status_t;

/* A record as the firmware works with it. The CRC exists only in the encoded form. */
typedef struct {
    uint32_t ts;     /* Unix seconds */
    uint8_t tenths;  /* 0..9: tenths of a second after ts */
    int16_t t_mean;
    int16_t t_min;
    int16_t t_max;
    uint16_t rh_mean;
    uint16_t rh_min;
    uint16_t rh_max;
    uint8_t temp_valid_pct; /* % of the window's expected samples that were valid, 0..100 */
    uint8_t rh_valid_pct;
    uint8_t flags;
} ws_record_t;


/* A validated frame. `records` points into the buffer passed to ws_frame_decode. */
typedef struct {
    uint8_t version;
    uint8_t frame_flags;
    uint8_t count;
    uint32_t seq_first;
    const uint8_t *records;
} ws_frame_t;

/* Stable reason-code name, e.g. "RECORD_CRC". Used in logs and in the shared test vectors. */
const char *ws_status_name(ws_status_t status);

/* Checks the §4 rules: sentinels, ranges, min <= mean <= max, event shape. */
bool ws_record_is_valid(const ws_record_t *rec);

/* Refuses to encode a record that a decoder would reject. */
ws_status_t ws_record_encode(const ws_record_t *rec, uint8_t out[WS_RECORD_SIZE]);

ws_status_t ws_record_decode(const uint8_t in[WS_RECORD_SIZE], ws_record_t *rec);

/*
 * Builds a frame from records that are already encoded (for example read straight from flash).
 * Every record is decoded first, so a corrupt stored record is reported instead of transmitted.
 */
ws_status_t ws_frame_encode(uint8_t frame_flags, uint32_t seq_first, const uint8_t *records,
                            size_t count, uint8_t *out, size_t out_cap, size_t *out_len);

/*
 * Validates a whole frame in the §9 order. On WS_ERR_RECORD_CRC or WS_ERR_RECORD_INVALID,
 * *bad_record (if not NULL) is the index of the first failing record.
 */
ws_status_t ws_frame_decode(const uint8_t *buf, size_t len, ws_frame_t *frame,
                            size_t *bad_record);

#endif
