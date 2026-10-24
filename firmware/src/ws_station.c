#include "ws_station.h"

#include "ws_sensors.h"

void ws_station_init(ws_station_t *st, uint32_t next_seq, const ws_event_config_t *cfg,
                     ws_record_sink_t sink, void *sink_ctx)
{
    *st = (ws_station_t){.next_seq = next_seq, .sink = sink, .sink_ctx = sink_ctx};
    ws_aggregator_init(&st->agg, cfg);
}

void ws_station_tick(ws_station_t *st, uint32_t now, uint8_t tenths, bool time_synced)
{
    ws_sample_t sample = {.ts = now, .tenths = tenths,
                          .flags = time_synced ? 0 : WS_FLAG_TIME_UNSYNCED};
    int16_t hdc_temp; /* HDC3020 temperature: unused for now, kept for a future cross-check */

    /* LM92 is the temperature source of record; the HDC3020 provides humidity. */
    sample.temp_ok = ws_lm92_read(WS_LM92_ADDR, &sample.temp) == WS_SENSOR_OK;
    if (!sample.temp_ok) {
        sample.flags |= WS_FLAG_TEMP_ERROR;
        st->temp_errors++;
    }
    sample.rh_ok = ws_hdc3020_read(WS_HDC3020_ADDR, &hdc_temp, &sample.rh) == WS_SENSOR_OK;
    if (!sample.rh_ok) {
        sample.flags |= WS_FLAG_RH_ERROR;
        st->rh_errors++;
    }

    ws_record_t records[WS_AGGREGATOR_MAX_OUT];
    size_t n = ws_aggregator_add(&st->agg, &sample, records);

    for (size_t i = 0; i < n; i++) {
        uint8_t encoded[WS_RECORD_SIZE];
        if (ws_record_encode(&records[i], encoded) != WS_OK) {
            st->encode_errors++;
            continue;
        }
        /* Only minute summaries are stored and numbered; a change is a live-only alert. */
        bool is_change = (records[i].flags & WS_FLAG_EVENT) != 0;
        st->sink(st->sink_ctx, is_change ? 0 : st->next_seq++, encoded);
    }
}
