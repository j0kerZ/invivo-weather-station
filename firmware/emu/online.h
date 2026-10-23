#ifndef ONLINE_H
#define ONLINE_H

/*
 * Online mode of the emulator: the station publishes to AWS IoT Core over MQTT/TLS with its
 * X.509 certificate, in real time (or faster), with optional simulated network outages.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ws_cursor.h"
#include "ws_station.h"
#include "ws_store.h"
#include "ws_uplink.h"

typedef struct {
    const char *station_id; /* AWS IoT thing name */
    const char *certs_dir;  /* <id>.crt, <id>.key, AmazonRootCA1.pem, endpoint.txt */
    double speed;           /* emulated seconds per real second */
    double outage_at_min;   /* emulated minute the link drops; negative for none */
    double outage_min;      /* how long it stays down */
    bool verbose;
} online_config_t;

/* Prepares the MQTT client and the uplink. Returns the uplink the record sink must feed. */
ws_uplink_t *online_init(const online_config_t *cfg, ws_store_t *store, ws_cursor_t *cursor);

/* Runs the station in (scaled) real time while publishing. Returns 0 if everything was delivered. */
int online_run(ws_station_t *st, uint32_t start, uint32_t duration_s);

/* Publishes to another station's topic; returns 0 if IoT Core refuses it, as the policy requires. */
int online_authz_check(const char *other_station_id);

#endif
