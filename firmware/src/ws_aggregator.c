#include "ws_aggregator.h"

/* Integer mean, rounded half away from zero. */
static int32_t div_round(int32_t sum, int32_t n)
{
    return (sum >= 0 ? sum + n / 2 : sum - n / 2) / n;
}

/* % of the window's expected samples that were valid: whole percent, rounded down, so 100%
 * only ever means nothing was missing. */
static uint8_t valid_pct(uint16_t n)
{
    return (uint8_t)((WS_VALID_PCT_MAX * n) / WS_WINDOW_SAMPLES);
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
    ws_record_t rec = {.ts = agg->window_start, .tenths = 0, .flags = agg->window_flags};

    /* Under 1% valid (fewer than 6 of 600) rounds down to 0%: reported as no data. */
    rec.temp_valid_pct = valid_pct(agg->n_temp);
    if (rec.temp_valid_pct > 0) {
        rec.t_mean = (int16_t)div_round(agg->t_sum, agg->n_temp);
        rec.t_min = agg->t_min;
        rec.t_max = agg->t_max;
        agg->have_temp = true;
        agg->published_temp = rec.t_mean;
    } else {
        rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    }

    rec.rh_valid_pct = valid_pct(agg->n_rh);
    if (rec.rh_valid_pct > 0) {
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

    /* At most the expected number of samples; extra ones (a timer fault) are dropped. */
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

/* Report by exception: has a value left the band around the last published value? */
static bool changed(const ws_aggregator_t *agg, const ws_sample_t *s)
{
    if (s->temp_ok && agg->have_temp &&
        abs_diff(s->temp, agg->published_temp) >= (uint32_t)agg->cfg.temp_band) {
        return true;
    }
    return s->rh_ok && agg->have_rh && abs_diff(s->rh, agg->published_rh) >= agg->cfg.rh_band;
}

static ws_record_t make_event(ws_aggregator_t *agg, const ws_sample_t *s)
{
    ws_record_t rec = {.ts = s->ts, .tenths = s->tenths,
                       .flags = (uint8_t)(WS_FLAG_EVENT | s->flags)};

    if (s->temp_ok) {
        rec.t_mean = rec.t_min = rec.t_max = s->temp;
        rec.temp_valid_pct = WS_VALID_PCT_MAX;
        agg->published_temp = s->temp; /* re-centre the band on the value just sent */
    } else {
        rec.t_mean = rec.t_min = rec.t_max = WS_TEMP_INVALID;
    }
    if (s->rh_ok) {
        rec.rh_mean = rec.rh_min = rec.rh_max = s->rh;
        rec.rh_valid_pct = WS_VALID_PCT_MAX;
        agg->published_rh = s->rh;
    } else {
        rec.rh_mean = rec.rh_min = rec.rh_max = WS_RH_INVALID;
    }
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

    if (changed(agg, sample)) {
        out[n_out++] = make_event(agg, sample);
    }
    return n_out;
}
