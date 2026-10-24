/*
 * LM92 ±0.33 °C digital temperature sensor (TI SNIS110).
 *
 * Temperature register (pointer 0x00), read MSB first:
 *   D15..D3  temperature, 13-bit two's complement, 0.0625 °C per count
 *   D2..D0   status bits (CRIT, HIGH, LOW): not temperature, masked off
 *
 * The real LM92 converts every 500 ms typical, 1000 ms maximum, and reading faster stops the value
 * from updating. The exercise assumes sensors that respond within milliseconds, which the station's
 * 10 Hz sampling relies on: a real build would need a faster temperature sensor.
 */

#include "ws_hal.h"
#include "ws_protocol.h"
#include "ws_sensors.h"

#define LM92_REG_TEMPERATURE 0x00u

int32_t ws_lm92_raw_to_centi(uint16_t raw)
{
    /* Sign-extend the 16-bit word without relying on implementation-defined conversions. */
    int32_t word = (int32_t)(raw & 0xFFF8u);
    if (word >= 0x8000) {
        word -= 0x10000;
    }
    int32_t counts = word / 8; /* exact: the low three bits are zero */

    /* 1 count = 6.25 hundredths of a degree; round half away from zero. */
    int32_t scaled = counts * 25;
    return (scaled + (scaled >= 0 ? 2 : -2)) / 4;
}

ws_sensor_status_t ws_lm92_read(uint8_t addr, int16_t *temp_centi)
{
    const uint8_t pointer = LM92_REG_TEMPERATURE;
    uint8_t rx[2];

    if (!ws_hal_i2c_write_read(addr, &pointer, 1, rx, sizeof rx)) {
        return WS_SENSOR_BUS_ERROR;
    }
    int32_t centi = ws_lm92_raw_to_centi((uint16_t)((rx[0] << 8) | rx[1]));
    if (centi < WS_TEMP_MIN || centi > WS_TEMP_MAX) {
        return WS_SENSOR_OUT_OF_RANGE;
    }
    *temp_centi = (int16_t)centi;
    return WS_SENSOR_OK;
}
