/*
 * Whole-station tests: the real firmware (drivers, aggregator, station) running emulated hours
 * against the emulated sensors on the emulated I2C bus, at 10 samples a second.
 */

#include "check.h"
#include "emu.h"

#define DAY_START 1792713600u /* 2026-10-23T00:00:00Z */
#define HOUR 3600u
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

static ws_station_t run(capture_t *c, emu_scenario_t scenario, uint64_t seed, double i2c_err,
                        double crc_err, uint32_t start, uint32_t duration_s)
{
    const emu_config_t cfg = {.scenario = scenario, .seed = seed, .i2c_error_rate = i2c_err,
                              .crc_error_rate = crc_err};
    const ws_event_config_t events = WS_EVENT_CONFIG_DEFAULT;
    ws_station_t st;

    *c = (capture_t){.fingerprint = 2166136261u};
    emu_init(&cfg);
    ws_station_init(&st, 1, &events, capture, c);
    emu_run(&st, start, duration_s);
    return st;
}

static bool is_change(const ws_record_t *r)
{
    return (r->flags & WS_FLAG_EVENT) != 0;
}

/* 12:00 to 14:00 of a normal day: steady weather, then the 13:00 rain shower. */
static void test_rain_shower(void)
{
    static capture_t c;
    const uint32_t start = DAY_START + 12 * HOUR;
    ws_station_t st = run(&c, EMU_SCENARIO_NORMAL, 1, 0.0, 0.0, start, 2 * HOUR);
    uint32_t minutes = 0, changes = 0;

    for (uint32_t i = 0; i < c.count; i++) {
        const ws_record_t *r = &c.rec[i];
        CHECK(c.seq[i] == i + 1); /* contiguous sequence numbers from 1 */
        if (is_change(r)) {
            changes++;
            /* Only the shower changes fast enough to leave the 0.5 °C / 0.4 %RH band */
            CHECK(r->ts >= DAY_START + 13 * HOUR && r->ts < DAY_START + 14 * HOUR);
            CHECK(r->temp_valid_pct == 100 && r->rh_valid_pct == 100);
        } else {
            CHECK(r->ts == start + 60 * minutes); /* one per minute, aligned */
            minutes++;
            CHECK(r->temp_valid_pct == 100 && r->rh_valid_pct == 100); /* 600 of 600 */
            CHECK(r->flags == 0);
        }
    }
    CHECK(minutes == 120);
    CHECK(changes >= 1);
    CHECK(st.temp_errors == 0 && st.rh_errors == 0 && st.encode_errors == 0);
}

/* A frost night: the minute summaries keep the sub-zero lows. */
static void test_frost_night(void)
{
    static capture_t c;
    run(&c, EMU_SCENARIO_FROST, 1, 0.0, 0.0, DAY_START + 2 * HOUR, 2 * HOUR);
    int32_t coldest = INT32_MAX;

    for (uint32_t i = 0; i < c.count; i++) {
        if (!is_change(&c.rec[i]) && c.rec[i].t_min < coldest) {
            coldest = c.rec[i].t_min;
        }
    }
    CHECK(coldest < 0);
}

static void test_faulty_bus_still_produces_valid_records(void)
{
    static capture_t c;
    ws_station_t st = run(&c, EMU_SCENARIO_NORMAL, 7, 0.05, 0.02, DAY_START, HOUR);
    uint32_t minutes = 0, flagged = 0, partial = 0;

    for (uint32_t i = 0; i < c.count; i++) {
        const ws_record_t *r = &c.rec[i];
        if (is_change(r)) continue;
        minutes++;
        if (r->flags & (WS_FLAG_TEMP_ERROR | WS_FLAG_RH_ERROR)) flagged++;
        if (r->temp_valid_pct < 100 || r->rh_valid_pct < 100) partial++;
    }
    /* Records were already checked as decodable in capture(). */
    CHECK(minutes == 60);
    CHECK(flagged > 0 && partial == flagged);
    CHECK(st.temp_errors > 0 && st.rh_errors > 0);
    CHECK(st.encode_errors == 0);
}

static void test_same_seed_same_output(void)
{
    static capture_t a, b;
    run(&a, EMU_SCENARIO_NORMAL, 42, 0.01, 0.01, DAY_START + 13 * HOUR, HOUR / 2);
    run(&b, EMU_SCENARIO_NORMAL, 42, 0.01, 0.01, DAY_START + 13 * HOUR, HOUR / 2);
    CHECK(a.count == b.count && a.fingerprint == b.fingerprint);
}

int main(void)
{
    test_rain_shower();
    test_frost_night();
    test_faulty_bus_still_produces_valid_records();
    test_same_seed_same_output();
    return check_report("test_emulator");
}
