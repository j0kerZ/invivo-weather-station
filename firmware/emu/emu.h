#ifndef EMU_H
#define EMU_H

/*
 * PC emulation of the station's hardware: the air (a weather model), the LM92 and HDC3020 at the
 * I2C byte level, and the HAL functions the firmware calls. Host-only code; floating point is fine
 * here because it models the physical world, not the firmware.
 */

#include <stdint.h>

#include "ws_station.h"

typedef enum {
    EMU_SCENARIO_NORMAL, /* autumn day, 7..21 °C, rain shower at 13:00 */
    EMU_SCENARIO_FROST,  /* cold night, -3..9 °C: frost from evening to morning */
} emu_scenario_t;

typedef struct {
    emu_scenario_t scenario;
    uint64_t seed;
    double i2c_error_rate; /* probability that a sensor transaction is not acknowledged */
    double crc_error_rate; /* probability that an HDC3020 readout is corrupted on the bus */
} emu_config_t;

void emu_init(const emu_config_t *cfg);

/* The air conditions at a time, without sensor noise. */
void emu_world_at(uint32_t ts, double *temp_c, double *rh_pct);

/* Runs the station firmware from `start` for `duration_s` seconds of emulated time. */
void emu_run(ws_station_t *st, uint32_t start, uint32_t duration_s);

#endif
