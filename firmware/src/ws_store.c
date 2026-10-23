#include "ws_store.h"

#include <string.h>

#include "ws_crc.h"
#include "ws_hal.h"

static const uint8_t MAGIC[4] = {'W', 'S', 'R', '1'};

typedef struct {
    uint32_t seq_first;
    uint32_t generation;
} header_t;

static uint32_t sector_addr(uint32_t sector)
{
    return sector * WS_FLASH_SECTOR_SIZE;
}

static uint32_t slot_addr(uint32_t sector, uint32_t slot)
{
    return sector_addr(sector) + WS_STORE_HEADER_SIZE + slot * WS_RECORD_SIZE;
}

static bool is_blank(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* Programs any length, split at page boundaries, then reads back to verify. */
static bool program_verified(uint32_t addr, const uint8_t *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        uint32_t at = addr + (uint32_t)done;
        size_t room = WS_FLASH_PAGE_SIZE - at % WS_FLASH_PAGE_SIZE;
        size_t chunk = len - done < room ? len - done : room;
        if (!ws_hal_flash_program(at, &data[done], chunk)) {
            return false;
        }
        done += chunk;
    }

    uint8_t check[WS_STORE_HEADER_SIZE > WS_RECORD_SIZE ? WS_STORE_HEADER_SIZE : WS_RECORD_SIZE];
    return len <= sizeof check && ws_hal_flash_read(addr, check, len) &&
           memcmp(check, data, len) == 0;
}

static bool read_header(uint32_t sector, header_t *h)
{
    uint8_t b[WS_STORE_HEADER_SIZE];

    if (!ws_hal_flash_read(sector_addr(sector), b, sizeof b) || memcmp(b, MAGIC, 4) != 0 ||
        ws_crc16(b, 14) != (uint16_t)(b[14] | (b[15] << 8))) {
        return false;
    }
    h->seq_first = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) |
                   ((uint32_t)b[7] << 24);
    h->generation = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) |
                    ((uint32_t)b[11] << 24);
    return true;
}

static bool write_header(uint32_t sector, const header_t *h)
{
    uint8_t b[WS_STORE_HEADER_SIZE] = {0};

    memcpy(b, MAGIC, 4);
    for (int i = 0; i < 4; i++) {
        b[4 + i] = (uint8_t)(h->seq_first >> (8 * i));
        b[8 + i] = (uint8_t)(h->generation >> (8 * i));
    }
    uint16_t crc = ws_crc16(b, 14);
    b[14] = (uint8_t)crc;
    b[15] = (uint8_t)(crc >> 8);
    return program_verified(sector_addr(sector), b, sizeof b);
}

/* Erases the sector after the head. If it held data, that was the oldest data in the log. */
static bool erase_ahead(ws_store_t *st)
{
    uint32_t next = st->has_head ? (st->head + 1) % st->sector_count : 0;
    header_t h;

    if (read_header(next, &h) && h.seq_first + WS_STORE_RECORDS_PER_SECTOR > st->oldest_seq) {
        st->oldest_seq = h.seq_first + WS_STORE_RECORDS_PER_SECTOR;
    }
    if (!ws_hal_flash_erase_sector(next)) {
        return false;
    }
    st->erase_ahead_pending = false;
    return true;
}

static ws_store_status_t open_next_sector(ws_store_t *st)
{
    uint32_t next = st->has_head ? (st->head + 1) % st->sector_count : 0;
    uint32_t seq_first = ws_store_next_seq(st);

    /* Normally erased already by ws_store_service(); if not, the append pays for it now. */
    if (st->erase_ahead_pending && !erase_ahead(st)) {
        return WS_STORE_ERR_FLASH;
    }
    const uint32_t ring_records = st->sector_count * WS_STORE_RECORDS_PER_SECTOR;
    header_t h = {.seq_first = seq_first, .generation = (seq_first - st->first_seq) / ring_records};
    if (!write_header(next, &h)) {
        return WS_STORE_ERR_FLASH;
    }
    st->has_head = true;
    st->head = next;
    st->head_seq_first = seq_first;
    st->head_used = 0;
    st->erase_ahead_pending = true; /* the sector after the new head */
    return WS_STORE_OK;
}

ws_store_status_t ws_store_mount(ws_store_t *st, uint32_t sector_count, uint32_t first_seq)
{
    if (sector_count < 2) {
        return WS_STORE_ERR_CONFIG;
    }
    *st = (ws_store_t){.sector_count = sector_count, .first_seq = first_seq,
                       .oldest_seq = first_seq, .erase_ahead_pending = true};

    /* The head is the valid sector with the highest seq_first; the oldest has the lowest. */
    for (uint32_t s = 0; s < sector_count; s++) {
        header_t h;
        if (!read_header(s, &h)) {
            continue;
        }
        if (!st->has_head || h.seq_first > st->head_seq_first) {
            st->has_head = true;
            st->head = s;
            st->head_seq_first = h.seq_first;
        }
        if (st->valid_sectors == 0 || h.seq_first < st->oldest_seq) {
            st->oldest_seq = h.seq_first;
        }
        st->valid_sectors++;
    }
    if (!st->has_head) {
        return WS_STORE_OK; /* blank flash: sector 0 is erased and opened on first append */
    }

    /* Used slots in the head end at the last slot that is not blank. */
    for (uint32_t slot = 0; slot < WS_STORE_RECORDS_PER_SECTOR; slot++) {
        uint8_t rec[WS_RECORD_SIZE];
        if (!ws_hal_flash_read(slot_addr(st->head, slot), rec, sizeof rec)) {
            return WS_STORE_ERR_FLASH;
        }
        if (is_blank(rec, sizeof rec)) {
            continue;
        }
        st->head_used = slot + 1;
        if (ws_crc8(rec, WS_RECORD_CRC_OFFSET) != rec[WS_RECORD_CRC_OFFSET]) {
            st->torn_records++;
        }
    }
    /* erase_ahead_pending stays true: a power cut may have interrupted the last erase-ahead. */
    return WS_STORE_OK;
}

uint32_t ws_store_next_seq(const ws_store_t *st)
{
    return st->has_head ? st->head_seq_first + st->head_used : st->first_seq;
}

ws_store_status_t ws_store_append(ws_store_t *st, uint32_t seq,
                                  const uint8_t record[WS_RECORD_SIZE])
{
    if (seq != ws_store_next_seq(st)) {
        return WS_STORE_ERR_SEQ;
    }
    if (!st->has_head || st->head_used == WS_STORE_RECORDS_PER_SECTOR) {
        ws_store_status_t status = open_next_sector(st);
        if (status != WS_STORE_OK) {
            return status;
        }
    }

    /* The slot is consumed even if programming fails: a partly programmed slot cannot be
     * rewritten without erasing the whole sector, and the boot scan would skip it anyway. */
    uint32_t addr = slot_addr(st->head, st->head_used++);
    return program_verified(addr, record, WS_RECORD_SIZE) ? WS_STORE_OK : WS_STORE_ERR_FLASH;
}

ws_store_status_t ws_store_service(ws_store_t *st)
{
    if (!st->erase_ahead_pending) {
        return WS_STORE_OK;
    }
    return erase_ahead(st) ? WS_STORE_OK : WS_STORE_ERR_FLASH;
}

ws_store_status_t ws_store_read(const ws_store_t *st, uint32_t seq,
                                uint8_t record[WS_RECORD_SIZE])
{
    if (!st->has_head || seq < st->oldest_seq || seq >= ws_store_next_seq(st)) {
        return WS_STORE_NOT_FOUND;
    }

    /* Sectors hold consecutive runs of 204 sequence numbers, in ring order behind the head. */
    uint32_t back = seq >= st->head_seq_first
                        ? 0
                        : (st->head_seq_first - seq + WS_STORE_RECORDS_PER_SECTOR - 1) /
                              WS_STORE_RECORDS_PER_SECTOR;
    if (back >= st->sector_count) {
        return WS_STORE_NOT_FOUND;
    }
    uint32_t sector = (st->head + st->sector_count - back) % st->sector_count;

    header_t h;
    if (!read_header(sector, &h) || seq < h.seq_first ||
        seq - h.seq_first >= WS_STORE_RECORDS_PER_SECTOR) {
        return WS_STORE_NOT_FOUND;
    }
    if (!ws_hal_flash_read(slot_addr(sector, seq - h.seq_first), record, WS_RECORD_SIZE)) {
        return WS_STORE_ERR_FLASH;
    }
    if (is_blank(record, WS_RECORD_SIZE)) {
        return WS_STORE_NOT_FOUND;
    }
    if (ws_crc8(record, WS_RECORD_CRC_OFFSET) != record[WS_RECORD_CRC_OFFSET]) {
        return WS_STORE_CORRUPT;
    }
    return WS_STORE_OK;
}
