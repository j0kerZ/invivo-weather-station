#ifndef WS_HAL_H
#define WS_HAL_H

/*
 * Hardware abstraction layer: everything the portable firmware needs from the board.
 *
 * On the station these are implemented by the Cortex-M3's I2C peripheral driver and a timer.
 * On a PC they are implemented by the emulator (firmware/emu), and in unit tests by fakes. The
 * firmware above this line is identical in all three.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 7-bit addresses. Each function returns false if the device does not acknowledge. */
bool ws_hal_i2c_write(uint8_t addr, const uint8_t *tx, size_t tx_len);
bool ws_hal_i2c_read(uint8_t addr, uint8_t *rx, size_t rx_len);
/* Write then read with a repeated START, e.g. set a register pointer and read the register. */
bool ws_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                           size_t rx_len);

void ws_hal_delay_ms(uint32_t ms);

/*
 * SPI NOR flash. Erased bytes read 0xFF, programming can only clear bits (1 -> 0), and one
 * program operation must stay inside a 256-byte page. Each function returns false on failure.
 */
#define WS_FLASH_SECTOR_SIZE 4096u
#define WS_FLASH_PAGE_SIZE 256u

bool ws_hal_flash_read(uint32_t addr, uint8_t *data, size_t len);
bool ws_hal_flash_program(uint32_t addr, const uint8_t *data, size_t len);
bool ws_hal_flash_erase_sector(uint32_t sector);

#endif
