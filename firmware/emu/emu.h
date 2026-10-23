#ifndef EMU_H
#define EMU_H

/*
 * PC emulation of the station's hardware: the air (a weather model), the LM92 and HDC3020 at the
 * I2C byte level, and the HAL functions the firmware calls. Host-only code; floating point is fine
 * here because it models the physical world, not the firmware.
 */

#include <stdbool.h>
#include <stddef.h>
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

/*
 * Runs the station firmware from `start` for `duration_s` seconds of emulated time. Stops early,
 * returning false, if the emulated flash loses power.
 */
bool emu_run(ws_station_t *st, uint32_t start, uint32_t duration_s);

/* One station tick at emulated time t. */
void emu_step(ws_station_t *st, uint32_t t);

/* ---- Emulated SPI NOR flash (emu_flash.c) ---- */

void emu_flash_init(uint32_t sector_count); /* blank: every byte 0xFF */
bool emu_flash_load(const char *path);      /* false if there is no image to load */
bool emu_flash_save(const char *path);

/* Power fails after `bytes` more bytes are programmed (0 = before the next write or erase). */
void emu_flash_power_cut_after(size_t bytes);
void emu_flash_power_restore(void);
bool emu_flash_is_dead(void);

/*
 * The next program operation writes only `bytes_written` bytes; power stays on. It reports the
 * failure, or, if `reports_success`, claims success as a worn cell can (only a read-back shows it).
 */
void emu_flash_fail_next_program(size_t bytes_written, bool reports_success);

uint8_t *emu_flash_memory(void);      /* raw contents, for corruption tests */
uint32_t emu_flash_violations(void);  /* NOR rule violations by the firmware: must stay 0 */
uint32_t emu_flash_erases(void);

#endif
