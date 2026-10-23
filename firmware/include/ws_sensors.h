#ifndef WS_SENSORS_H
#define WS_SENSORS_H

/*
 * Sensor drivers. Both return values in protocol units (0.01 °C, 0.01 %RH), using integer
 * arithmetic only: the Cortex-M3 has no FPU.
 */

#include <stdint.h>

#define WS_LM92_ADDR 0x48u    /* A1 = A0 = GND */
#define WS_HDC3020_ADDR 0x44u /* ADDR = ADDR1 = GND */

typedef enum {
    WS_SENSOR_OK = 0,
    WS_SENSOR_BUS_ERROR,    /* no acknowledge: sensor missing, bus fault, or still converting */
    WS_SENSOR_CRC_ERROR,    /* data corrupted on the bus */
    WS_SENSOR_NO_DATA,      /* sensor answered, but has no new measurement */
    WS_SENSOR_OUT_OF_RANGE, /* reading outside the sensor's specified range */
} ws_sensor_status_t;

/* LM92 temperature register (big-endian 16 bits) to 0.01 °C. Exposed for tests. */
int32_t ws_lm92_raw_to_centi(uint16_t raw);

ws_sensor_status_t ws_lm92_read(uint8_t addr, int16_t *temp_centi);

/* HDC3020 16-bit outputs to 0.01 °C and 0.01 %RH. Exposed for tests. */
int32_t ws_hdc3020_raw_to_temp_centi(uint16_t raw);
uint16_t ws_hdc3020_raw_to_rh_centi(uint16_t raw);

/* One trigger-on-demand measurement (blocks ~15 ms). Temperature is returned for diagnostics. */
ws_sensor_status_t ws_hdc3020_read(uint8_t addr, int16_t *temp_centi, uint16_t *rh_centi);

#endif
