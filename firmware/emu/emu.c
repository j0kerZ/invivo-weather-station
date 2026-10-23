#include "emu.h"

#include <math.h>
#include <stdbool.h>

#include "ws_crc.h"
#include "ws_hal.h"
#include "ws_sensors.h"

static const double PI = 3.14159265358979323846;

static emu_config_t config;
static uint64_t rng_state;
static uint32_t now;

/* ---- Deterministic randomness (xorshift64*): the same seed gives the same run ---- */

static double rng_unit(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (double)((rng_state * 2685821657736338717ull) >> 11) / 9007199254740992.0;
}

static double rng_gauss(double sigma)
{
    double u1 = rng_unit() + 1e-12;
    double u2 = rng_unit();
    return sigma * sqrt(-2.0 * log(u1)) * cos(2.0 * PI * u2);
}

static bool chance(double p)
{
    return p > 0.0 && rng_unit() < p;
}

/* ---- The air ---- */

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void emu_world_at(uint32_t ts, double *temp_c, double *rh_pct)
{
    double hour = (double)(ts % 86400u) / 3600.0;
    bool frost = config.scenario == EMU_SCENARIO_FROST;
    double mean = frost ? 3.0 : 14.0;
    double amplitude = frost ? 6.0 : 7.0;

    /* Daily cycle: warmest at 15:00, coldest at 03:00. */
    double base = mean + amplitude * cos(2.0 * PI * (hour - 15.0) / 24.0);

    /* Rain shower (normal scenario): 3 °C colder and 20 %RH wetter within 2 minutes of 13:00,
     * recovering over 10 minutes from 13:30. Produces "large change" events. */
    double shower = 0.0;
    if (config.scenario == EMU_SCENARIO_NORMAL) {
        double onset = clamp((hour - 13.0) * 30.0, 0.0, 1.0);       /* 2 min ramp */
        double recovery = clamp((hour - 13.5) * 6.0, 0.0, 1.0);     /* 10 min ramp */
        shower = onset * (1.0 - recovery);
    }

    *temp_c = base - 3.0 * shower;
    *rh_pct = clamp(70.0 - 3.0 * (base - mean) + 20.0 * shower, 5.0, 100.0);
}

/* ---- Emulated LM92: free-running, 13-bit two's complement, 0.0625 °C per count ---- */

static uint8_t lm92_pointer;

static bool lm92_read(uint8_t *rx, size_t len)
{
    if (len != 2 || lm92_pointer != 0x00 || chance(config.i2c_error_rate)) {
        return false;
    }
    double t, rh;
    emu_world_at(now, &t, &rh);
    long counts = lround((t + rng_gauss(0.05)) / 0.0625);
    uint16_t raw = (uint16_t)((uint32_t)(counts * 8) & 0xFFF8u); /* status bits 0 */
    rx[0] = (uint8_t)(raw >> 8);
    rx[1] = (uint8_t)raw;
    return true;
}

/* ---- Emulated HDC3020: trigger-on-demand LPM0 only ---- */

static bool hdc_has_result;
static uint16_t hdc_t_raw, hdc_rh_raw;

static bool hdc_write(const uint8_t *tx, size_t len)
{
    if (len != 2 || tx[0] != 0x24 || tx[1] != 0x00 || chance(config.i2c_error_rate)) {
        return false; /* only the command the driver uses is emulated */
    }
    double t, rh;
    emu_world_at(now, &t, &rh);
    hdc_t_raw = (uint16_t)lround(clamp((t + 45.0) / 175.0, 0.0, 1.0) * 65535.0);
    hdc_rh_raw = (uint16_t)lround(clamp((rh + rng_gauss(0.2)) / 100.0, 0.0, 1.0) * 65535.0);
    hdc_has_result = true;
    return true;
}

static bool hdc_read(uint8_t *rx, size_t len)
{
    if (len != 6 || !hdc_has_result) {
        return false; /* datasheet: NACK when no measurement is ready */
    }
    hdc_has_result = false;
    rx[0] = (uint8_t)(hdc_t_raw >> 8);
    rx[1] = (uint8_t)hdc_t_raw;
    rx[2] = ws_crc8(&rx[0], 2);
    rx[3] = (uint8_t)(hdc_rh_raw >> 8);
    rx[4] = (uint8_t)hdc_rh_raw;
    rx[5] = ws_crc8(&rx[3], 2);
    if (chance(config.crc_error_rate)) {
        rx[4] ^= 0x04; /* a bit flipped on the bus */
    }
    return true;
}

/* ---- HAL implementation used by the firmware ---- */

bool ws_hal_i2c_write(uint8_t addr, const uint8_t *tx, size_t tx_len)
{
    if (addr == WS_LM92_ADDR) {
        if (tx_len != 1) return false;
        lm92_pointer = tx[0];
        return true;
    }
    if (addr == WS_HDC3020_ADDR) {
        return hdc_write(tx, tx_len);
    }
    return false;
}

bool ws_hal_i2c_read(uint8_t addr, uint8_t *rx, size_t rx_len)
{
    if (addr == WS_LM92_ADDR) return lm92_read(rx, rx_len);
    if (addr == WS_HDC3020_ADDR) return hdc_read(rx, rx_len);
    return false;
}

bool ws_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                           size_t rx_len)
{
    return ws_hal_i2c_write(addr, tx, tx_len) && ws_hal_i2c_read(addr, rx, rx_len);
}

void ws_hal_delay_ms(uint32_t ms)
{
    (void)ms; /* emulated time only advances between ticks */
}

/* ---- Running ---- */

void emu_init(const emu_config_t *cfg)
{
    config = *cfg;
    rng_state = cfg->seed ? cfg->seed : 1;
    lm92_pointer = 0x00; /* power-on default: temperature register */
    hdc_has_result = false;
}

void emu_step(ws_station_t *st, uint32_t t)
{
    now = t;
    ws_station_tick(st, t, true);
}

bool emu_run(ws_station_t *st, uint32_t start, uint32_t duration_s)
{
    /* `<=` so the final window is closed by the first sample of the next one. */
    for (uint32_t t = start; t - start <= duration_s; t += WS_SAMPLE_PERIOD_S) {
        emu_step(st, t);
        if (emu_flash_is_dead()) {
            return false;
        }
    }
    return true;
}
