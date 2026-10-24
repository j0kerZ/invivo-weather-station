#define _POSIX_C_SOURCE 200809L /* nanosleep */

#include "online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu.h"
#include "emu_net.h"
#include "ws_mqtt.h"

#define IOT_PORT 8883u
#define BACKOFF_MIN_MS 1000u
#define BACKOFF_MAX_MS 30000u
#define DRAIN_MS 15000u
#define PENDING 64u
#define MAX_RTT 100000u

static online_config_t cfg;
static char endpoint[256], ca_file[512], cert_file[512], key_file[512];
static char topic[WS_MQTT_TOPIC_MAX];
static ws_store_t *store;

static NetworkContext_t net;
static TransportInterface_t transport;
static ws_mqtt_t mqtt;
static ws_uplink_t uplink;
static bool link_up;
static uint32_t next_attempt_ms, backoff_ms = BACKOFF_MIN_MS;
static uint32_t connections, link_losses;

/* Publish times by packet ID, for PUBACK round-trip times. */
static struct {
    uint16_t id;
    uint32_t sent_ms;
    uint32_t first;
    uint8_t count;
    bool backfill;
} pending[PENDING];
static uint32_t rtt[MAX_RTT];
static size_t n_rtt;
static uint16_t last_ack_id;
static uint8_t last_ack_reason;

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static bool read_first_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    bool ok = f != NULL && fgets(buf, (int)size, f) != NULL;
    if (f != NULL) {
        fclose(f);
    }
    if (ok) {
        buf[strcspn(buf, "\r\n")] = '\0';
    }
    return ok && buf[0] != '\0';
}

static bool publish(void *ctx, const uint8_t *frame, size_t len, uint16_t *packet_id)
{
    (void)ctx;
    if (!ws_mqtt_publish(&mqtt, frame, len, packet_id)) {
        return false;
    }
    pending[*packet_id % PENDING].id = *packet_id;
    pending[*packet_id % PENDING].sent_ms = emu_now_ms();
    pending[*packet_id % PENDING].first = (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) |
                                          ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 24);
    pending[*packet_id % PENDING].count = frame[2];
    pending[*packet_id % PENDING].backfill = (frame[1] & WS_FRAME_FLAG_BACKFILL) != 0;
    return true;
}

static void on_ack(void *ctx, uint16_t packet_id, uint8_t reason)
{
    (void)ctx;
    last_ack_id = packet_id;
    last_ack_reason = reason;

    if (pending[packet_id % PENDING].id == packet_id) {
        uint32_t ms = emu_now_ms() - pending[packet_id % PENDING].sent_ms;
        uint32_t first = pending[packet_id % PENDING].first;
        uint8_t count = pending[packet_id % PENDING].count;
        bool backfill = pending[packet_id % PENDING].backfill;
        pending[packet_id % PENDING].id = 0;
        if (n_rtt < MAX_RTT) {
            rtt[n_rtt++] = ms;
        }
        if (count > 1) {
            printf("    PUBACK #%lu-#%lu (backfill, %u records) %4lu ms", (unsigned long)first,
                   (unsigned long)(first + count - 1), count, (unsigned long)ms);
        } else {
            printf("    PUBACK #%lu (%s) %4lu ms", (unsigned long)first,
                   backfill ? "backfill" : "live", (unsigned long)ms);
        }
        printf(reason < 0x80 ? "\n" : "  REJECTED, reason 0x%02x\n", reason);
    }
    ws_uplink_ack(&uplink, packet_id, reason < 0x80);
}

static bool connect_now(void)
{
    uint32_t t0 = emu_now_ms();

    if (!emu_net_connect(&net, endpoint, IOT_PORT, ca_file, cert_file, key_file)) {
        return false;
    }
    transport = (TransportInterface_t){
        .pNetworkContext = &net, .send = emu_net_send, .recv = emu_net_recv};
    if (!ws_mqtt_init(&mqtt, &transport, emu_now_ms, on_ack, NULL) ||
        !ws_mqtt_connect(&mqtt, cfg.station_id, topic)) {
        fprintf(stderr, "mqtt: connect failed: %s\n", MQTT_Status_strerror(mqtt.last_status));
        emu_net_close(&net);
        return false;
    }
    printf("--- connected to %s as %s (TLS + MQTT %lu ms) ---\n", endpoint, cfg.station_id,
           (unsigned long)(emu_now_ms() - t0));
    return true;
}

static void drop_link(const char *why, bool graceful)
{
    if (!link_up) {
        return;
    }
    if (graceful) {
        ws_mqtt_disconnect(&mqtt);
    }
    emu_net_close(&net);
    link_up = false;
    ws_uplink_disconnected(&uplink);
    printf("--- link down: %s ---\n", why);
}

static bool in_outage(uint32_t elapsed_s)
{
    double minute = elapsed_s / 60.0;
    return cfg.outage_at_min >= 0 && minute >= cfg.outage_at_min &&
           minute < cfg.outage_at_min + cfg.outage_min;
}

/* Keeps the connection up (with backoff), receives PUBACKs, sends backlog. */
static void service(uint32_t elapsed_s)
{
    uint32_t now = emu_now_ms();

    if (in_outage(elapsed_s)) {
        if (link_up) {
            link_losses++;
            drop_link("simulated loss of signal", false);
        }
        return;
    }
    if (link_up && uplink.needs_reconnect) {
        drop_link("publish rejected; will resend from the cursor", true);
    }
    if (!link_up && (int32_t)(now - next_attempt_ms) >= 0) {
        if (connect_now()) {
            link_up = true;
            connections++;
            backoff_ms = BACKOFF_MIN_MS;
            ws_uplink_connected(&uplink);
        } else {
            next_attempt_ms = now + backoff_ms;
            backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
        }
    }
    if (link_up) {
        if (!ws_mqtt_process(&mqtt)) {
            link_losses++;
            drop_link(MQTT_Status_strerror(mqtt.last_status), false);
        } else {
            ws_uplink_poll(&uplink);
        }
    }
}

ws_uplink_t *online_init(const online_config_t *config, ws_store_t *s, ws_cursor_t *cursor)
{
    cfg = *config;
    store = s;
    snprintf(ca_file, sizeof ca_file, "%s/AmazonRootCA1.pem", cfg.certs_dir);
    snprintf(cert_file, sizeof cert_file, "%s/%s.crt", cfg.certs_dir, cfg.station_id);
    snprintf(key_file, sizeof key_file, "%s/%s.key", cfg.certs_dir, cfg.station_id);
    snprintf(topic, sizeof topic, "invivo-ws/%s/telemetry", cfg.station_id);

    char endpoint_file[512];
    snprintf(endpoint_file, sizeof endpoint_file, "%s/endpoint.txt", cfg.certs_dir);
    if (!read_first_line(endpoint_file, endpoint, sizeof endpoint)) {
        fprintf(stderr, "online: cannot read %s (run tools/station-credentials.sh fetch)\n",
                endpoint_file);
        return NULL;
    }
    ws_uplink_init(&uplink, store, cursor, publish, NULL);
    return &uplink;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static uint32_t percentile(double p)
{
    return rtt[(size_t)(p * (double)(n_rtt - 1))];
}

int online_run(ws_station_t *st, uint32_t start, uint32_t duration_s)
{
    uint32_t real_start = emu_now_ms();

    for (uint64_t i = 0; i <= (uint64_t)duration_s * WS_SAMPLE_HZ; i++) {
        uint32_t elapsed_s = (uint32_t)(i / WS_SAMPLE_HZ);
        uint32_t due = real_start + (uint32_t)((double)i * WS_SAMPLE_PERIOD_MS / cfg.speed);
        while ((int32_t)(emu_now_ms() - due) < 0) {
            service(elapsed_s);
            sleep_ms(2);
        }
        /* sample; a finished record or change is stored and published at once */
        emu_step(st, start + elapsed_s, (uint8_t)(i % WS_SAMPLE_HZ * 10u / WS_SAMPLE_HZ));
        service(elapsed_s);
    }

    /* Let outstanding PUBACKs and the last backlog arrive. */
    uint32_t last = ws_store_next_seq(store) - 1;
    uint32_t drain_until = emu_now_ms() + DRAIN_MS;
    while (uplink.acked_through < last && (int32_t)(emu_now_ms() - drain_until) < 0) {
        service(duration_s);
        sleep_ms(2);
    }
    drop_link("run finished", true);

    fprintf(stderr,
            "\nuplink: %lu connections, %lu link losses; %lu live frames, %lu backfill frames "
            "(%lu records); %lu rejected, %lu unreadable\n"
            "uplink: delivered through #%lu of #%lu%s\n",
            (unsigned long)connections, (unsigned long)link_losses,
            (unsigned long)uplink.live_frames, (unsigned long)uplink.backfill_frames,
            (unsigned long)uplink.backfill_records, (unsigned long)uplink.rejected,
            (unsigned long)uplink.missing, (unsigned long)uplink.acked_through,
            (unsigned long)last, uplink.acked_through == last ? " (all)" : "");
    if (n_rtt > 0) {
        qsort(rtt, n_rtt, sizeof rtt[0], compare_u32);
        fprintf(stderr, "PUBACK round trip over %zu publishes: median %lu ms, p95 %lu ms, max %lu ms\n",
                n_rtt, (unsigned long)percentile(0.5), (unsigned long)percentile(0.95),
                (unsigned long)rtt[n_rtt - 1]);
    }
    return uplink.acked_through == last ? 0 : 1;
}

/* Waits up to 5 s for the PUBACK of `id`. Returns its reason code, or -1 if the link closed. */
static int await_ack(uint16_t id)
{
    uint32_t until = emu_now_ms() + 5000;
    last_ack_id = 0;
    while ((int32_t)(emu_now_ms() - until) < 0) {
        if (!ws_mqtt_process(&mqtt)) {
            return -1;
        }
        if (last_ack_id == id) {
            return last_ack_reason;
        }
        sleep_ms(2);
    }
    return -2;
}

int online_authz_check(const char *other_station_id)
{
    char other_topic[WS_MQTT_TOPIC_MAX];
    uint8_t record[WS_RECORD_SIZE], frame[WS_FRAME_SIZE(1)];
    size_t len;
    uint16_t id;
    ws_record_t r = {.ts = (uint32_t)time(NULL), .t_mean = 1234, .t_min = 1234, .t_max = 1234,
                     .rh_mean = 5000, .rh_min = 5000, .rh_max = 5000, .temp_valid_pct = 100,
                     .rh_valid_pct = 100,
                     .flags = WS_FLAG_EVENT};
    ws_record_encode(&r, record);
    ws_frame_encode(0, 1, record, 1, frame, sizeof frame, &len);
    snprintf(other_topic, sizeof other_topic, "invivo-ws/%s/telemetry", other_station_id);

    if (!connect_now()) {
        return 1;
    }
    printf("publishing as %s to %s ...\n", cfg.station_id, other_topic);
    int denied = ws_mqtt_publish_to(&mqtt, other_topic, frame, len, &id) ? await_ack(id) : -1;
    if (denied == -1) {
        printf("  refused: IoT Core closed the connection\n");
    } else if (denied >= 0x80) {
        printf("  refused: PUBACK reason 0x%02x%s\n", (unsigned)denied,
               denied == 0x87 ? " (not authorized)" : "");
    } else {
        printf("  ACCEPTED (reason %d): the policy is too permissive\n", denied);
    }
    emu_net_close(&net);

    /* Control: the station's own topic must still be allowed. */
    if (!connect_now()) {
        return 1;
    }
    printf("publishing as %s to %s ...\n", cfg.station_id, topic);
    int own = ws_mqtt_publish_to(&mqtt, topic, frame, len, &id) ? await_ack(id) : -1;
    printf(own >= 0 && own < 0x80 ? "  accepted (reason 0x%02x)\n" : "  FAILED (%d)\n", own);
    ws_mqtt_disconnect(&mqtt);
    emu_net_close(&net);

    bool refused = denied == -1 || denied >= 0x80;
    return refused && own >= 0 && own < 0x80 ? 0 : 1;
}
