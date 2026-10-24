#ifndef WS_STATION_H
#define WS_STATION_H

/*
 * The station's main loop body. On the MCU, a timer interrupt wakes the CPU every
 * WS_SAMPLE_PERIOD_S seconds and calls ws_station_tick(); in the emulator, a loop does.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ws_aggregator.h"
#include "ws_protocol.h"

#define WS_SAMPLE_PERIOD_S 2u /* see PROTOCOL.md §2 */

/*
 * Receives every encoded record with its sequence number. This is where the record is written to
 * flash and published (steps 1d and 1e).
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

void ws_station_tick(ws_station_t *st, uint32_t now, bool time_synced);

#endif
