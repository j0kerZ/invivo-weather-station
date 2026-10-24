/*
 * MQTT adapter tests over a fake network: the broker side is a scripted CONNACK, and every byte the
 * station sends is captured. Checks the MQTT 5 topic alias byte for byte.
 */

#include <string.h>

#include "check.h"
#include "ws_mqtt.h"

#define TOPIC "invivo-ws/station-001/telemetry" /* 31 bytes */

struct NetworkContext {
    const uint8_t *rx;
    size_t rx_len, rx_pos;
    uint8_t tx[4096];
    size_t tx_len;
};

static int32_t fake_recv(NetworkContext_t *n, void *buf, size_t len)
{
    size_t left = n->rx_len - n->rx_pos;
    size_t k = len < left ? len : left;
    memcpy(buf, &n->rx[n->rx_pos], k);
    n->rx_pos += k;
    return (int32_t)k; /* 0 = nothing to read now */
}

static int32_t fake_send(NetworkContext_t *n, const void *buf, size_t len)
{
    if (n->tx_len + len > sizeof n->tx) {
        return -1;
    }
    memcpy(&n->tx[n->tx_len], buf, len);
    n->tx_len += len;
    return (int32_t)len;
}

static uint32_t fake_ms;
static uint32_t now_ms(void)
{
    return fake_ms++;
}

/* MQTT 5 CONNACK, success, with or without a Topic Alias Maximum property (0x22). */
static const uint8_t CONNACK_ALIAS_8[] = {0x20, 0x06, 0x00, 0x00, 0x03, 0x22, 0x00, 0x08};
static const uint8_t CONNACK_NO_ALIAS[] = {0x20, 0x03, 0x00, 0x00, 0x00};

/* The 31-byte example frame from docs/PROTOCOL.md §10. */
static const uint8_t FRAME[] = {
    0x01, 0x00, 0x01, 0x00, 0x12, 0x04, 0x00, 0x00, 0x00, 0x14, 0xdb, 0x6a, 0x00,
    0xd2, 0x04, 0xba, 0x04, 0xed, 0x04, 0xd6, 0x1f, 0x9a, 0x1f, 0x1c, 0x20,
    0x64, 0x64, 0x00, 0x61, 0x04, 0xa3,
};

static NetworkContext_t net;
static TransportInterface_t transport;
static ws_mqtt_t mqtt;

static void connect_with(const uint8_t *connack, size_t len, bool use_alias)
{
    memset(&net, 0, sizeof net);
    net.rx = connack;
    net.rx_len = len;
    transport = (TransportInterface_t){.pNetworkContext = &net, .send = fake_send,
                                       .recv = fake_recv};
    CHECK(ws_mqtt_init(&mqtt, &transport, now_ms, NULL, NULL));
    mqtt.use_topic_alias = use_alias;
    CHECK(ws_mqtt_connect(&mqtt, "station-001", TOPIC));
}

/* Publishes the frame and returns how many bytes went onto the network. */
static size_t publish(const char *topic, const uint8_t **packet)
{
    uint16_t id;
    size_t before = net.tx_len;
    CHECK(ws_mqtt_publish_to(&mqtt, topic, FRAME, sizeof FRAME, &id));
    *packet = &net.tx[before];
    return net.tx_len - before;
}

static void test_alias_after_first_publish(void)
{
    const uint8_t *p;
    connect_with(CONNACK_ALIAS_8, sizeof CONNACK_ALIAS_8, true);
    CHECK(mqtt.topic_alias_max == 8);

    /* First: full topic plus alias 1, which sets up the mapping on the broker.
     * 2 fixed header + 2+31 topic + 2 packet id + 1+3 properties + 31 payload = 72 */
    CHECK(publish(TOPIC, &p) == 72);
    CHECK(p[0] == 0x32);                                  /* PUBLISH, QoS 1 */
    CHECK(p[2] == 0x00 && p[3] == 31);                    /* topic length */
    CHECK(memcmp(&p[4], TOPIC, 31) == 0);
    CHECK(p[37] == 3 && p[38] == 0x23 && p[39] == 0x00 && p[40] == 0x01); /* alias = 1 */

    /* After: empty topic, alias only. 2 + 2 + 2 + 4 + 31 = 41 */
    CHECK(publish(TOPIC, &p) == 41);
    CHECK(p[2] == 0x00 && p[3] == 0x00);                  /* zero-length topic */
    CHECK(p[6] == 3 && p[7] == 0x23 && p[9] == 0x01);
    CHECK(memcmp(&p[10], FRAME, sizeof FRAME) == 0);
    CHECK(publish(TOPIC, &p) == 41);
}

static void test_no_alias_when_broker_allows_none(void)
{
    const uint8_t *p;
    connect_with(CONNACK_NO_ALIAS, sizeof CONNACK_NO_ALIAS, true);
    CHECK(mqtt.topic_alias_max == 0);
    /* 2 + 33 + 2 + 1 (empty properties) + 31 = 69, every time */
    CHECK(publish(TOPIC, &p) == 69);
    CHECK(publish(TOPIC, &p) == 69);
}

static void test_alias_can_be_switched_off(void)
{
    const uint8_t *p;
    connect_with(CONNACK_ALIAS_8, sizeof CONNACK_ALIAS_8, false);
    CHECK(publish(TOPIC, &p) == 69);
    CHECK(publish(TOPIC, &p) == 69);
}

static void test_other_topics_never_use_the_alias(void)
{
    const uint8_t *p;
    const char *other = "invivo-ws/station-002/telemetry";
    connect_with(CONNACK_ALIAS_8, sizeof CONNACK_ALIAS_8, true);
    CHECK(publish(TOPIC, &p) == 72);
    CHECK(publish(other, &p) == 69);
    CHECK(memcmp(&p[4], other, 31) == 0);
}

static void test_alias_set_up_again_after_reconnect(void)
{
    const uint8_t *p;
    connect_with(CONNACK_ALIAS_8, sizeof CONNACK_ALIAS_8, true);
    CHECK(publish(TOPIC, &p) == 72);
    CHECK(publish(TOPIC, &p) == 41);
    connect_with(CONNACK_ALIAS_8, sizeof CONNACK_ALIAS_8, true); /* a new connection */
    CHECK(publish(TOPIC, &p) == 72);                             /* full topic once more */
}

int main(void)
{
    test_alias_after_first_publish();
    test_no_alias_when_broker_allows_none();
    test_alias_can_be_switched_off();
    test_other_topics_never_use_the_alias();
    test_alias_set_up_again_after_reconnect();
    return check_report("test_mqtt");
}
