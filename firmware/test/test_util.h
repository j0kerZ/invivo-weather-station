#ifndef TEST_UTIL_H
#define TEST_UTIL_H

/* Helpers for building deliberately broken frames. Shared by the tests and the vector generator. */

#include "ws_crc.h"
#include "ws_protocol.h"

/* Re-seals a hand-modified frame so a test reaches the check it targets, not the CRC check. */
static inline void reseal_frame(uint8_t *frame, size_t len)
{
    uint16_t crc = ws_crc16(frame, len - 2);
    frame[len - 2] = (uint8_t)crc;
    frame[len - 1] = (uint8_t)(crc >> 8);
}

static inline void reseal_record(uint8_t *record)
{
    record[WS_RECORD_CRC_OFFSET] = ws_crc8(record, WS_RECORD_CRC_OFFSET);
}

/* Packs any record, valid or not, with a correct CRC: simulates a buggy encoder. */
static inline void pack_record_unchecked(const ws_record_t *rec, uint8_t out[WS_RECORD_SIZE])
{
    const uint16_t fields[] = {
        (uint16_t)rec->t_mean, (uint16_t)rec->t_min, (uint16_t)rec->t_max,
        rec->rh_mean, rec->rh_min, rec->rh_max,
    };

    for (size_t i = 0; i < 4; i++) {
        out[i] = (uint8_t)(rec->ts >> (8 * i));
    }
    out[4] = rec->tenths;
    for (size_t i = 0; i < 6; i++) {
        out[5 + 2 * i] = (uint8_t)fields[i];
        out[6 + 2 * i] = (uint8_t)(fields[i] >> 8);
    }
    out[17] = rec->temp_valid_pct;
    out[18] = rec->rh_valid_pct;
    out[19] = rec->flags;
    reseal_record(out);
}

#endif
