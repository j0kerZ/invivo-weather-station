#include <string.h>

#include "check.h"
#include "ws_aggregator.h"

#define T0 1792742400u /* 2026-10-23T08:00:00Z, a whole minute */

/* Bands too wide to trigger: tests aggregation on its own. */
static const ws_event_config_t NO_EVENTS = {INT16_MAX, UINT16_MAX};
static const ws_event_config_t DEFAULT_EVENTS = WS_EVENT_CONFIG_DEFAULT;

static ws_record_t out[WS_AGGREGATOR_MAX_OUT];

/* Feeds one sample at T0 + tick tenths; every record produced must be valid under the
 * protocol rules. */
static size_t feed(ws_aggregator_t *agg, uint32_t tick, bool temp_ok, int16_t temp, bool rh_ok,
                   uint16_t rh, uint8_t flags)
{
    ws_sample_t s = {.ts = T0 + tick / 10, .tenths = (uint8_t)(tick % 10), .temp_ok = temp_ok,
                     .temp = temp, .rh_ok = rh_ok, .rh = rh, .flags = flags};
    size_t n = ws_aggregator_add(agg, &s, out);
    for (size_t i = 0; i < n; i++) {
        CHECK(ws_record_is_valid(&out[i]));
    }
    return n;
}

static size_t feed_ok(ws_aggregator_t *agg, uint32_t tick, int16_t temp, uint16_t rh)
{
    return feed(agg, tick, true, temp, true, rh, 0);
}

#define MINUTE 600u /* ticks in a minute at 10 Hz */

static void test_full_window(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    for (uint32_t i = 0; i < MINUTE; i++) {
        bool second_half = i >= MINUTE / 2;
        CHECK(feed_ok(&agg, i, second_half ? 1029 : 1000, second_half ? 5290 : 5000) == 0);
    }
    /* The first sample of the next minute closes the window. */
    CHECK(feed_ok(&agg, MINUTE, 0, 0) == 1);
    CHECK(out[0].ts == T0 && out[0].tenths == 0);
    CHECK(out[0].t_min == 1000 && out[0].t_max == 1029);
    CHECK(out[0].t_mean == 1015); /* 1014.5 rounds half away from zero */
    CHECK(out[0].rh_min == 5000 && out[0].rh_max == 5290 && out[0].rh_mean == 5145);
    CHECK(out[0].temp_valid_pct == 100 && out[0].rh_valid_pct == 100);
    CHECK(out[0].flags == 0);
}

static void test_negative_mean_rounding(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* Six samples: the fewest that make 1% of a minute (6 of 600). */
    for (uint32_t i = 0; i < 6; i++) {
        feed_ok(&agg, i, i < 3 ? -1 : -2, 0);
    }
    feed_ok(&agg, MINUTE, -1, 0);
    CHECK(out[0].temp_valid_pct == 1);
    CHECK(out[0].t_mean == -2); /* -1.5 rounds away from zero, as +1.5 rounds to 2 */

    for (uint32_t i = 1; i < 6; i++) {
        feed_ok(&agg, MINUTE + i, i < 4 ? -1 : -2, 0); /* -1 x4 (with the one above), -2 x2 */
    }
    feed_ok(&agg, 2 * MINUTE, 0, 0);
    CHECK(out[0].t_mean == -1); /* -8 / 6 = -1.33 */
}

static void test_valid_percentage(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* Temperature sensor failed for the whole minute: no data, flagged. */
    for (uint32_t i = 0; i < MINUTE; i++) {
        feed(&agg, i, false, 0, true, 4000, WS_FLAG_TEMP_ERROR);
    }
    CHECK(feed_ok(&agg, MINUTE, 2000, 4000) == 1);
    CHECK(out[0].temp_valid_pct == 0);
    CHECK(out[0].t_mean == WS_TEMP_INVALID && out[0].t_min == WS_TEMP_INVALID &&
          out[0].t_max == WS_TEMP_INVALID);
    CHECK(out[0].rh_valid_pct == 100 && out[0].rh_mean == 4000);
    CHECK(out[0].flags == WS_FLAG_TEMP_ERROR);

    /* Half the readings failed: 50% valid, mean of the valid ones only. */
    for (uint32_t i = 1; i < MINUTE; i++) {
        feed(&agg, MINUTE + i, i % 2 == 0, 2000, true, 4000, i % 2 ? WS_FLAG_TEMP_ERROR : 0);
    }
    feed_ok(&agg, 2 * MINUTE, 0, 0);
    CHECK(out[0].temp_valid_pct == 50 && out[0].t_mean == 2000);

    /* Rounded down: 599 of 600 is 99%, so 100% only ever means nothing missing. */
    for (uint32_t i = 1; i < MINUTE; i++) {
        feed_ok(&agg, 2 * MINUTE + i, 2000, 4000);
    }
    feed_ok(&agg, 3 * MINUTE, 0, 0);
    CHECK(out[0].temp_valid_pct == 100); /* 600 of 600: the closing sample above counts too */

    for (uint32_t i = 2; i < MINUTE; i++) {
        feed_ok(&agg, 3 * MINUTE + i, 2000, 4000); /* one sample missing */
    }
    feed_ok(&agg, 4 * MINUTE, 0, 0);
    CHECK(out[0].temp_valid_pct == 99);

    /* Fewer than 6 valid readings round down to 0%: reported as no data. */
    for (uint32_t i = 1; i < 5; i++) {
        feed_ok(&agg, 4 * MINUTE + i, 2000, 4000);
    }
    feed_ok(&agg, 5 * MINUTE, 0, 0);
    CHECK(out[0].temp_valid_pct == 0 && out[0].t_mean == WS_TEMP_INVALID);
}

static void test_window_alignment_and_gaps(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* Booted mid-minute: the window still starts on the minute, half valid. */
    for (uint32_t i = MINUTE / 2; i < MINUTE; i++) {
        feed_ok(&agg, i, 1000, 5000);
    }
    CHECK(feed_ok(&agg, MINUTE, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 && out[0].temp_valid_pct == 50);

    /* Five minutes without samples (e.g. a brown-out): no empty records are invented. */
    CHECK(feed_ok(&agg, 6 * MINUTE, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 60);

    /* Clock set backwards by a time sync: the current window is closed, not merged. */
    CHECK(feed_ok(&agg, 3 * MINUTE, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 360);
    CHECK(feed_ok(&agg, 4 * MINUTE, 1000, 5000) == 1);
    CHECK(out[0].ts == T0 + 180);
}

static void test_sample_cap(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    /* A runaway timer delivering 700 samples in one minute stays within the protocol. */
    for (uint32_t i = 0; i < 700; i++) {
        feed_ok(&agg, i % MINUTE, 1000, 5000);
    }
    CHECK(feed_ok(&agg, MINUTE, 1000, 5000) == 1);
    CHECK(out[0].temp_valid_pct == 100 && out[0].rh_valid_pct == 100);
}

static void test_flags_propagate(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &NO_EVENTS);

    feed(&agg, 0, true, 1000, true, 5000, WS_FLAG_TIME_UNSYNCED);
    feed_ok(&agg, 1, 1000, 5000);
    feed_ok(&agg, MINUTE, 1000, 5000);
    CHECK(out[0].flags == WS_FLAG_TIME_UNSYNCED);
}

static void test_change_events(void)
{
    ws_aggregator_t agg;
    ws_aggregator_init(&agg, &DEFAULT_EVENTS); /* 0.5 °C, 0.4 %RH */

    /* No published value yet: a large first reading is not a "change". */
    for (uint32_t i = 0; i < MINUTE; i++) {
        CHECK(feed_ok(&agg, i, 2000, 6000) == 0);
    }

    /* A change at a minute boundary: the minute closes first, then the change. */
    CHECK(feed_ok(&agg, MINUTE, 2050, 6000) == 2);
    CHECK(!(out[0].flags & WS_FLAG_EVENT) && out[0].t_mean == 2000);
    CHECK((out[1].flags & WS_FLAG_EVENT) && out[1].t_mean == 2050);
    CHECK(out[1].ts == T0 + 60 && out[1].tenths == 0);
    CHECK(out[1].temp_valid_pct == 100 && out[1].rh_valid_pct == 100);

    /* The band re-centres on the value sent: 0.49 °C from it is not a change. */
    CHECK(feed_ok(&agg, MINUTE + 1, 2099, 6000) == 0);
    /* No rate limit: the very next sample may be a change, 100 ms later. */
    CHECK(feed_ok(&agg, MINUTE + 2, 2100, 6000) == 1);
    CHECK(out[0].t_mean == 2100 && out[0].tenths == 2);
    CHECK(feed_ok(&agg, MINUTE + 3, 2100, 6039) == 0); /* humidity +0.39 %RH: no */
    CHECK(feed_ok(&agg, MINUTE + 4, 2100, 6040) == 1); /* humidity +0.40 %RH */
    CHECK(out[0].rh_mean == 6040);

    /* Humidity sensor failed: a change carries the temperature and a humidity sentinel. */
    CHECK(feed(&agg, MINUTE + 5, true, 2200, false, 0, WS_FLAG_RH_ERROR) == 1);
    CHECK(out[0].rh_valid_pct == 0 && out[0].rh_mean == WS_RH_INVALID);
    CHECK(out[0].flags == (WS_FLAG_EVENT | WS_FLAG_RH_ERROR));

    /* A falling value is a change too. */
    CHECK(feed_ok(&agg, MINUTE + 6, 2150, 6040) == 1);
}

int main(void)
{
    test_full_window();
    test_negative_mean_rounding();
    test_valid_percentage();
    test_window_alignment_and_gaps();
    test_sample_cap();
    test_flags_propagate();
    test_change_events();
    return check_report("test_aggregator");
}
