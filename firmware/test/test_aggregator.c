#include <string.h>

#include "check.h"
#include "ws_aggregator.h"

#define T0 1792742400u /* 2026-10-23T08:00:00Z, a whole minute */

/* Events effectively off, to test aggregation on its own. */
static const ws_event_config_t NO_EVENTS = {INT16_MIN, INT16_MAX, INT16_MAX, UINT16_MAX,
                                            UINT32_MAX};
static const ws_event_config_t DEFAULT_EVENTS = WS_EVENT_CONFIG_DEFAULT;

static ws_record_t out[WS_AGGREGATOR_MAX_OUT];

/* Feeds one sample; every record produced must be valid under the protocol rules. */
static size_t feed(ws_aggregator_t *agg, uint32_t ts, bool temp_ok, int16_t temp, bool rh_ok,
                   uint16_t rh, uint8_t flags)
{
    ws_sample_t s = {.ts = ts, .temp_ok = temp_ok, .temp = temp, .rh_ok = rh_ok, .rh = rh,
                     .flags = flags};
    size_t n = ws_aggregator_add(agg, &s, out);
    for (size_t i = 0; i < n; i++) {
        CHECK(ws_record_is_valid(&out[i]));
    }
    return n;
}

static size_t feed_ok(ws_aggregator_t *agg, uint32_t ts, int16_t temp, uint16_t rh)
{
    return feed(agg, ts, true, temp, true, rh, 0);
}

static void test_full_window(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    for (uint32_t i = 0; i < 30; i++) {
        CHECK(feed_ok(&agg, T0 + 2 * i, (int16_t)(1000 + i), (uint16_t)(5000 + 10 * i)) == 0);
    }
    /* The first sample of the next minute closes the window. */
    CHECK(feed_ok(&agg, T0 + 60, 0, 0) == 1);
    CHECK(out[0].ts == T0);
    CHECK(out[0].t_min == 1000 && out[0].t_max == 1029);
    CHECK(out[0].t_mean == 1015); /* 1014.5 rounds half away from zero */
    CHECK(out[0].rh_min == 5000 && out[0].rh_max == 5290 && out[0].rh_mean == 5145);
    CHECK(out[0].n_temp == 30 && out[0].n_rh == 30);
    CHECK(out[0].flags == 0);
}

static void test_negative_mean_rounding(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    feed_ok(&agg, T0, -1, 0);
    feed_ok(&agg, T0 + 2, -2, 0);
    feed_ok(&agg, T0 + 60, 0, 0);
    CHECK(out[0].t_mean == -2); /* -1.5 rounds away from zero, as +1.5 rounds to 2 */

    feed_ok(&agg, T0 + 62, -1, 0);
    feed_ok(&agg, T0 + 64, -2, 0);
    feed_ok(&agg, T0 + 120, 0, 0);
    CHECK(out[0].t_mean == -1); /* (0 - 1 - 2) / 3 = -1 */
}

static void test_sensor_failure(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    for (uint32_t i = 0; i < 30; i++) {
        feed(&agg, T0 + 2 * i, false, 0, true, 4000, WS_FLAG_TEMP_ERROR);
    }
    CHECK(feed_ok(&agg, T0 + 60, 0, 0) == 1);
    CHECK(out[0].n_temp == 0);
    CHECK(out[0].t_mean == WS_TEMP_INVALID && out[0].t_min == WS_TEMP_INVALID &&
          out[0].t_max == WS_TEMP_INVALID);
    CHECK(out[0].n_rh == 30 && out[0].rh_mean == 4000);
    CHECK(out[0].flags == WS_FLAG_TEMP_ERROR);

    /* One failed read in an otherwise good window: flagged, and counted out of the mean. */
    feed(&agg, T0 + 62, false, 0, true, 4000, WS_FLAG_TEMP_ERROR);
    feed_ok(&agg, T0 + 64, 2000, 4000);
    feed_ok(&agg, T0 + 120, 0, 0);
    CHECK(out[0].n_temp == 2 && out[0].t_mean == 1000); /* (0 + 2000) / 2 */
    CHECK(out[0].flags == WS_FLAG_TEMP_ERROR);
}

static void test_window_alignment_and_gaps(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* Booted mid-minute: the window still starts on the minute, with fewer samples. */
    for (uint32_t ts = T0 + 30; ts < T0 + 60; ts += 2) {
        feed_ok(&agg, ts, 1000, 5000);
    }
    CHECK(feed_ok(&agg, T0 + 60, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 && out[0].n_temp == 15);

    /* Five minutes without samples (e.g. a brown-out): no empty records are invented. */
    CHECK(feed_ok(&agg, T0 + 360, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 60 && out[0].n_temp == 1);

    /* Clock set backwards by a time sync: the current window is closed, not merged. */
    CHECK(feed_ok(&agg, T0 + 180, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 360);
    CHECK(feed_ok(&agg, T0 + 240, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 180);
}

static void test_sample_cap(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* A runaway timer delivering 70 samples in one minute must not exceed the protocol limit. */
    for (uint32_t i = 0; i < 70; i++) {
        feed_ok(&agg, T0 + i % 60, 1000, 5000);
    }
    CHECK(feed_ok(&agg, T0 + 60, 1000, 5000) == 1);
    CHECK(out[0].n_temp == WS_WINDOW_SAMPLES && out[0].n_rh == WS_WINDOW_SAMPLES);
}

static void test_flags_propagate(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    feed(&agg, T0, true, 1000, true, 5000, WS_FLAG_TIME_UNSYNCED);
    feed_ok(&agg, T0 + 2, 1000, 5000);
    feed_ok(&agg, T0 + 60, 1000, 5000);
    CHECK(out[0].flags == WS_FLAG_TIME_UNSYNCED);
}

static void test_frost_event_with_hysteresis_and_rate_limit(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &DEFAULT_EVENTS);

    CHECK(feed_ok(&agg, T0, 300, 8000) == 0);      /* 3.00 °C: nothing to report */
    CHECK(feed_ok(&agg, T0 + 2, 100, 8000) == 1);  /* 1.00 °C: frost begins */
    CHECK((out[0].flags & WS_FLAG_EVENT) && out[0].t_mean == 100 && out[0].ts == T0 + 2);
    CHECK(out[0].n_temp == 1 && out[0].n_rh == 1);

    /* Hysteresis, checked well clear of the rate limit so the limit cannot hide a wrong event */
    CHECK(feed_ok(&agg, T0 + 20, 120, 8000) == 0); /* above 1.00 but below 1.50: still frost */
    CHECK(feed_ok(&agg, T0 + 22, 90, 8000) == 0);  /* already reported */
    CHECK(feed_ok(&agg, T0 + 24, 149, 8000) == 0);
    CHECK(feed_ok(&agg, T0 + 30, 150, 8000) == 1); /* 1.50 °C: frost ends */
    CHECK((out[0].flags & WS_FLAG_EVENT) && out[0].t_mean == 150);

    /* Rate limit: frost returns 2 s later; reported once 10 s have passed, if still frosty */
    CHECK(feed_ok(&agg, T0 + 32, 100, 8000) == 0);
    CHECK(feed_ok(&agg, T0 + 38, 100, 8000) == 0);
    CHECK(feed_ok(&agg, T0 + 40, 100, 8000) == 1); /* re-evaluated, no pending queue needed */
    CHECK((out[0].flags & WS_FLAG_EVENT) && out[0].t_mean == 100 && out[0].ts == T0 + 40);
    CHECK(feed_ok(&agg, T0 + 50, 90, 8000) == 0);  /* still frost: nothing new */
}

static void test_change_events(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &DEFAULT_EVENTS);

    /* No published value yet: a large first reading is not a "change". */
    for (uint32_t i = 0; i < 30; i++) {
        CHECK(feed_ok(&agg, T0 + 2 * i, 2000, 6000) == 0);
    }

    /* A change at a minute boundary: the window closes first, then the event. */
    CHECK(feed_ok(&agg, T0 + 60, 2100, 6000) == 2);
    CHECK(!(out[0].flags & WS_FLAG_EVENT) && out[0].t_mean == 2000);
    CHECK((out[1].flags & WS_FLAG_EVENT) && out[1].t_mean == 2100);

    CHECK(feed_ok(&agg, T0 + 70, 2199, 6000) == 0); /* 0.99 °C from the event value: no */
    CHECK(feed_ok(&agg, T0 + 72, 2100, 6500) == 1); /* humidity +5.00 %RH */
    CHECK(out[0].rh_mean == 6500);
    CHECK(feed_ok(&agg, T0 + 74, 2100, 7000) == 0); /* rate-limited */

    /* Humidity sensor failed: an event carries the temperature and a humidity sentinel. */
    CHECK(feed(&agg, T0 + 90, true, 2300, false, 0, WS_FLAG_RH_ERROR) == 1);
    CHECK(out[0].n_rh == 0 && out[0].rh_mean == WS_RH_INVALID);
    CHECK(out[0].flags == (WS_FLAG_EVENT | WS_FLAG_RH_ERROR));
}

int main(void)
{
    test_full_window();
    test_negative_mean_rounding();
    test_sensor_failure();
    test_window_alignment_and_gaps();
    test_sample_cap();
    test_flags_propagate();
    test_frost_event_with_hysteresis_and_rate_limit();
    test_change_events();
    return check_report("test_aggregator");
}
