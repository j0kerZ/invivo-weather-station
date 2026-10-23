/*
 * HDC3020 relative humidity and temperature sensor (TI SNAS778D).
 *
 * Trigger-on-demand, low power mode 0 (lowest noise): write command 0x24 0x00, wait for the
 * 12.5 ms typical conversion, then read 6 bytes: T MSB, T LSB, CRC, RH MSB, RH LSB, CRC.
 * The CRC is CRC-8/NRSC-5 over each 16-bit word: the same algorithm as our record CRC.
 *
 * Trigger-on-demand is used instead of auto measurement mode because each sample is then taken
 * exactly when the station asks for it, and the sensor sleeps in between. In auto mode a read
 * that arrives before the next conversion returns 0xFFFF / 0xFFFF, which converts to +130 °C and
 * 100 %RH: a plausible-looking but false reading. We reject that pattern in either mode.
 */

#include "ws_crc.h"
#include "ws_hal.h"
#include "ws_protocol.h"
#include "ws_sensors.h"

#define HDC3020_CONVERSION_WAIT_MS 15u /* 12.5 ms typical for LPM0, plus margin */
#define HDC3020_TEMP_MIN_CENTI (-4000)  /* specified range -40 °C ... 125 °C */
#define HDC3020_TEMP_MAX_CENTI 12500

/* T(°C) = -45 + 175 * raw / (2^16 - 1), in hundredths, rounded to nearest. */
int32_t ws_hdc3020_raw_to_temp_centi(uint16_t raw)
{
    return (int32_t)((17500u * (uint32_t)raw + 32767u) / 65535u) - 4500; /* fits in 32 bits */
}

/* RH(%) = 100 * raw / (2^16 - 1), in hundredths, rounded to nearest. */
uint16_t ws_hdc3020_raw_to_rh_centi(uint16_t raw)
{
    return (uint16_t)((10000u * (uint32_t)raw + 32767u) / 65535u);
}

ws_sensor_status_t ws_hdc3020_read(uint8_t addr, int16_t *temp_centi, uint16_t *rh_centi)
{
    static const uint8_t trigger_lpm0[] = {0x24, 0x00};
    uint8_t rx[6];

    if (!ws_hal_i2c_write(addr, trigger_lpm0, sizeof trigger_lpm0)) {
        return WS_SENSOR_BUS_ERROR;
    }
    ws_hal_delay_ms(HDC3020_CONVERSION_WAIT_MS);
    if (!ws_hal_i2c_read(addr, rx, sizeof rx)) {
        return WS_SENSOR_BUS_ERROR;
    }
    if (ws_crc8(&rx[0], 2) != rx[2] || ws_crc8(&rx[3], 2) != rx[5]) {
        return WS_SENSOR_CRC_ERROR;
    }

    uint16_t t_raw = (uint16_t)((rx[0] << 8) | rx[1]);
    uint16_t rh_raw = (uint16_t)((rx[3] << 8) | rx[4]);
    if (t_raw == 0xFFFFu && rh_raw == 0xFFFFu) {
        return WS_SENSOR_NO_DATA;
    }

    int32_t t = ws_hdc3020_raw_to_temp_centi(t_raw);
    if (t < HDC3020_TEMP_MIN_CENTI || t > HDC3020_TEMP_MAX_CENTI) {
        return WS_SENSOR_OUT_OF_RANGE;
    }
    *temp_centi = (int16_t)t;
    *rh_centi = ws_hdc3020_raw_to_rh_centi(rh_raw);
    return WS_SENSOR_OK;
}
