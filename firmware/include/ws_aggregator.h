#ifndef WS_AGGREGATOR_H
#define WS_AGGREGATOR_H

/*
 * Turns individual samples into protocol records (docs/PROTOCOL.md §4):
 *   - one aggregate per wall-clock minute (min/mean/max of the valid samples), and
 *   - an immediate event when a trigger fires (frost threshold, large change), rate-limited.
 *
 * Pure logic: no hardware, no clock reads, no heap. Time arrives with each sample.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ws_protocol.h"

#define WS_WINDOW_S 60u
#define WS_AGGREGATOR_MAX_OUT 2u /* a closed window and an event, from one sample */

typedef struct {
    uint32_t ts;      /* Unix seconds */
    bool temp_ok;
    int16_t temp;     /* 0.01 °C, valid if temp_ok */
    bool rh_ok;
    uint16_t rh;      /* 0.01 %RH, valid if rh_ok */
    uint8_t flags;    /* WS_FLAG_TEMP_ERROR, WS_FLAG_RH_ERROR, WS_FLAG_TIME_UNSYNCED */
} ws_sample_t;

typedef struct {
    int16_t frost_enter;      /* event when temperature falls to or below this (0.01 °C) */
    int16_t frost_exit;       /* event when it rises back to or above this */
    int16_t temp_step;        /* event when temperature moves this far from the last published value */
    uint16_t rh_step;         /* same for humidity (0.01 %RH) */
    uint32_t min_event_gap_s; /* rate limit between events */
} ws_event_config_t;

#define WS_EVENT_CONFIG_DEFAULT {100, 150, 100, 500, 10}

typedef struct {
    ws_event_config_t cfg;

    /* Current window */
    bool window_open;
    uint32_t window_start;
    int32_t t_sum;
    int16_t t_min, t_max;
    uint8_t n_temp;
    uint32_t rh_sum;
    uint16_t rh_min, rh_max;
    uint8_t n_rh;
    uint8_t window_flags;

    /* Last published state, for event triggers */
    bool have_temp, have_rh;
    int16_t published_temp;
    uint16_t published_rh;
    bool frost;
    bool any_event;
    uint32_t last_event_ts;
} ws_aggregator_t;

void ws_aggregator_init(ws_aggregator_t *agg, const ws_event_config_t *cfg);

/*
 * Adds one sample. Writes 0, 1 or 2 records to `out`, in order: first the aggregate of a window
 * this sample closed, then an event this sample triggered. Returns the number written.
 */
size_t ws_aggregator_add(ws_aggregator_t *agg, const ws_sample_t *sample,
                         ws_record_t out[WS_AGGREGATOR_MAX_OUT]);

#endif
