/*
 * Upload logic tests: real flash log and cursor on emulated flash, a fake publisher in place of
 * MQTT. The fake decides whether a publish succeeds and records every frame sent.
 */

#include <string.h>

#include "check.h"
#include "emu.h"
#include "ws_uplink.h"

#define SECTORS 8u
#define STORE_SECTORS (SECTORS - WS_CURSOR_SECTORS)
#define T0 1792713600u
#define MAX_SENT 4096u

typedef struct {
    uint16_t packet_id;
    uint8_t flags;
    uint32_t first;
    uint32_t count;
} sent_t;

static struct {
    bool accept;       /* publish succeeds */
    uint16_t next_id;
    sent_t sent[MAX_SENT];
    uint32_t n_sent;
    uint32_t times_sent[4096]; /* per sequence number */
} pub;

static bool fake_publish(void *ctx, const uint8_t *frame, size_t len, uint16_t *packet_id)
{
    ws_frame_t f;
    (void)ctx;
    if (!pub.accept) {
        return false;
    }
    CHECK(ws_frame_decode(frame, len, &f, NULL) == WS_OK); /* every frame sent is valid */
    *packet_id = ++pub.next_id;
    if (pub.n_sent < MAX_SENT) {
        pub.sent[pub.n_sent++] = (sent_t){*packet_id, f.frame_flags, f.seq_first, f.count};
    }
    for (uint32_t s = f.seq_first; s < f.seq_first + f.count && s < 4096; s++) {
        pub.times_sent[s]++;
    }
    return true;
}

typedef struct {
    ws_store_t store;
    ws_cursor_t cursor;
    ws_uplink_t up;
} station_t;

static void boot(station_t *s)
{
    CHECK(ws_store_mount(&s->store, STORE_SECTORS, 1) == WS_STORE_OK);
    CHECK(ws_cursor_mount(&s->cursor, STORE_SECTORS));
    ws_uplink_init(&s->up, &s->store, &s->cursor, fake_publish, NULL);
}

static void fresh(station_t *s)
{
    memset(&pub, 0, sizeof pub);
    pub.accept = true;
    emu_flash_init(SECTORS);
    boot(s);
}

/* What the station does for each new record: store first, then hand it to the uplink. */
static void record(station_t *s)
{
    uint8_t rec[WS_RECORD_SIZE];
    uint32_t seq = ws_store_next_seq(&s->store);
    ws_record_t r = {.ts = T0 + 60 * seq, .t_mean = 100, .t_min = 100, .t_max = 100,
                     .rh_mean = 5000, .rh_min = 5000, .rh_max = 5000, .n_temp = 30, .n_rh = 30};
    ws_record_encode(&r, rec);
    CHECK(ws_store_append(&s->store, seq, rec) == WS_STORE_OK);
    ws_uplink_record_stored(&s->up, seq, rec);
    ws_store_service(&s->store);
}

static void ack_all(station_t *s)
{
    for (uint32_t i = 0; i < pub.n_sent; i++) {
        ws_uplink_ack(&s->up, pub.sent[i].packet_id, true);
    }
}

static void test_live_records_publish_at_once(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);

    record(&s);
    CHECK(pub.n_sent == 1);
    CHECK(pub.sent[0].first == 1 && pub.sent[0].count == 1 && pub.sent[0].flags == 0);
    CHECK(s.up.acked_through == 0); /* sent is not delivered */

    ws_uplink_ack(&s.up, pub.sent[0].packet_id, true);
    CHECK(s.up.acked_through == 1);
    CHECK(s.cursor.value == 1); /* persisted */
}

static void test_backlog_after_outage(void)
{
    station_t s;
    fresh(&s);

    for (int i = 0; i < 120; i++) {
        record(&s); /* offline: stored only */
    }
    CHECK(pub.n_sent == 0);

    ws_uplink_connected(&s.up);
    record(&s);         /* #121 goes live straight away... */
    ws_uplink_poll(&s.up); /* ...and the backlog follows from flash */
    CHECK(pub.sent[0].first == 121 && pub.sent[0].flags == 0);
    CHECK(pub.n_sent == 4); /* 50 + 50 + 20 */
    CHECK(pub.sent[1].first == 1 && pub.sent[1].count == 50);
    CHECK(pub.sent[3].first == 101 && pub.sent[3].count == 20);
    CHECK(pub.sent[1].flags == WS_FRAME_FLAG_BACKFILL);

    /* The live record is acknowledged first, but the cursor waits for the backlog before it. */
    ws_uplink_ack(&s.up, pub.sent[0].packet_id, true);
    CHECK(s.up.acked_through == 0);
    ws_uplink_ack(&s.up, pub.sent[2].packet_id, true);
    CHECK(s.up.acked_through == 0);
    ws_uplink_ack(&s.up, pub.sent[1].packet_id, true);
    CHECK(s.up.acked_through == 100);
    ws_uplink_ack(&s.up, pub.sent[3].packet_id, true);
    CHECK(s.up.acked_through == 121);
}

static void test_lost_connection_resends_from_cursor(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);
    for (int i = 0; i < 5; i++) {
        record(&s);
    }
    ws_uplink_ack(&s.up, pub.sent[0].packet_id, true);
    ws_uplink_ack(&s.up, pub.sent[1].packet_id, true);
    CHECK(s.up.acked_through == 2);

    ws_uplink_disconnected(&s.up); /* PUBACKs for #3..#5 never arrive */
    record(&s);                    /* #6 while offline */
    pub.n_sent = 0;
    ws_uplink_connected(&s.up);
    ws_uplink_poll(&s.up);
    CHECK(pub.n_sent == 1 && pub.sent[0].first == 3 && pub.sent[0].count == 4);
    ack_all(&s);
    CHECK(s.up.acked_through == 6);
}

/*
 * coreMQTT starts packet IDs again after a reconnect. A PUBACK must only ever match a publish from
 * the current connection, or the cursor could pass a record that was never delivered.
 */
static void test_packet_ids_reused_after_reconnect(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);
    record(&s);                                    /* #1, packet 1 */
    ws_uplink_ack(&s.up, 1, true);
    record(&s);                                    /* #2, packet 2: its PUBACK is lost */
    ws_uplink_disconnected(&s.up);

    pub.next_id = 0;                               /* new connection: IDs restart */
    ws_uplink_connected(&s.up);
    ws_uplink_poll(&s.up);                         /* backlog #2 goes out as packet 1 */
    record(&s);                                    /* #3, packet 2 */
    CHECK(pub.sent[2].packet_id == 1 && pub.sent[2].first == 2);
    CHECK(pub.sent[3].packet_id == 2 && pub.sent[3].first == 3);

    ws_uplink_ack(&s.up, 2, true);                 /* #3 confirmed; #2 is not */
    CHECK(s.up.acked_through == 1);
    ws_uplink_ack(&s.up, 1, true);
    CHECK(s.up.acked_through == 3);
}

static void test_publish_failure_falls_back_to_flash(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);

    pub.accept = false; /* e.g. the network buffer is full */
    record(&s);
    record(&s);
    CHECK(pub.n_sent == 0);

    pub.accept = true;
    record(&s);
    ws_uplink_poll(&s.up);
    ack_all(&s);
    CHECK(s.up.acked_through == 3);
    for (uint32_t seq = 1; seq <= 3; seq++) {
        CHECK(pub.times_sent[seq] >= 1);
    }
}

static void test_inflight_limit(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);

    /* No PUBACKs at all: sending stops at the limit instead of growing without bound. */
    for (uint32_t i = 0; i < 2 * WS_UPLINK_MAX_INFLIGHT; i++) {
        record(&s);
        ws_uplink_poll(&s.up);
    }
    CHECK(pub.n_sent == WS_UPLINK_MAX_INFLIGHT);

    /* Once acknowledged, the rest are sent from flash. */
    for (int round = 0; round < 4; round++) {
        ack_all(&s);
        ws_uplink_poll(&s.up);
    }
    CHECK(s.up.acked_through == 2 * WS_UPLINK_MAX_INFLIGHT);
}

static void test_rejected_publish(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);
    record(&s);
    ws_uplink_ack(&s.up, pub.sent[0].packet_id, false); /* e.g. reason 0x87, not authorized */
    CHECK(s.up.acked_through == 0);
    CHECK(s.up.needs_reconnect);
    CHECK(s.up.rejected == 1);
}

static void test_reboot_keeps_backlog(void)
{
    station_t s;
    fresh(&s);
    ws_uplink_connected(&s.up);
    for (int i = 0; i < 10; i++) {
        record(&s);
    }
    for (uint32_t i = 0; i < 4; i++) {
        ws_uplink_ack(&s.up, pub.sent[i].packet_id, true);
    }
    ws_uplink_disconnected(&s.up);
    for (int i = 0; i < 30; i++) {
        record(&s); /* offline */
    }

    /* Power lost while offline; on boot only flash remains. */
    boot(&s);
    CHECK(s.up.acked_through == 4);
    pub.n_sent = 0;
    ws_uplink_connected(&s.up);
    ws_uplink_poll(&s.up);
    CHECK(pub.n_sent == 1 && pub.sent[0].first == 5 && pub.sent[0].count == 36);
    ack_all(&s);
    CHECK(s.up.acked_through == 40);
}

static void test_unreadable_record_is_skipped(void)
{
    station_t s;
    fresh(&s);
    for (int i = 0; i < 5; i++) {
        record(&s);
    }
    /* Bit rot in record #3 (sector 0, slot 2) */
    emu_flash_memory()[WS_STORE_HEADER_SIZE + 2 * WS_RECORD_SIZE + 6] ^= 0x01;

    ws_uplink_connected(&s.up);
    ws_uplink_poll(&s.up);
    CHECK(pub.n_sent == 2);
    CHECK(pub.sent[0].first == 1 && pub.sent[0].count == 2);
    CHECK(pub.sent[1].first == 4 && pub.sent[1].count == 2);
    CHECK(s.up.missing == 1);
    ack_all(&s);
    CHECK(s.up.acked_through == 5); /* the cursor does not get stuck on it */
}

/* Random outages, lost PUBACKs and reboots: in the end every record is delivered. */
static void test_random_interleavings(void)
{
    uint64_t rng = 12345;
    for (int run = 0; run < 50; run++) {
        station_t s;
        fresh(&s);
        for (int step = 0; step < 600; step++) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            uint32_t r = (uint32_t)(rng % 100);
            if (r < 40) {
                record(&s);
            } else if (r < 70) {
                ws_uplink_poll(&s.up);
            } else if (r < 88 && pub.n_sent > 0) {
                /* acknowledge a random earlier publish (out of order, maybe twice) */
                ws_uplink_ack(&s.up, pub.sent[(uint32_t)(rng >> 32) % pub.n_sent].packet_id, true);
            } else if (r < 94) {
                if (s.up.connected) ws_uplink_disconnected(&s.up);
                else ws_uplink_connected(&s.up);
            } else if (r < 96) {
                boot(&s); /* power cycle: connection gone, RAM state rebuilt from flash */
            }
        }
        /* Settle: connect, then deliver everything. */
        if (!s.up.connected) ws_uplink_connected(&s.up);
        for (int round = 0; round < 50; round++) {
            ws_uplink_poll(&s.up);
            ack_all(&s);
        }
        uint32_t last = ws_store_next_seq(&s.store) - 1;
        CHECK(s.up.acked_through == last);
        for (uint32_t seq = 1; seq <= last; seq++) {
            CHECK(pub.times_sent[seq] >= 1);
        }
    }
}

int main(void)
{
    test_live_records_publish_at_once();
    test_backlog_after_outage();
    test_lost_connection_resends_from_cursor();
    test_packet_ids_reused_after_reconnect();
    test_publish_failure_falls_back_to_flash();
    test_inflight_limit();
    test_rejected_publish();
    test_reboot_keeps_backlog();
    test_unreadable_record_is_skipped();
    test_random_interleavings();
    CHECK(emu_flash_violations() == 0);
    return check_report("test_uplink");
}
