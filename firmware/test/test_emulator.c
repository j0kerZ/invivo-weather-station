/*
 * Whole-station tests: the real firmware (drivers, aggregator, station) running a full emulated day
 * against the emulated sensors on the emulated I2C bus.
 */

#include "check.h"
#include "emu.h"

#define DAY_START 1792713600u /* 2026-10-23T00:00:00Z */
#define MAX_RECORDS 2000u

typedef struct {
    uint32_t count;
    uint32_t seq[MAX_RECORDS];
    ws_record_t rec[MAX_RECORDS];
    uint32_t fingerprint; /* FNV-1a over every byte produced */
} capture_t;

static void capture(void *ctx, uint32_t seq, const uint8_t record[WS_RECORD_SIZE])
{
    capture_t *c = ctx;
    for (size_t i = 0; i < WS_RECORD_SIZE; i++) {
        c->fingerprint = (c->fingerprint ^ record[i]) * 16777619u;
    }
    if (c->count < MAX_RECORDS) {
        CHECK(ws_record_decode(record, &c->rec[c->count]) == WS_OK);
        c->seq[c->count++] = seq;
    }
}

static ws_station_t run_day(capture_t *c, emu_scenario_t scenario, uint64_t seed, double i2c_err,
                            double crc_err)
{
    const emu_config_t cfg = {.scenario = scenario, .seed = seed, .i2c_error_rate = i2c_err,
                              .crc_error_rate = crc_err};
    const ws_event_config_t events = WS_EVENT_CONFIG_DEFAULT;
    ws_station_t st;

    *c = (capture_t){.fingerprint = 2166136261u};
    emu_init(&cfg);
    ws_station_init(&st, 1, &events, capture, c);
    emu_run(&st, DAY_START, 86400);
    return st;
}

static bool is_event(const ws_record_t *r)
{
    return (r->flags & WS_FLAG_EVENT) != 0;
}

static void test_normal_day(void)
{
    static capture_t c;
    ws_station_t st = run_day(&c, EMU_SCENARIO_NORMAL, 1, 0.0, 0.0);
    uint32_t aggregates = 0, events = 0;

    for (uint32_t i = 0; i < c.count; i++) {
        const ws_record_t *r = &c.rec[i];
        CHECK(c.seq[i] == i + 1); /* contiguous sequence numbers from 1 */
        if (is_event(r)) {
            events++;
            /* Only the 13:00 rain shower changes fast enough to trigger an event */
            CHECK(r->ts >= DAY_START + 13 * 3600 && r->ts < DAY_START + 14 * 3600);
        } else {
            CHECK(r->ts == DAY_START + 60 * aggregates); /* one per minute, aligned */
            aggregates++;
            CHECK(r->n_temp == 30 && r->n_rh == 30); /* one sample every 2 s */
            CHECK(r->flags == 0);
            CHECK(r->t_min > 600 && r->t_max < 2200); /* 7..21 °C daily cycle, plus noise */
        }
    }
    CHECK(aggregates == 1440);
    CHECK(events >= 1);
    CHECK(st.temp_errors == 0 && st.rh_errors == 0 && st.encode_errors == 0);
}

static void test_frost_night_reports_frost_promptly(void)
{
    static capture_t c;
    run_day(&c, EMU_SCENARIO_FROST, 1, 0.0, 0.0);
    int32_t coldest = INT32_MAX;
    bool frost_reported = false, thaw_reported = false;

    for (uint32_t i = 0; i < c.count; i++) {
        const ws_record_t *r = &c.rec[i];
        if (!is_event(r)) {
            if (r->t_min < coldest) coldest = r->t_min;
            continue;
        }
        if (r->t_mean <= 100) frost_reported = true;
        if (frost_reported && r->t_mean >= 150) thaw_reported = true;
    }
    CHECK(coldest < 0);
    CHECK(frost_reported);
    CHECK(thaw_reported);
}

static void test_faulty_bus_still_produces_valid_records(void)
{
    static capture_t c;
    ws_station_t st = run_day(&c, EMU_SCENARIO_NORMAL, 7, 0.05, 0.02);
    uint32_t aggregates = 0, flagged = 0, partial = 0;

    for (uint32_t i = 0; i < c.count; i++) {
        const ws_record_t *r = &c.rec[i];
        if (is_event(r)) continue;
        aggregates++;
        if (r->flags & (WS_FLAG_TEMP_ERROR | WS_FLAG_RH_ERROR)) flagged++;
        if (r->n_temp < 30 || r->n_rh < 30) partial++;
    }
    /* Records were already checked as decodable in capture(). */
    CHECK(aggregates == 1440);
    CHECK(flagged > 0 && partial == flagged);
    CHECK(st.temp_errors > 0 && st.rh_errors > 0);
    CHECK(st.encode_errors == 0);
}

static void test_same_seed_same_output(void)
{
    static capture_t a, b;
    run_day(&a, EMU_SCENARIO_FROST, 42, 0.01, 0.01);
    run_day(&b, EMU_SCENARIO_FROST, 42, 0.01, 0.01);
    CHECK(a.count == b.count && a.fingerprint == b.fingerprint);
}

int main(void)
{
    test_normal_day();
    test_frost_night_reports_frost_promptly();
    test_faulty_bus_still_produces_valid_records();
    test_same_seed_same_output();
    return check_report("test_emulator");
}
