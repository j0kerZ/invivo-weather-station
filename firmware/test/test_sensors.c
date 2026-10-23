/*
 * Sensor driver tests against a fake I2C bus. Expected values come from the datasheets, not from
 * the emulator, so a mistake shared by a driver and its emulated chip would still be caught.
 */

#include <string.h>

#include "check.h"
#include "ws_crc.h"
#include "ws_hal.h"
#include "ws_sensors.h"

/* ---- Fake HAL: acknowledges or not, returns scripted bytes, records what was sent ---- */

static struct {
    bool ack_write;
    bool ack_read;
    uint8_t rx[6];
    size_t rx_len;
    uint8_t addr;
    uint8_t tx[4];
    size_t tx_len;
    int reads;
    uint32_t delay_ms;
} fake;

static void fake_reset(bool ack, const uint8_t *rx, size_t rx_len)
{
    memset(&fake, 0, sizeof fake);
    fake.ack_write = fake.ack_read = ack;
    memcpy(fake.rx, rx, rx_len);
    fake.rx_len = rx_len;
}

bool ws_hal_i2c_write(uint8_t addr, const uint8_t *tx, size_t tx_len)
{
    fake.addr = addr;
    memcpy(fake.tx, tx, tx_len);
    fake.tx_len = tx_len;
    return fake.ack_write;
}

bool ws_hal_i2c_read(uint8_t addr, uint8_t *rx, size_t rx_len)
{
    fake.reads++;
    if (!fake.ack_read || addr != fake.addr || rx_len != fake.rx_len) {
        return false;
    }
    memcpy(rx, fake.rx, rx_len);
    return true;
}

bool ws_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                           size_t rx_len)
{
    return ws_hal_i2c_write(addr, tx, tx_len) && ws_hal_i2c_read(addr, rx, rx_len);
}

void ws_hal_delay_ms(uint32_t ms)
{
    fake.delay_ms += ms;
}

/* ---- LM92 ---- */

static void test_lm92_conversion(void)
{
    /* Datasheet temperature table, 13 bits shifted left by 3; values in 0.01 °C */
    CHECK(ws_lm92_raw_to_centi(0x4B00) == 15000); /* +150 °C */
    CHECK(ws_lm92_raw_to_centi(0x0C80) == 2500);  /* +25 °C */
    CHECK(ws_lm92_raw_to_centi(0x0010) == 13);    /* +0.125 °C rounds half away from zero */
    CHECK(ws_lm92_raw_to_centi(0x0008) == 6);     /* +0.0625 °C */
    CHECK(ws_lm92_raw_to_centi(0x0000) == 0);
    CHECK(ws_lm92_raw_to_centi(0xFFF8) == -6);    /* -0.0625 °C */
    CHECK(ws_lm92_raw_to_centi(0xFFF0) == -13);   /* -0.125 °C */
    CHECK(ws_lm92_raw_to_centi(0xF380) == -2500); /* -25 °C */
    CHECK(ws_lm92_raw_to_centi(0xE480) == -5500); /* -55 °C */
    CHECK(ws_lm92_raw_to_centi(0x0C87) == 2500);  /* status bits D2..D0 are not temperature */
    CHECK(ws_lm92_raw_to_centi(0xFFFF) == -6);    /* ...also for negative values */
}

static void test_lm92_read(void)
{
    int16_t t = 0;

    fake_reset(true, (const uint8_t[]){0x0C, 0x80}, 2);
    CHECK(ws_lm92_read(WS_LM92_ADDR, &t) == WS_SENSOR_OK);
    CHECK(t == 2500);
    CHECK(fake.addr == 0x48);
    CHECK(fake.tx_len == 1 && fake.tx[0] == 0x00); /* temperature register pointer */

    fake_reset(true, (const uint8_t[]){0x4B, 0x80}, 2); /* +151 °C */
    CHECK(ws_lm92_read(WS_LM92_ADDR, &t) == WS_SENSOR_OUT_OF_RANGE);
    fake_reset(true, (const uint8_t[]){0xE4, 0x00}, 2); /* -56 °C */
    CHECK(ws_lm92_read(WS_LM92_ADDR, &t) == WS_SENSOR_OUT_OF_RANGE);

    fake_reset(false, (const uint8_t[]){0x0C, 0x80}, 2);
    CHECK(ws_lm92_read(WS_LM92_ADDR, &t) == WS_SENSOR_BUS_ERROR);
}

/* ---- HDC3020 ---- */

static void test_hdc3020_conversion(void)
{
    CHECK(ws_hdc3020_raw_to_temp_centi(0x0000) == -4500);
    CHECK(ws_hdc3020_raw_to_temp_centi(0xFFFF) == 13000);
    CHECK(ws_hdc3020_raw_to_temp_centi(0x6666) == 2500); /* 24.9996 °C rounds to 25.00 */
    CHECK(ws_hdc3020_raw_to_rh_centi(0x0000) == 0);
    CHECK(ws_hdc3020_raw_to_rh_centi(0xFFFF) == 10000);
    CHECK(ws_hdc3020_raw_to_rh_centi(0x8000) == 5000);
    CHECK(ws_hdc3020_raw_to_rh_centi(0xABCD) == 6711);
}

/* Builds a 6-byte readout with correct CRCs. */
static void readout(uint8_t out[6], uint16_t t_raw, uint16_t rh_raw)
{
    out[0] = (uint8_t)(t_raw >> 8);
    out[1] = (uint8_t)t_raw;
    out[2] = ws_crc8(&out[0], 2);
    out[3] = (uint8_t)(rh_raw >> 8);
    out[4] = (uint8_t)rh_raw;
    out[5] = ws_crc8(&out[3], 2);
}

static void test_hdc3020_read(void)
{
    uint8_t rx[6];
    int16_t t = 0;
    uint16_t rh = 0;

    /* The datasheet's CRC example is CRC(0xABCD) = 0x6F; used here as the RH word. */
    readout(rx, 0x6666, 0xABCD);
    CHECK(rx[5] == 0x6F);
    fake_reset(true, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_OK);
    CHECK(t == 2500);
    CHECK(rh == 6711);
    CHECK(fake.addr == 0x44);
    CHECK(fake.tx_len == 2 && fake.tx[0] == 0x24 && fake.tx[1] == 0x00); /* trigger, LPM0 */
    CHECK(fake.delay_ms >= 13); /* waited out the 12.5 ms conversion before reading */

    readout(rx, 0x6666, 0xABCD);
    rx[5] ^= 0x01;
    fake_reset(true, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_CRC_ERROR);

    readout(rx, 0x6666, 0xABCD);
    rx[0] ^= 0x80; /* data bit flipped, CRC byte intact */
    fake_reset(true, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_CRC_ERROR);

    /* "No new data" pattern: must not become +130 °C / 100 %RH */
    readout(rx, 0xFFFF, 0xFFFF);
    fake_reset(true, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_NO_DATA);

    readout(rx, 0x0000, 0x8000); /* -45 °C: below the -40 °C specified range */
    fake_reset(true, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_OUT_OF_RANGE);

    readout(rx, 0x6666, 0xABCD);
    fake_reset(false, rx, 6);
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_BUS_ERROR);
    CHECK(fake.reads == 0); /* no read attempted after the trigger was not acknowledged */

    fake_reset(true, rx, 6);
    fake.ack_read = false; /* trigger accepted, readout not acknowledged */
    CHECK(ws_hdc3020_read(WS_HDC3020_ADDR, &t, &rh) == WS_SENSOR_BUS_ERROR);
}

int main(void)
{
    test_lm92_conversion();
    test_lm92_read();
    test_hdc3020_conversion();
    test_hdc3020_read();
    return check_report("test_sensors");
}
