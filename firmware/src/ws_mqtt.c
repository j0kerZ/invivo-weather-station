#include "ws_mqtt.h"

#include <string.h>

#define CONNACK_TIMEOUT_MS 5000u
#define KEEP_ALIVE_S 60u

static bool on_packet(MQTTContext_t *ctx, MQTTPacketInfo_t *packet, MQTTDeserializedInfo_t *info,
                      MQTTSuccessFailReasonCode_t *reason_out, MQTTPropBuilder_t *send_props,
                      MQTTPropBuilder_t *get_props)
{
    ws_mqtt_t *m = (ws_mqtt_t *)ctx; /* ctx is the first member of ws_mqtt_t */
    (void)reason_out;
    (void)send_props;
    (void)get_props;

    if ((packet->type & 0xF0u) == MQTT_PACKET_TYPE_PUBACK && m->on_ack != NULL) {
        /* MQTT 5 may omit the reason code on success. */
        uint8_t reason = 0;
        if (info->pReasonCode != NULL && info->pReasonCode->reasonCodeLength > 0) {
            reason = info->pReasonCode->reasonCode[0];
        }
        m->on_ack(m->ack_ctx, info->packetIdentifier, reason);
    }
    return true;
}

bool ws_mqtt_init(ws_mqtt_t *m, const TransportInterface_t *transport,
                  MQTTGetCurrentTimeFunc_t now_ms, ws_mqtt_ack_fn on_ack, void *ack_ctx)
{
    memset(m, 0, sizeof *m);
    m->on_ack = on_ack;
    m->ack_ctx = ack_ctx;

    MQTTFixedBuffer_t fixed = {.pBuffer = m->buffer, .size = sizeof m->buffer};
    m->last_status = MQTT_Init(&m->mqtt, transport, now_ms, on_packet, &fixed);
    if (m->last_status != MQTTSuccess) {
        return false;
    }
    /* QoS 1 bookkeeping only. No retransmit store: flash is the retransmit buffer. */
    m->last_status = MQTT_InitStatefulQoS(&m->mqtt, m->outgoing, WS_UPLINK_MAX_INFLIGHT, NULL, 0,
                                          m->ack_props, sizeof m->ack_props);
    return m->last_status == MQTTSuccess;
}

bool ws_mqtt_connect(ws_mqtt_t *m, const char *thing_name, const char *topic)
{
    size_t topic_len = strlen(topic);
    if (topic_len >= sizeof m->topic) {
        return false;
    }
    memcpy(m->topic, topic, topic_len + 1);

    /* The client ID must be the thing name: the IoT policy only allows that. Clean session,
     * because nothing is resumed from the broker: unacknowledged records are resent from flash. */
    MQTTConnectInfo_t info = {
        .cleanSession = true,
        .keepAliveSeconds = KEEP_ALIVE_S,
        .pClientIdentifier = thing_name,
        .clientIdentifierLength = (uint16_t)strlen(thing_name),
    };
    bool session_present = false;
    m->last_status =
        MQTT_Connect(&m->mqtt, &info, NULL, CONNACK_TIMEOUT_MS, &session_present, NULL, NULL);
    m->connected = m->last_status == MQTTSuccess;
    return m->connected;
}

bool ws_mqtt_publish_to(ws_mqtt_t *m, const char *topic, const uint8_t *frame, size_t len,
                        uint16_t *packet_id)
{
    MQTTPublishInfo_t info = {
        .qos = MQTTQoS1,
        .pTopicName = topic,
        .topicNameLength = (uint16_t)strlen(topic),
        .pPayload = frame,
        .payloadLength = len,
    };
    uint16_t id = MQTT_GetPacketId(&m->mqtt);

    m->last_status = MQTT_Publish(&m->mqtt, &info, id, NULL);
    if (m->last_status != MQTTSuccess) {
        return false;
    }
    *packet_id = id;
    return true;
}

bool ws_mqtt_publish(void *m, const uint8_t *frame, size_t len, uint16_t *packet_id)
{
    ws_mqtt_t *client = m;
    return client->connected && ws_mqtt_publish_to(client, client->topic, frame, len, packet_id);
}

bool ws_mqtt_process(ws_mqtt_t *m)
{
    if (!m->connected) {
        return false;
    }
    m->last_status = MQTT_ProcessLoop(&m->mqtt);
    if (m->last_status != MQTTSuccess && m->last_status != MQTTNeedMoreBytes) {
        m->connected = false;
    }
    return m->connected;
}

void ws_mqtt_disconnect(ws_mqtt_t *m)
{
    if (m->connected) {
        (void)MQTT_Disconnect(&m->mqtt, NULL, NULL);
    }
    m->connected = false;
}
