/*
 * Emulated SPI NOR flash with the rules of the real part, and power-cut injection.
 *
 * Rule violations by the firmware (programming bits from 0 back to 1, crossing a page, going out
 * of range) are counted: on real flash they silently corrupt data, so the tests require zero.
 * Partial sector erases are not emulated; erases are atomic here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "ws_hal.h"

static uint8_t *mem;
static uint32_t sectors;
static size_t power_budget; /* bytes that can still be programmed before power fails */
static bool power_cut_armed;
static bool dead;
static bool fail_next;      /* next program writes only `fail_bytes` bytes; power stays on */
static size_t fail_bytes;
static bool fail_silently;  /* ...and still reports success, as a worn cell can */
static uint32_t violations;
static uint32_t erases;

void emu_flash_init(uint32_t sector_count)
{
    free(mem);
    sectors = sector_count;
    mem = malloc((size_t)sector_count * WS_FLASH_SECTOR_SIZE);
    if (mem == NULL) {
        fprintf(stderr, "emu_flash: out of memory\n");
        exit(1);
    }
    memset(mem, 0xFF, (size_t)sector_count * WS_FLASH_SECTOR_SIZE);
    power_cut_armed = dead = fail_next = false;
    erases = 0;
    /* `violations` is deliberately not reset: it accumulates over a whole test program. */
}

void emu_flash_fail_next_program(size_t bytes_written, bool reports_success)
{
    fail_next = true;
    fail_bytes = bytes_written;
    fail_silently = reports_success;
}

bool emu_flash_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false; /* no image yet: start blank */
    }
    size_t size = (size_t)sectors * WS_FLASH_SECTOR_SIZE;
    bool ok = fread(mem, 1, size, f) == size;
    fclose(f);
    return ok;
}

bool emu_flash_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    size_t size = (size_t)sectors * WS_FLASH_SECTOR_SIZE;
    bool ok = fwrite(mem, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

void emu_flash_power_cut_after(size_t bytes)
{
    power_cut_armed = true;
    power_budget = bytes;
}

void emu_flash_power_restore(void)
{
    power_cut_armed = dead = false;
}

bool emu_flash_is_dead(void)
{
    return dead;
}

uint8_t *emu_flash_memory(void)
{
    return mem;
}

uint32_t emu_flash_violations(void)
{
    return violations;
}

uint32_t emu_flash_erases(void)
{
    return erases;
}

static bool in_range(uint32_t addr, size_t len)
{
    return (uint64_t)addr + len <= (uint64_t)sectors * WS_FLASH_SECTOR_SIZE;
}

bool ws_hal_flash_read(uint32_t addr, uint8_t *data, size_t len)
{
    if (dead || !in_range(addr, len)) {
        return false;
    }
    memcpy(data, &mem[addr], len);
    return true;
}

bool ws_hal_flash_program(uint32_t addr, const uint8_t *data, size_t len)
{
    if (dead) {
        return false;
    }
    if (!in_range(addr, len) || addr / WS_FLASH_PAGE_SIZE != (addr + len - 1) / WS_FLASH_PAGE_SIZE) {
        violations++;
        return false;
    }
    size_t n = len;
    bool failed = false;
    if (fail_next) {
        n = fail_bytes < len ? fail_bytes : len; /* e.g. a worn cell: the write does not stick */
        fail_next = false;
        failed = !fail_silently;
    }
    if (power_cut_armed && power_budget < n) {
        n = power_budget; /* power fails part-way through this write */
        dead = true;
    }
    for (size_t i = 0; i < n; i++) {
        if ((mem[addr + i] & data[i]) != data[i]) {
            violations++; /* tried to set a 0 bit back to 1: needs an erase first */
        }
        mem[addr + i] &= data[i];
    }
    if (power_cut_armed) {
        power_budget -= n;
    }
    return !dead && !failed;
}

bool ws_hal_flash_erase_sector(uint32_t sector)
{
    if (dead) {
        return false;
    }
    if (sector >= sectors) {
        violations++;
        return false;
    }
    if (power_cut_armed && power_budget == 0) {
        dead = true; /* power failed before the erase started */
        return false;
    }
    memset(&mem[(size_t)sector * WS_FLASH_SECTOR_SIZE], 0xFF, WS_FLASH_SECTOR_SIZE);
    erases++;
    return true;
}
