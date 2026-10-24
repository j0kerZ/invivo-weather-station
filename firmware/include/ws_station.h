#ifndef WS_STATION_H
#define WS_STATION_H

/*
 * The station's main loop body. On the MCU, a timer interrupt wakes the CPU every
 * WS_SAMPLE_PERIOD_MS and calls ws_station_tick(); in the emulator, a loop does.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ws_aggregator.h"
#include "ws_protocol.h"

#define WS_SAMPLE_PERIOD_MS (1000u / WS_SAMPLE_HZ) /* 100 ms */

/*
 * Receives every encoded record: where it is stored and published. A minute summary comes with its
 * sequence number; a change record comes with 0, because changes are not stored.
 */
typedef void (*ws_record_sink_t)(void *ctx, uint32_t seq, const uint8_t record[WS_RECORD_SIZE]);

typedef struct {
    ws_aggregator_t agg;
    uint32_t next_seq;
    ws_record_sink_t sink;
    void *sink_ctx;

    /* Diagnostics */
    uint32_t temp_errors;
    uint32_t rh_errors;
    uint32_t encode_errors; /* should stay 0: would mean a firmware bug */
} ws_station_t;

void ws_station_init(ws_station_t *st, uint32_t next_seq, const ws_event_config_t *cfg,
                     ws_record_sink_t sink, void *sink_ctx);

/* One sample at Unix time `now` + `tenths` tenths of a second. */
void ws_station_tick(ws_station_t *st, uint32_t now, uint8_t tenths, bool time_synced);

#endif
