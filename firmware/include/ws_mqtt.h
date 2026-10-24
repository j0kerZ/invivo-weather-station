#ifndef WS_MQTT_H
#define WS_MQTT_H

/*
 * Thin adapter over coreMQTT (AWS's embedded MQTT client, v5): connect with the thing name as
 * client ID, publish frames with QoS 1, and report each PUBACK with its MQTT 5 reason code.
 *
 * Portable: the network below (TLS socket on a PC, cellular modem or mbedTLS on the station) is
 * supplied as a coreMQTT TransportInterface_t.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_mqtt.h"
#include "ws_protocol.h"
#include "ws_uplink.h"

#define WS_MQTT_TOPIC_MAX 96u
#define WS_MQTT_OUTGOING (2u * WS_UPLINK_MAX_INFLIGHT) /* tracked records + live changes */

/* `reason` is the MQTT 5 reason code: below 0x80 is success (0x10 = no subscribers yet). */
typedef void (*ws_mqtt_ack_fn)(void *ctx, uint16_t packet_id, uint8_t reason);

typedef struct {
    MQTTContext_t mqtt; /* first member: the coreMQTT callback finds the adapter from it */
    uint8_t buffer[WS_FRAME_MAX_SIZE + 256];
    MQTTPubAckInfo_t outgoing[WS_MQTT_OUTGOING];
    uint8_t ack_props[64];
    char topic[WS_MQTT_TOPIC_MAX];
    bool connected;
    ws_mqtt_ack_fn on_ack;
    void *ack_ctx;
    MQTTStatus_t last_status;
} ws_mqtt_t;

bool ws_mqtt_init(ws_mqtt_t *m, const TransportInterface_t *transport,
                  MQTTGetCurrentTimeFunc_t now_ms, ws_mqtt_ack_fn on_ack, void *ack_ctx);

/* MQTT CONNECT over an already connected transport. Telemetry goes to `topic`. */
bool ws_mqtt_connect(ws_mqtt_t *m, const char *thing_name, const char *topic);

/* QoS 1 publish of a frame to the telemetry topic. Matches ws_publish_fn. */
bool ws_mqtt_publish(void *m, const uint8_t *frame, size_t len, uint16_t *packet_id);

/* Same, to any topic: used to check that the policy refuses other stations' topics. */
bool ws_mqtt_publish_to(ws_mqtt_t *m, const char *topic, const uint8_t *frame, size_t len,
                        uint16_t *packet_id);

/* Receives PUBACKs and sends keep-alives. Returns false when the connection is lost. */
bool ws_mqtt_process(ws_mqtt_t *m);

void ws_mqtt_disconnect(ws_mqtt_t *m);

#endif
