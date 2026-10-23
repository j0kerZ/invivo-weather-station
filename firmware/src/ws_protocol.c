#include "ws_protocol.h"

#include <string.h>

#include "ws_crc.h"

/*
 * Explicit little-endian access, one byte at a time. This never depends on the host's byte order,
 * on struct padding, or on unaligned access (which faults on Cortex-M0 and is slow elsewhere).
 */
static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *ws_status_name(ws_status_t status)
{
    switch (status) {
    case WS_OK: return "OK";
    case WS_ERR_BUFFER: return "BUFFER";
    case WS_ERR_LENGTH: return "LENGTH";
    case WS_ERR_VERSION: return "VERSION";
    case WS_ERR_FRAME_CRC: return "FRAME_CRC";
    case WS_ERR_COUNT: return "COUNT";
    case WS_ERR_RECORD_CRC: return "RECORD_CRC";
    case WS_ERR_RECORD_INVALID: return "RECORD_INVALID";
    }
    return "UNKNOWN";
}

/*
 * One quantity's statistics (temperature or humidity), widened to int32 so the same check serves
 * the signed temperature and the unsigned humidity fields.
 */
static bool stats_are_valid(uint8_t n, int32_t mean, int32_t min, int32_t max, int32_t invalid,
                            int32_t lo, int32_t hi, bool is_event)
{
    if (n == 0) {
        return mean == invalid && min == invalid && max == invalid;
    }
    if (n > (is_event ? 1u : WS_WINDOW_SAMPLES)) {
        return false;
    }
    if (min < lo || max > hi || min > mean || mean > max) {
        return false;
    }
    return !is_event || (min == mean && mean == max);
}

bool ws_record_is_valid(const ws_record_t *rec)
{
    bool is_event = (rec->flags & WS_FLAG_EVENT) != 0;

    if (is_event && rec->n_temp == 0 && rec->n_rh == 0) {
        return false; /* an event must carry at least one reading */
    }
    return stats_are_valid(rec->n_temp, rec->t_mean, rec->t_min, rec->t_max, WS_TEMP_INVALID,
                           WS_TEMP_MIN, WS_TEMP_MAX, is_event) &&
           stats_are_valid(rec->n_rh, rec->rh_mean, rec->rh_min, rec->rh_max, WS_RH_INVALID, 0,
                           WS_RH_MAX, is_event);
}

ws_status_t ws_record_encode(const ws_record_t *rec, uint8_t out[WS_RECORD_SIZE])
{
    if (!ws_record_is_valid(rec)) {
        return WS_ERR_RECORD_INVALID;
    }
    put_u32le(&out[0], rec->ts);
    put_u16le(&out[4], (uint16_t)rec->t_mean);
    put_u16le(&out[6], (uint16_t)rec->t_min);
    put_u16le(&out[8], (uint16_t)rec->t_max);
    put_u16le(&out[10], rec->rh_mean);
    put_u16le(&out[12], rec->rh_min);
    put_u16le(&out[14], rec->rh_max);
    out[16] = rec->n_temp;
    out[17] = rec->n_rh;
    out[18] = rec->flags;
    out[WS_RECORD_CRC_OFFSET] = ws_crc8(out, WS_RECORD_CRC_OFFSET);
    return WS_OK;
}

ws_status_t ws_record_decode(const uint8_t in[WS_RECORD_SIZE], ws_record_t *rec)
{
    if (ws_crc8(in, WS_RECORD_CRC_OFFSET) != in[WS_RECORD_CRC_OFFSET]) {
        return WS_ERR_RECORD_CRC;
    }
    rec->ts = get_u32le(&in[0]);
    rec->t_mean = (int16_t)get_u16le(&in[4]);
    rec->t_min = (int16_t)get_u16le(&in[6]);
    rec->t_max = (int16_t)get_u16le(&in[8]);
    rec->rh_mean = get_u16le(&in[10]);
    rec->rh_min = get_u16le(&in[12]);
    rec->rh_max = get_u16le(&in[14]);
    rec->n_temp = in[16];
    rec->n_rh = in[17];
    rec->flags = in[18];
    return ws_record_is_valid(rec) ? WS_OK : WS_ERR_RECORD_INVALID;
}

ws_status_t ws_frame_encode(uint8_t frame_flags, uint32_t seq_first, const uint8_t *records,
                            size_t count, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (count == 0 || count > WS_FRAME_MAX_RECORDS) {
        return WS_ERR_COUNT;
    }
    size_t len = WS_FRAME_SIZE(count);
    if (out == NULL || out_cap < len) {
        return WS_ERR_BUFFER;
    }
    for (size_t i = 0; i < count; i++) {
        ws_record_t rec;
        ws_status_t status = ws_record_decode(&records[i * WS_RECORD_SIZE], &rec);
        if (status != WS_OK) {
            return status;
        }
    }

    out[0] = WS_PROTOCOL_VERSION;
    out[1] = frame_flags;
    out[2] = (uint8_t)count;
    out[3] = 0;
    put_u32le(&out[4], seq_first);
    memcpy(&out[WS_FRAME_HEADER_SIZE], records, count * WS_RECORD_SIZE);
    put_u16le(&out[len - WS_FRAME_CRC_SIZE], ws_crc16(out, len - WS_FRAME_CRC_SIZE));
    *out_len = len;
    return WS_OK;
}

ws_status_t ws_frame_decode(const uint8_t *buf, size_t len, ws_frame_t *frame,
                            size_t *bad_record)
{
    if (len < WS_FRAME_HEADER_SIZE + WS_FRAME_CRC_SIZE) {
        return WS_ERR_LENGTH;
    }
    if (buf[0] != WS_PROTOCOL_VERSION) {
        return WS_ERR_VERSION;
    }
    if (ws_crc16(buf, len - WS_FRAME_CRC_SIZE) != get_u16le(&buf[len - WS_FRAME_CRC_SIZE])) {
        return WS_ERR_FRAME_CRC;
    }
    uint8_t count = buf[2];
    if (count == 0 || count > WS_FRAME_MAX_RECORDS) {
        return WS_ERR_COUNT;
    }
    if (len != WS_FRAME_SIZE(count)) {
        return WS_ERR_LENGTH;
    }

    const uint8_t *records = &buf[WS_FRAME_HEADER_SIZE];
    for (size_t i = 0; i < count; i++) {
        ws_record_t rec;
        ws_status_t status = ws_record_decode(&records[i * WS_RECORD_SIZE], &rec);
        if (status != WS_OK) {
            if (bad_record != NULL) {
                *bad_record = i;
            }
            return status;
        }
    }

    frame->version = buf[0];
    frame->frame_flags = buf[1];
    frame->count = count;
    frame->seq_first = get_u32le(&buf[4]);
    frame->records = records;
    return WS_OK;
}
