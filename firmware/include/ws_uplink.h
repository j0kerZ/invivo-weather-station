#ifndef WS_UPLINK_H
#define WS_UPLINK_H

/*
 * Decides what the station publishes, independent of MQTT:
 *   - every new record immediately, as a one-record frame (the 1 s latency path);
 *   - after (re)connecting, everything not yet acknowledged, read back from flash in frames of up
 *     to WS_UPLINK_BACKFILL_RECORDS records with the BACKFILL flag.
 *
 * Flash is the retransmission buffer: nothing is kept in RAM for resending. A record counts as
 * delivered only when IoT Core acknowledges it (QoS 1 PUBACK). The upload cursor moves forward
 * over contiguous acknowledged records and is persisted, so the backlog survives a reboot.
 * Duplicates are possible (a resend after a lost PUBACK); the cloud de-duplicates by sequence.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ws_cursor.h"
#include "ws_store.h"

#define WS_UPLINK_MAX_INFLIGHT 16u
#define WS_UPLINK_BACKFILL_RECORDS 50u /* 1010-byte frames: one IoT Core billing unit is 5 KB */

/* Sends one frame. Returns false if it could not be sent; *packet_id identifies the PUBACK. */
typedef bool (*ws_publish_fn)(void *ctx, const uint8_t *frame, size_t len, uint16_t *packet_id);

typedef struct {
    uint16_t packet_id; /* 0 for a range that needs no PUBACK (records missing from flash) */
    uint32_t first;
    uint32_t last;
    bool acked;
} ws_inflight_t;

typedef struct {
    ws_store_t *store;
    ws_cursor_t *cursor;
    ws_publish_fn publish;
    void *publish_ctx;

    bool connected;
    bool needs_reconnect; /* set when IoT Core rejects a publish: retry from the cursor later */
    uint32_t acked_through; /* every record up to this sequence number is acknowledged */
    uint32_t backfill_next; /* next record to send from flash */
    uint32_t live_from;     /* records from here on are published as they are stored */
    ws_inflight_t inflight[WS_UPLINK_MAX_INFLIGHT];
    size_t n_inflight;

    /* Statistics */
    uint32_t live_frames, backfill_frames, backfill_records, rejected, missing, cursor_errors;
} ws_uplink_t;

/* Starts from the persisted cursor; without one, from the oldest record in flash. */
void ws_uplink_init(ws_uplink_t *u, ws_store_t *store, ws_cursor_t *cursor,
                    ws_publish_fn publish, void *publish_ctx);

void ws_uplink_connected(ws_uplink_t *u);
void ws_uplink_disconnected(ws_uplink_t *u);

/* Call after each record is stored in flash. Publishes it at once if connected. */
void ws_uplink_record_stored(ws_uplink_t *u, uint32_t seq, const uint8_t record[]);

/* Call regularly while connected: sends backlog frames while there is room in flight. */
void ws_uplink_poll(ws_uplink_t *u);

/* A PUBACK arrived. `accepted` is false for MQTT 5 reason codes >= 0x80 (e.g. not authorized). */
void ws_uplink_ack(ws_uplink_t *u, uint16_t packet_id, bool accepted);

#endif
