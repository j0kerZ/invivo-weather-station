#ifndef WS_AGGREGATOR_H
#define WS_AGGREGATOR_H

/*
 * Turns individual samples into protocol records (docs/PROTOCOL.md §4):
 *   - one summary per wall-clock minute (min/mean/max of the valid samples), and
 *   - a change event as soon as a value leaves the band around the last published value
 *     (report by exception).
 *
 * Pure logic: no hardware, no clock reads, no heap. Time arrives with each sample.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ws_protocol.h"

#define WS_SAMPLE_HZ 10u
#define WS_WINDOW_S 60u
#define WS_WINDOW_SAMPLES (WS_SAMPLE_HZ * WS_WINDOW_S) /* expected in a full minute: 600 */
#define WS_AGGREGATOR_MAX_OUT 2u /* a closed window and an event, from one sample */

typedef struct {
    uint32_t ts;      /* Unix seconds */
    uint8_t tenths;   /* 0..9 */
    bool temp_ok;
    int16_t temp;     /* 0.01 °C, valid if temp_ok */
    bool rh_ok;
    uint16_t rh;      /* 0.01 %RH, valid if rh_ok */
    uint8_t flags;    /* WS_FLAG_TEMP_ERROR, WS_FLAG_RH_ERROR, WS_FLAG_TIME_UNSYNCED */
} ws_sample_t;

/* The "changed" bands: a change event is sent when a value moves this far from the last
 * published value. Settings, to be tuned with field data. */
typedef struct {
    int16_t temp_band; /* 0.01 °C */
    uint16_t rh_band;  /* 0.01 %RH */
} ws_event_config_t;

#define WS_EVENT_CONFIG_DEFAULT {50, 40} /* 0.5 °C, 0.4 %RH */

typedef struct {
    ws_event_config_t cfg;

    /* Current window */
    bool window_open;
    uint32_t window_start;
    int32_t t_sum;
    int16_t t_min, t_max;
    uint16_t n_temp;
    uint32_t rh_sum;
    uint16_t rh_min, rh_max;
    uint16_t n_rh;
    uint8_t window_flags;

    /* Last published values, for change events */
    bool have_temp, have_rh;
    int16_t published_temp;
    uint16_t published_rh;
} ws_aggregator_t;

void ws_aggregator_init(ws_aggregator_t *agg, const ws_event_config_t *cfg);

/*
 * Adds one sample. Writes 0, 1 or 2 records to `out`, in order: first the summary of a window
 * this sample closed, then a change event this sample triggered. Returns the number written.
 */
size_t ws_aggregator_add(ws_aggregator_t *agg, const ws_sample_t *sample,
                         ws_record_t out[WS_AGGREGATOR_MAX_OUT]);

#endif
