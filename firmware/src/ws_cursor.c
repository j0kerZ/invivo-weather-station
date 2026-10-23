#include "ws_cursor.h"

#include "ws_hal.h"

#define ENTRIES_PER_SECTOR (WS_FLASH_SECTOR_SIZE / WS_CURSOR_ENTRY_SIZE)

static uint32_t entry_addr(const ws_cursor_t *c, uint32_t sector, uint32_t slot)
{
    return (c->first_sector + sector) * WS_FLASH_SECTOR_SIZE + slot * WS_CURSOR_ENTRY_SIZE;
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* An entry is the value followed by its bitwise complement: a torn write cannot match. */
static bool decode(const uint8_t e[WS_CURSOR_ENTRY_SIZE], uint32_t *value)
{
    uint32_t v = get_u32le(&e[0]);
    if (get_u32le(&e[4]) != ~v) {
        return false;
    }
    *value = v;
    return true;
}

static bool is_blank(const uint8_t e[WS_CURSOR_ENTRY_SIZE])
{
    for (uint32_t i = 0; i < WS_CURSOR_ENTRY_SIZE; i++) {
        if (e[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

bool ws_cursor_mount(ws_cursor_t *c, uint32_t first_sector)
{
    uint32_t used[WS_CURSOR_SECTORS] = {0};

    *c = (ws_cursor_t){.first_sector = first_sector};
    for (uint32_t s = 0; s < WS_CURSOR_SECTORS; s++) {
        for (uint32_t slot = 0; slot < ENTRIES_PER_SECTOR; slot++) {
            uint8_t e[WS_CURSOR_ENTRY_SIZE];
            uint32_t v;
            if (!ws_hal_flash_read(entry_addr(c, s, slot), e, sizeof e)) {
                return false;
            }
            if (is_blank(e)) {
                continue;
            }
            used[s] = slot + 1;
            if (decode(e, &v) && (!c->has_value || v > c->value)) {
                c->has_value = true;
                c->value = v;
                c->active = s;
            }
        }
    }
    c->used = used[c->active];
    return true;
}

bool ws_cursor_save(ws_cursor_t *c, uint32_t value)
{
    if (c->has_value && value <= c->value) {
        return true; /* the cursor only moves forward */
    }
    if (c->used == ENTRIES_PER_SECTOR) {
        /* Switch sectors. The full sector keeps the latest value until the new entry is written,
         * so a power cut during the erase loses nothing. */
        uint32_t other = 1 - c->active;
        if (!ws_hal_flash_erase_sector(c->first_sector + other)) {
            return false;
        }
        c->active = other;
        c->used = 0;
    }

    uint8_t e[WS_CURSOR_ENTRY_SIZE];
    for (uint32_t i = 0; i < 4; i++) {
        e[i] = (uint8_t)(value >> (8 * i));
        e[4 + i] = (uint8_t)(~value >> (8 * i));
    }
    uint32_t addr = entry_addr(c, c->active, c->used++); /* consumed even if the write fails */
    uint8_t check[WS_CURSOR_ENTRY_SIZE];
    uint32_t stored;
    if (!ws_hal_flash_program(addr, e, sizeof e) || !ws_hal_flash_read(addr, check, sizeof check) ||
        !decode(check, &stored) || stored != value) {
        return false;
    }
    c->has_value = true;
    c->value = value;
    return true;
}
