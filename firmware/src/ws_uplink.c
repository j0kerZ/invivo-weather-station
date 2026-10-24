#include "ws_uplink.h"

#include "ws_protocol.h"

void ws_uplink_init(ws_uplink_t *u, ws_store_t *store, ws_cursor_t *cursor,
                    ws_publish_fn publish, void *publish_ctx)
{
    *u = (ws_uplink_t){.store = store, .cursor = cursor, .publish = publish,
                       .publish_ctx = publish_ctx};

    /* Records older than the flash's oldest were overwritten before they could be uploaded. */
    u->acked_through = store->oldest_seq - 1;
    if (cursor->has_value && cursor->value > u->acked_through) {
        u->acked_through = cursor->value;
    }
    u->backfill_next = u->live_from = u->acked_through + 1;
}

static bool track(ws_uplink_t *u, uint16_t packet_id, uint32_t first, uint32_t last, bool acked)
{
    if (u->n_inflight == WS_UPLINK_MAX_INFLIGHT) {
        return false;
    }
    u->inflight[u->n_inflight++] =
        (ws_inflight_t){.packet_id = packet_id, .first = first, .last = last, .acked = acked};
    return true;
}

/* Moves the cursor over acknowledged ranges that continue it, and persists the result. */
static void advance(ws_uplink_t *u)
{
    uint32_t before = u->acked_through;
    bool moved;
    do {
        moved = false;
        for (size_t i = 0; i < u->n_inflight; i++) {
            ws_inflight_t *f = &u->inflight[i];
            if (f->acked && f->first <= u->acked_through + 1) {
                if (f->last > u->acked_through) {
                    u->acked_through = f->last;
                }
                u->inflight[i] = u->inflight[--u->n_inflight];
                moved = true;
                break;
            }
        }
    } while (moved);

    if (u->acked_through != before && !ws_cursor_save(u->cursor, u->acked_through)) {
        u->cursor_errors++; /* uploads continue; a reboot would just resend more */
    }
}

static bool send_frame(ws_uplink_t *u, uint8_t frame_flags, uint32_t first, const uint8_t *records,
                       size_t count)
{
    static uint8_t frame[WS_FRAME_SIZE(WS_UPLINK_BACKFILL_RECORDS)]; /* 1 KB: not on the stack */
    size_t len;
    uint16_t packet_id;

    if (u->n_inflight == WS_UPLINK_MAX_INFLIGHT ||
        ws_frame_encode(frame_flags, first, records, count, frame, sizeof frame, &len) != WS_OK ||
        !u->publish(u->publish_ctx, frame, len, &packet_id)) {
        return false;
    }
    return track(u, packet_id, first, first + (uint32_t)count - 1, false);
}

void ws_uplink_connected(ws_uplink_t *u)
{
    u->connected = true;
    u->needs_reconnect = false;
    u->backfill_next = u->acked_through + 1;
    u->live_from = ws_store_next_seq(u->store); /* everything before this is backlog */
}

void ws_uplink_disconnected(ws_uplink_t *u)
{
    /* PUBACKs for frames in flight will never arrive: they are resent after reconnecting. */
    u->connected = false;
    u->n_inflight = 0;
}

void ws_uplink_record_stored(ws_uplink_t *u, uint32_t seq, const uint8_t record[])
{
    if (!u->connected || seq < u->live_from) {
        return;
    }
    if (send_frame(u, 0, seq, record, 1)) {
        u->live_frames++;
    } else {
        /* Could not send now: make it part of the backlog, sent from flash by ws_uplink_poll. */
        u->live_from = seq + 1;
    }
}

void ws_uplink_change(ws_uplink_t *u, const uint8_t record[])
{
    uint8_t frame[WS_FRAME_SIZE(1)];
    size_t len;
    uint16_t packet_id; /* its PUBACK matches nothing in flight and is ignored */

    if (!u->connected ||
        ws_frame_encode(WS_FRAME_FLAG_LIVE_ONLY, 0, record, 1, frame, sizeof frame, &len) != WS_OK ||
        !u->publish(u->publish_ctx, frame, len, &packet_id)) {
        u->changes_dropped++;
        return;
    }
    u->change_frames++;
}

void ws_uplink_poll(ws_uplink_t *u)
{
    static uint8_t records[WS_UPLINK_BACKFILL_RECORDS * WS_RECORD_SIZE]; /* 1 KB: not on the stack */

    while (u->connected && u->backfill_next < u->live_from &&
           u->n_inflight < WS_UPLINK_MAX_INFLIGHT) {
        /* Collect consecutive readable records; a frame must not have gaps. */
        uint32_t first = u->backfill_next;
        size_t count = 0;
        while (count < WS_UPLINK_BACKFILL_RECORDS && first + count < u->live_from &&
               ws_store_read(u->store, first + (uint32_t)count, &records[count * WS_RECORD_SIZE]) ==
                   WS_STORE_OK) {
            count++;
        }

        if (count == 0) {
            /* Torn or corrupt in flash: nothing to send, so treat it as delivered. */
            u->missing++;
            track(u, 0, first, first, true);
            u->backfill_next++;
            advance(u);
            continue;
        }
        if (!send_frame(u, WS_FRAME_FLAG_BACKFILL, first, records, count)) {
            return; /* retried on the next poll */
        }
        u->backfill_frames++;
        u->backfill_records += (uint32_t)count;
        u->backfill_next = first + (uint32_t)count;
    }
}

void ws_uplink_ack(ws_uplink_t *u, uint16_t packet_id, bool accepted)
{
    for (size_t i = 0; i < u->n_inflight; i++) {
        ws_inflight_t *f = &u->inflight[i];
        if (f->packet_id != packet_id || f->acked) {
            continue;
        }
        if (accepted) {
            f->acked = true;
            advance(u);
        } else {
            /* Rejected by IoT Core: the cursor cannot pass this range. Reconnect and resend from
             * the cursor (with backoff), rather than resending in a tight loop. */
            u->rejected++;
            u->needs_reconnect = true;
            u->inflight[i] = u->inflight[--u->n_inflight];
        }
        return;
    }
}
