/*
 * Writes the shared protocol test vectors (JSON) to stdout.
 *
 * Every vector is checked against the C decoder before it is written, so the file is a record of
 * what the reference implementation does. Other decoders (the TypeScript Lambda) must produce the
 * same result for every vector. CTest fails if protocol/vectors-v1.json is out of date.
 */

#include <stdio.h>
#include <string.h>

#include "test_util.h"

static const ws_record_t BASE = {
    .ts = 1792742400u, /* 2026-10-23 08:00:00 UTC */
    .t_mean = 1234, .t_min = 1210, .t_max = 1261,
    .rh_mean = 8150, .rh_min = 8090, .rh_max = 8220,
    .temp_valid_pct = 100, .rh_valid_pct = 100, .flags = 0,
};

static int mismatches;
static int emitted;

static void print_record(const ws_record_t *r)
{
    printf("{\"ts\": %lu, \"tenths\": %u, \"tMean\": %d, \"tMin\": %d, \"tMax\": %d, "
           "\"rhMean\": %u, \"rhMin\": %u, \"rhMax\": %u, "
           "\"tempValidPct\": %u, \"rhValidPct\": %u, \"flags\": %u}",
           (unsigned long)r->ts, r->tenths, r->t_mean, r->t_min, r->t_max, r->rh_mean,
           r->rh_min, r->rh_max, r->temp_valid_pct, r->rh_valid_pct, r->flags);
}

static void emit(const char *name, const char *description, const uint8_t *buf, size_t len,
                 ws_status_t expect)
{
    ws_frame_t frame;
    size_t bad = 0;
    ws_status_t actual = ws_frame_decode(buf, len, &frame, &bad);

    if (actual != expect) {
        fprintf(stderr, "%s: decoder returned %s, expected %s\n", name, ws_status_name(actual),
                ws_status_name(expect));
        mismatches++;
    }

    printf("%s    {\n", emitted++ ? ",\n" : "");
    printf("      \"name\": \"%s\",\n", name);
    printf("      \"description\": \"%s\",\n", description);
    printf("      \"expect\": \"%s\",\n", ws_status_name(expect));
    if (actual == WS_OK) {
        printf("      \"frame\": {\"version\": %u, \"frameFlags\": %u, \"seqFirst\": %lu, "
               "\"records\": [\n",
               frame.version, frame.frame_flags, (unsigned long)frame.seq_first);
        for (size_t i = 0; i < frame.count; i++) {
            ws_record_t rec;
            ws_record_decode(&frame.records[i * WS_RECORD_SIZE], &rec);
            printf("        ");
            print_record(&rec);
            printf("%s\n", i + 1 < frame.count ? "," : "");
        }
        printf("      ]},\n");
    } else if (actual == WS_ERR_RECORD_CRC || actual == WS_ERR_RECORD_INVALID) {
        printf("      \"badRecord\": %zu,\n", bad);
    }
    printf("      \"hex\": \"");
    for (size_t i = 0; i < len; i++) {
        printf("%02x", buf[i]);
    }
    printf("\"\n    }");
}

/* Encodes valid records into a frame with the library encoder. */
static size_t build(uint8_t *out, uint8_t frame_flags, uint32_t seq_first,
                    const ws_record_t *recs, size_t count)
{
    static uint8_t encoded[WS_FRAME_MAX_RECORDS * WS_RECORD_SIZE];
    size_t len = 0;

    for (size_t i = 0; i < count; i++) {
        if (ws_record_encode(&recs[i], &encoded[i * WS_RECORD_SIZE]) != WS_OK) {
            fprintf(stderr, "invalid record %zu passed to build()\n", i);
            mismatches++;
        }
    }
    if (ws_frame_encode(frame_flags, seq_first, encoded, count, out, WS_FRAME_MAX_SIZE, &len) !=
        WS_OK) {
        fprintf(stderr, "ws_frame_encode failed in build()\n");
        mismatches++;
    }
    return len;
}

/* A one-record frame whose record content is invalid but correctly sealed. */
static size_t build_invalid(uint8_t *out, const ws_record_t *rec)
{
    size_t len = build(out, 0, 1, &BASE, 1);
    pack_record_unchecked(rec, &out[WS_FRAME_HEADER_SIZE]);
    reseal_frame(out, len);
    return len;
}

int main(void)
{
    static uint8_t buf[WS_FRAME_MAX_SIZE];
    static ws_record_t recs[WS_FRAME_MAX_RECORDS];
    ws_record_t rec;
    size_t len;

    printf("{\n  \"protocolVersion\": 1,\n");
    printf("  \"generatedBy\": \"firmware/tools/gen_vectors.c\",\n");
    printf("  \"units\": \"temperature 0.01 degC, humidity 0.01 %%RH, ts Unix seconds UTC\",\n");
    printf("  \"vectors\": [\n");

    /* ---- Valid frames ---- */

    len = build(buf, 0, 1042, &BASE, 1);
    emit("example_live", "PROTOCOL.md section 11: one live 1-minute aggregate", buf, len, WS_OK);

    rec = (ws_record_t){.ts = BASE.ts, .t_mean = 0, .t_min = WS_TEMP_MIN, .t_max = WS_TEMP_MAX,
                        .rh_mean = 5000, .rh_min = 0, .rh_max = WS_RH_MAX,
                        .temp_valid_pct = 100, .rh_valid_pct = 1};
    len = build(buf, 0, 1, &rec, 1);
    emit("range_limits", "Extremes of both sensor ranges; negative temperature", buf, len, WS_OK);

    rec = BASE;
    rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    rec.temp_valid_pct = 0;
    rec.flags = WS_FLAG_TEMP_ERROR;
    len = build(buf, 0, 2, &rec, 1);
    emit("temp_sensor_failed", "No valid temperature in the window: sentinel values", buf, len,
         WS_OK);

    rec = (ws_record_t){.ts = BASE.ts + 17, .tenths = 3, .t_mean = 95, .t_min = 95,
                        .t_max = 95, .rh_mean = 9310, .rh_min = 9310, .rh_max = 9310,
                        .temp_valid_pct = 100, .rh_valid_pct = 100, .flags = WS_FLAG_EVENT};
    len = build(buf, WS_FRAME_FLAG_LIVE_ONLY, 0, &rec, 1);
    emit("change_live_only", "Change at 0.3 s past the second: live-only frame, sequence 0", buf,
         len, WS_OK);

    for (size_t i = 0; i < 3; i++) {
        recs[i] = BASE;
        recs[i].ts = BASE.ts + (uint32_t)(60 * i);
        recs[i].flags = WS_FLAG_TIME_UNSYNCED;
    }
    len = build(buf, WS_FRAME_FLAG_BACKFILL, 0xFFFFFFF0u, recs, 3);
    emit("backfill_high_seq",
         "Backfill of 3 records; seqFirst above 2^31 catches signed 32-bit bugs", buf, len, WS_OK);

    len = build(buf, 0, 4, &BASE, 1);
    buf[1] = 0xFE;
    buf[WS_FRAME_HEADER_SIZE + 19] = 0xE0;
    reseal_record(&buf[WS_FRAME_HEADER_SIZE]);
    reseal_frame(buf, len);
    emit("reserved_bits_set", "Reserved flag bits must be ignored, not rejected", buf, len,
         WS_OK);

    for (size_t i = 0; i < WS_FRAME_MAX_RECORDS; i++) {
        recs[i] = BASE;
        recs[i].ts = BASE.ts + (uint32_t)(60 * i);
        recs[i].t_mean = (int16_t)(BASE.t_mean + (int16_t)i);
        recs[i].t_max = (int16_t)(BASE.t_max + (int16_t)i);
    }
    len = build(buf, WS_FRAME_FLAG_BACKFILL, 5, recs, WS_FRAME_MAX_RECORDS);
    emit("max_records", "Largest allowed frame: 200 records, 4210 bytes", buf, len, WS_OK);

    /* ---- Malformed frames ---- */

    len = build(buf, 0, 1042, &BASE, 1);
    emit("too_short", "Shorter than header + CRC", buf, 9, WS_ERR_LENGTH);

    emit("truncated", "Last byte lost in transit", buf, len - 1, WS_ERR_FRAME_CRC);

    buf[0] = 2;
    reseal_frame(buf, len);
    emit("unknown_version", "Version 2 is not understood by a v1 decoder", buf, len,
         WS_ERR_VERSION);

    len = build(buf, 0, 1042, &BASE, 1);
    buf[len - 1] ^= 0x01;
    emit("bad_frame_crc", "One bit flipped in the frame CRC", buf, len, WS_ERR_FRAME_CRC);

    len = build(buf, 0, 1042, &BASE, 1);
    buf[2] = 0;
    reseal_frame(buf, len);
    emit("count_zero", "Header says zero records", buf, len, WS_ERR_COUNT);

    buf[2] = WS_FRAME_MAX_RECORDS + 1;
    reseal_frame(buf, len);
    emit("count_too_large", "Header says 201 records", buf, len, WS_ERR_COUNT);

    buf[2] = 2;
    reseal_frame(buf, len);
    emit("count_length_mismatch", "Header says 2 records, 1 present", buf, len, WS_ERR_LENGTH);

    len = build(buf, 0, 1042, &BASE, 1);
    buf[WS_FRAME_HEADER_SIZE + 5] ^= 0x10;
    reseal_frame(buf, len);
    emit("bad_record_crc", "Record corrupted in flash, frame sealed afterwards", buf, len,
         WS_ERR_RECORD_CRC);

    rec = BASE;
    rec.t_min = 1300;
    len = build_invalid(buf, &rec);
    emit("min_above_mean", "t_min > t_mean", buf, len, WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.t_max = WS_TEMP_MAX + 1;
    len = build_invalid(buf, &rec);
    emit("temp_above_range", "150.01 degC is outside the LM92 range", buf, len,
         WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.rh_max = WS_RH_MAX + 1;
    len = build_invalid(buf, &rec);
    emit("rh_above_range", "100.01 %RH", buf, len, WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    len = build_invalid(buf, &rec);
    emit("sentinel_with_samples", "100% valid but values are the no-data sentinel", buf, len,
         WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.rh_valid_pct = 0;
    len = build_invalid(buf, &rec);
    emit("values_without_samples", "0% humidity valid but values present", buf, len,
         WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.temp_valid_pct = WS_VALID_PCT_MAX + 1;
    len = build_invalid(buf, &rec);
    emit("valid_above_100", "101% of samples valid", buf, len, WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.tenths = 10;
    len = build_invalid(buf, &rec);
    emit("tenths_above_9", "Tenths of a second must be 0 to 9", buf, len, WS_ERR_RECORD_INVALID);

    rec = BASE;
    rec.flags = WS_FLAG_EVENT;
    len = build_invalid(buf, &rec);
    emit("event_with_spread", "Event record where min, mean and max differ", buf, len,
         WS_ERR_RECORD_INVALID);

    for (size_t i = 0; i < 3; i++) {
        recs[i] = BASE;
        recs[i].ts = BASE.ts + (uint32_t)(60 * i);
    }
    len = build(buf, WS_FRAME_FLAG_BACKFILL, 100, recs, 3);
    rec = recs[2];
    rec.rh_min = 9000;
    pack_record_unchecked(&rec, &buf[WS_FRAME_HEADER_SIZE + 2 * WS_RECORD_SIZE]);
    reseal_frame(buf, len);
    emit("third_record_invalid", "Whole frame rejected; badRecord points at index 2", buf, len,
         WS_ERR_RECORD_INVALID);

    printf("\n  ]\n}\n");
    return mismatches == 0 ? 0 : 1;
}
