#include "ws_aggregator.h"

/* Integer mean, rounded half away from zero. */
static int32_t div_round(int32_t sum, int32_t n)
{
    return (sum >= 0 ? sum + n / 2 : sum - n / 2) / n;
}

static void open_window(ws_aggregator_t *agg, uint32_t start)
{
    agg->window_open = true;
    agg->window_start = start;
    agg->t_sum = 0;
    agg->n_temp = 0;
    agg->rh_sum = 0;
    agg->n_rh = 0;
    agg->window_flags = 0;
}

static ws_record_t close_window(ws_aggregator_t *agg)
{
    ws_record_t rec = {.ts = agg->window_start, .flags = agg->window_flags};

    rec.n_temp = agg->n_temp;
    if (agg->n_temp > 0) {
        rec.t_mean = (int16_t)div_round(agg->t_sum, agg->n_temp);
        rec.t_min = agg->t_min;
        rec.t_max = agg->t_max;
        agg->have_temp = true;
        agg->published_temp = rec.t_mean;
    } else {
        rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    }

    rec.n_rh = agg->n_rh;
    if (agg->n_rh > 0) {
        rec.rh_mean = (uint16_t)((agg->rh_sum + agg->n_rh / 2u) / agg->n_rh);
        rec.rh_min = agg->rh_min;
        rec.rh_max = agg->rh_max;
        agg->have_rh = true;
        agg->published_rh = rec.rh_mean;
    } else {
        rec.rh_mean = rec.rh_min = rec.rh_max = WS_RH_INVALID;
    }

    agg->window_open = false;
    return rec;
}

static void accumulate(ws_aggregator_t *agg, const ws_sample_t *s)
{
    agg->window_flags |= s->flags;

    /* The protocol allows at most 60 samples; extra samples (a timer fault) are dropped. */
    if (s->temp_ok && agg->n_temp < WS_WINDOW_SAMPLES) {
        if (agg->n_temp == 0 || s->temp < agg->t_min) agg->t_min = s->temp;
        if (agg->n_temp == 0 || s->temp > agg->t_max) agg->t_max = s->temp;
        agg->t_sum += s->temp;
        agg->n_temp++;
    }
    if (s->rh_ok && agg->n_rh < WS_WINDOW_SAMPLES) {
        if (agg->n_rh == 0 || s->rh < agg->rh_min) agg->rh_min = s->rh;
        if (agg->n_rh == 0 || s->rh > agg->rh_max) agg->rh_max = s->rh;
        agg->rh_sum += s->rh;
        agg->n_rh++;
    }
}

static uint32_t abs_diff(int32_t a, int32_t b)
{
    return (uint32_t)(a > b ? a - b : b - a);
}

/*
 * Level-triggered: compares the sample with the last *published* state, so a trigger suppressed
 * by the rate limit fires on a later sample if its condition still holds. No pending queue.
 */
static bool event_triggered(const ws_aggregator_t *agg, const ws_sample_t *s, bool *frost_after)
{
    bool fire = false;

    *frost_after = agg->frost;
    if (s->temp_ok) {
        if (!agg->frost && s->temp <= agg->cfg.frost_enter) {
            *frost_after = true;
            fire = true;
        } else if (agg->frost && s->temp >= agg->cfg.frost_exit) {
            *frost_after = false;
            fire = true;
        }
        if (agg->have_temp && abs_diff(s->temp, agg->published_temp) >= (uint32_t)agg->cfg.temp_step) {
            fire = true;
        }
    }
    if (s->rh_ok && agg->have_rh && abs_diff(s->rh, agg->published_rh) >= agg->cfg.rh_step) {
        fire = true;
    }
    return fire;
}

static bool rate_limited(const ws_aggregator_t *agg, uint32_t ts)
{
    /* A clock that moved backwards must not block events until it catches up. */
    return agg->any_event && ts >= agg->last_event_ts &&
           ts - agg->last_event_ts < agg->cfg.min_event_gap_s;
}

static ws_record_t make_event(ws_aggregator_t *agg, const ws_sample_t *s, bool frost_after)
{
    ws_record_t rec = {.ts = s->ts, .flags = (uint8_t)(WS_FLAG_EVENT | s->flags)};

    if (s->temp_ok) {
        rec.t_mean = rec.t_min = rec.t_max = s->temp;
        rec.n_temp = 1;
        agg->have_temp = true;
        agg->published_temp = s->temp;
    } else {
        rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    }
    if (s->rh_ok) {
        rec.rh_mean = rec.rh_min = rec.rh_max = s->rh;
        rec.n_rh = 1;
        agg->have_rh = true;
        agg->published_rh = s->rh;
    } else {
        rec.rh_mean = rec.rh_min = rec.rh_max = WS_RH_INVALID;
    }

    agg->frost = frost_after;
    agg->any_event = true;
    agg->last_event_ts = s->ts;
    return rec;
}

void ws_aggregator_init(ws_aggregator_t *agg, const ws_event_config_t *cfg)
{
    *agg = (ws_aggregator_t){.cfg = *cfg};
}

size_t ws_aggregator_add(ws_aggregator_t *agg, const ws_sample_t *sample,
                         ws_record_t out[WS_AGGREGATOR_MAX_OUT])
{
    size_t n_out = 0;
    uint32_t start = sample->ts - sample->ts % WS_WINDOW_S;

    /*
     * Windows are aligned to wall-clock minutes, so records from different stations line up.
     * Any change of window closes the current one: the next minute, a gap (no empty records are
     * invented), or the clock moving backwards after a time sync.
     */
    if (agg->window_open && start != agg->window_start) {
        out[n_out++] = close_window(agg);
    }
    if (!agg->window_open) {
        open_window(agg, start);
    }
    accumulate(agg, sample);

    bool frost_after;
    if (event_triggered(agg, sample, &frost_after) && !rate_limited(agg, sample->ts)) {
        out[n_out++] = make_event(agg, sample, frost_after);
    }
    return n_out;
}
