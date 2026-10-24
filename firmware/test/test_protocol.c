#include <string.h>

#include "check.h"
#include "test_util.h"

/* docs/PROTOCOL.md §10 */
static const ws_record_t EXAMPLE_RECORD = {
    .ts = 1792742400u, /* 2026-10-23 08:00:00 UTC */
    .t_mean = 1234, .t_min = 1210, .t_max = 1261,
    .rh_mean = 8150, .rh_min = 8090, .rh_max = 8220,
    .temp_valid_pct = 100, .rh_valid_pct = 100, .flags = 0,
};

static const uint8_t EXAMPLE_FRAME[] = {
    0x01, 0x00, 0x01, 0x00, 0x12, 0x04, 0x00, 0x00, /* v1, live, 1 record, seq 1042 */
    0x00, 0x14, 0xdb, 0x6a,                         /* ts 1792742400 */
    0x00,                                           /* tenths 0 */
    0xd2, 0x04, 0xba, 0x04, 0xed, 0x04,             /* temperature mean, min, max */
    0xd6, 0x1f, 0x9a, 0x1f, 0x1c, 0x20,             /* humidity mean, min, max */
    0x64, 0x64, 0x00,                               /* 100% valid, 100% valid, flags */
    0x61,                                           /* record CRC-8 */
    0x04, 0xa3,                                     /* frame CRC-16 */
};

static bool records_equal(const ws_record_t *a, const ws_record_t *b)
{
    return a->ts == b->ts && a->tenths == b->tenths && a->t_mean == b->t_mean &&
           a->t_min == b->t_min && a->t_max == b->t_max && a->rh_mean == b->rh_mean &&
           a->rh_min == b->rh_min && a->rh_max == b->rh_max &&
           a->temp_valid_pct == b->temp_valid_pct && a->rh_valid_pct == b->rh_valid_pct &&
           a->flags == b->flags;
}

static void test_crc_check_values(void)
{
    const uint8_t check[] = "123456789";
    CHECK(ws_crc8(check, 9) == 0xF7);
    CHECK(ws_crc16(check, 9) == 0x29B1);
}

static void test_example_frame_encodes_exactly(void)
{
    uint8_t record[WS_RECORD_SIZE];
    uint8_t frame[WS_FRAME_MAX_SIZE];
    size_t len = 0;

    CHECK_STATUS(ws_record_encode(&EXAMPLE_RECORD, record), WS_OK);
    CHECK_STATUS(ws_frame_encode(0, 1042, record, 1, frame, sizeof frame, &len), WS_OK);
    CHECK(len == sizeof EXAMPLE_FRAME);
    CHECK(memcmp(frame, EXAMPLE_FRAME, sizeof EXAMPLE_FRAME) == 0);
}

static void test_example_frame_decodes(void)
{
    ws_frame_t frame;
    ws_record_t rec;

    ws_status_t status = ws_frame_decode(EXAMPLE_FRAME, sizeof EXAMPLE_FRAME, &frame, NULL);
    CHECK_STATUS(status, WS_OK);
    if (status != WS_OK) {
        return; /* `frame` is not filled in on failure */
    }
    CHECK(frame.version == 1);
    CHECK(frame.frame_flags == 0);
    CHECK(frame.count == 1);
    CHECK(frame.seq_first == 1042);
    CHECK_STATUS(ws_record_decode(frame.records, &rec), WS_OK);
    CHECK(records_equal(&rec, &EXAMPLE_RECORD));
}

static void test_boundary_values_round_trip(void)
{
    const ws_record_t cases[] = {
        /* full sensor range */
        {.ts = 0, .t_mean = 0, .t_min = WS_TEMP_MIN, .t_max = WS_TEMP_MAX,
         .rh_mean = 5000, .rh_min = 0, .rh_max = WS_RH_MAX, .temp_valid_pct = 100,
         .rh_valid_pct = 1},
        /* largest timestamp and tenths, all defined flags (so a change event) */
        {.ts = UINT32_MAX, .tenths = 9, .t_mean = -1, .t_min = -1, .t_max = -1, .rh_mean = 1,
         .rh_min = 1, .rh_max = 1, .temp_valid_pct = 100, .rh_valid_pct = 100, .flags = 0x1F},
        /* temperature sensor failed for the whole window */
        {.ts = 100, .t_mean = WS_TEMP_INVALID, .t_min = WS_TEMP_INVALID,
         .t_max = WS_TEMP_INVALID, .rh_mean = 4000, .rh_min = 3900, .rh_max = 4100,
         .temp_valid_pct = 0, .rh_valid_pct = 97, .flags = WS_FLAG_TEMP_ERROR},
        /* both sensors failed: still recorded, the station itself is alive */
        {.ts = 100, .t_mean = WS_TEMP_INVALID, .t_min = WS_TEMP_INVALID,
         .t_max = WS_TEMP_INVALID, .rh_mean = WS_RH_INVALID, .rh_min = WS_RH_INVALID,
         .rh_max = WS_RH_INVALID, .flags = WS_FLAG_TEMP_ERROR | WS_FLAG_RH_ERROR},
        /* change event while the humidity sensor failed: temperature is enough */
        {.ts = 200, .tenths = 3, .t_mean = 100, .t_min = 100, .t_max = 100,
         .rh_mean = WS_RH_INVALID, .rh_min = WS_RH_INVALID, .rh_max = WS_RH_INVALID,
         .temp_valid_pct = 100, .rh_valid_pct = 0, .flags = WS_FLAG_EVENT},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t buf[WS_RECORD_SIZE];
        ws_record_t out;
        CHECK_STATUS(ws_record_encode(&cases[i], buf), WS_OK);
        CHECK_STATUS(ws_record_decode(buf, &out), WS_OK);
        CHECK(records_equal(&out, &cases[i]));
    }
}

static void test_invalid_records_are_refused(void)
{
    struct {
        const char *name;
        ws_record_t rec;
    } cases[] = {
        {"temperature above range", EXAMPLE_RECORD},
        {"temperature below range", EXAMPLE_RECORD},
        {"humidity above range", EXAMPLE_RECORD},
        {"min above mean", EXAMPLE_RECORD},
        {"mean above max", EXAMPLE_RECORD},
        {"more than 100% valid", EXAMPLE_RECORD},
        {"0% valid but values present", EXAMPLE_RECORD},
        {"valid but sentinel values", EXAMPLE_RECORD},
        {"event with spread", EXAMPLE_RECORD},
        {"event only partly valid", EXAMPLE_RECORD},
        {"event with no readings", EXAMPLE_RECORD},
        {"tenths above 9", EXAMPLE_RECORD},
    };
    cases[0].rec.t_max = WS_TEMP_MAX + 1;
    cases[1].rec.t_min = WS_TEMP_MIN - 1;
    cases[2].rec.rh_max = WS_RH_MAX + 1;
    cases[3].rec.t_min = 1300;
    cases[4].rec.rh_max = 8100;
    cases[5].rec.temp_valid_pct = WS_VALID_PCT_MAX + 1;
    cases[6].rec.rh_valid_pct = 0;
    cases[7].rec.t_mean = cases[7].rec.t_min = cases[7].rec.t_max = WS_TEMP_INVALID;
    cases[8].rec.flags = WS_FLAG_EVENT;
    cases[9].rec = (ws_record_t){.t_mean = 5, .t_min = 5, .t_max = 5, .rh_mean = 5,
                                 .rh_min = 5, .rh_max = 5, .temp_valid_pct = 50,
                                 .rh_valid_pct = 100, .flags = WS_FLAG_EVENT};
    cases[10].rec = (ws_record_t){.t_mean = WS_TEMP_INVALID, .t_min = WS_TEMP_INVALID,
                                  .t_max = WS_TEMP_INVALID, .rh_mean = WS_RH_INVALID,
                                  .rh_min = WS_RH_INVALID, .rh_max = WS_RH_INVALID,
                                  .flags = WS_FLAG_EVENT};
    cases[11].rec.tenths = 10;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t buf[WS_RECORD_SIZE];
        ws_record_t out;

        /* The encoder refuses it... */
        ws_status_t status = ws_record_encode(&cases[i].rec, buf);
        CHECK_STATUS(status, WS_ERR_RECORD_INVALID);
        if (status != WS_ERR_RECORD_INVALID) {
            fprintf(stderr, "  case: %s\n", cases[i].name);
        }

        /* ...and so does the decoder, if a buggy encoder produced it anyway. */
        pack_record_unchecked(&cases[i].rec, buf);
        CHECK_STATUS(ws_record_decode(buf, &out), WS_ERR_RECORD_INVALID);
    }
}

static void test_reserved_bits_are_ignored(void)
{
    uint8_t frame[sizeof EXAMPLE_FRAME];
    ws_frame_t decoded;

    memcpy(frame, EXAMPLE_FRAME, sizeof frame);
    frame[1] = 0xFE;      /* frame_flags: every reserved bit set */
    frame[8 + 19] = 0xE0; /* record flags: every reserved bit set */
    reseal_record(&frame[8]);
    reseal_frame(frame, sizeof frame);
    CHECK_STATUS(ws_frame_decode(frame, sizeof frame, &decoded, NULL), WS_OK);
}

static void test_malformed_frames_are_rejected(void)
{
    uint8_t frame[WS_FRAME_MAX_SIZE];
    ws_frame_t decoded;
    size_t bad = 99;

    CHECK_STATUS(ws_frame_decode(EXAMPLE_FRAME, 0, &decoded, NULL), WS_ERR_LENGTH);
    CHECK_STATUS(ws_frame_decode(EXAMPLE_FRAME, 9, &decoded, NULL), WS_ERR_LENGTH);

    memcpy(frame, EXAMPLE_FRAME, sizeof EXAMPLE_FRAME);
    frame[0] = 2;
    reseal_frame(frame, sizeof EXAMPLE_FRAME);
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, NULL), WS_ERR_VERSION);

    memcpy(frame, EXAMPLE_FRAME, sizeof EXAMPLE_FRAME);
    frame[sizeof EXAMPLE_FRAME - 1] ^= 0x01;
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, NULL),
                 WS_ERR_FRAME_CRC);

    /* Truncated in transit: the CRC no longer sits at the end, so it is reported as FRAME_CRC */
    CHECK_STATUS(ws_frame_decode(EXAMPLE_FRAME, sizeof EXAMPLE_FRAME - 1, &decoded, NULL),
                 WS_ERR_FRAME_CRC);

    memcpy(frame, EXAMPLE_FRAME, sizeof EXAMPLE_FRAME);
    frame[2] = 0;
    reseal_frame(frame, sizeof EXAMPLE_FRAME);
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, NULL), WS_ERR_COUNT);

    frame[2] = WS_FRAME_MAX_RECORDS + 1;
    reseal_frame(frame, sizeof EXAMPLE_FRAME);
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, NULL), WS_ERR_COUNT);

    /* Header claims two records, only one present, CRC consistent with what was sent */
    frame[2] = 2;
    reseal_frame(frame, sizeof EXAMPLE_FRAME);
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, NULL), WS_ERR_LENGTH);

    /* Corrupt stored record, sealed again at frame level: only the record CRC can catch it */
    memcpy(frame, EXAMPLE_FRAME, sizeof EXAMPLE_FRAME);
    frame[8 + 5] ^= 0x10;
    reseal_frame(frame, sizeof EXAMPLE_FRAME);
    CHECK_STATUS(ws_frame_decode(frame, sizeof EXAMPLE_FRAME, &decoded, &bad),
                 WS_ERR_RECORD_CRC);
    CHECK(bad == 0);
}

static void test_every_single_bit_flip_is_detected(void)
{
    uint8_t frame[sizeof EXAMPLE_FRAME];
    ws_frame_t decoded;

    for (size_t bit = 0; bit < sizeof frame * 8; bit++) {
        memcpy(frame, EXAMPLE_FRAME, sizeof frame);
        frame[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(ws_frame_decode(frame, sizeof frame, &decoded, NULL) != WS_OK);
    }
}

static void test_multi_record_frame_reports_bad_index(void)
{
    uint8_t records[3 * WS_RECORD_SIZE];
    uint8_t frame[WS_FRAME_MAX_SIZE];
    ws_frame_t decoded;
    size_t len = 0;
    size_t bad = 99;

    for (size_t i = 0; i < 3; i++) {
        ws_record_t rec = EXAMPLE_RECORD;
        rec.ts += (uint32_t)(60 * i);
        CHECK_STATUS(ws_record_encode(&rec, &records[i * WS_RECORD_SIZE]), WS_OK);
    }
    /* Sequence numbers above 2^31 catch signed-integer bugs in other decoders */
    CHECK_STATUS(ws_frame_encode(WS_FRAME_FLAG_BACKFILL, 0xFFFFFFF0u, records, 3, frame,
                                 sizeof frame, &len),
                 WS_OK);
    CHECK_STATUS(ws_frame_decode(frame, len, &decoded, NULL), WS_OK);
    CHECK(decoded.count == 3);
    CHECK(decoded.seq_first == 0xFFFFFFF0u);
    CHECK(decoded.frame_flags == WS_FRAME_FLAG_BACKFILL);

    frame[8 + 2 * WS_RECORD_SIZE + 4] ^= 0x01;
    reseal_frame(frame, len);
    CHECK_STATUS(ws_frame_decode(frame, len, &decoded, &bad), WS_ERR_RECORD_CRC);
    CHECK(bad == 2);
}

static void test_frame_encoder_guards(void)
{
    static uint8_t records[(WS_FRAME_MAX_RECORDS + 1) * WS_RECORD_SIZE];
    uint8_t frame[WS_FRAME_MAX_SIZE];
    size_t len = 0;

    for (size_t i = 0; i <= WS_FRAME_MAX_RECORDS; i++) {
        CHECK_STATUS(ws_record_encode(&EXAMPLE_RECORD, &records[i * WS_RECORD_SIZE]), WS_OK);
    }
    CHECK_STATUS(ws_frame_encode(0, 0, records, 0, frame, sizeof frame, &len), WS_ERR_COUNT);
    CHECK_STATUS(ws_frame_encode(0, 0, records, WS_FRAME_MAX_RECORDS + 1, frame, sizeof frame,
                                 &len),
                 WS_ERR_COUNT);
    CHECK_STATUS(ws_frame_encode(0, 0, records, WS_FRAME_MAX_RECORDS, frame, sizeof frame, &len),
                 WS_OK);
    CHECK(len == WS_FRAME_MAX_SIZE);
    CHECK(WS_FRAME_MAX_SIZE == 4210);
    CHECK_STATUS(ws_frame_encode(0, 0, records, 2, frame, WS_FRAME_SIZE(2) - 1, &len),
                 WS_ERR_BUFFER);
    CHECK_STATUS(ws_frame_encode(0, 0, records, 1, NULL, 0, &len), WS_ERR_BUFFER);

    /* A record corrupted in flash is reported, never transmitted */
    records[WS_RECORD_SIZE + 3] ^= 0x80;
    CHECK_STATUS(ws_frame_encode(0, 0, records, 2, frame, sizeof frame, &len),
                 WS_ERR_RECORD_CRC);
}

int main(void)
{
    test_crc_check_values();
    test_example_frame_encodes_exactly();
    test_example_frame_decodes();
    test_boundary_values_round_trip();
    test_invalid_records_are_refused();
    test_reserved_bits_are_ignored();
    test_malformed_frames_are_rejected();
    test_every_single_bit_flip_is_detected();
    test_multi_record_frame_reports_bad_index();
    test_frame_encoder_guards();
    return check_report("test_protocol");
}
