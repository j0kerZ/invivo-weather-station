#ifndef WS_CURSOR_H
#define WS_CURSOR_H

/*
 * The upload cursor: the highest sequence number up to which every record has been acknowledged
 * by AWS IoT Core. Kept in flash so that a station that loses power while offline (a common case
 * on solar-powered farm stations) still uploads its backlog after it boots.
 *
 * Two flash sectors used as an append-only log of 8-byte entries (value, ~value). The current
 * value is the highest valid entry. When one sector is full the other is erased and used, so the
 * latest value is always in flash, even if power fails during the erase.
 */

#include <stdbool.h>
#include <stdint.h>

#define WS_CURSOR_SECTORS 2u
#define WS_CURSOR_ENTRY_SIZE 8u

typedef struct {
    uint32_t first_sector;
    uint32_t active;  /* 0 or 1: the sector being appended to */
    uint32_t used;    /* entries used in the active sector, including failed ones */
    bool has_value;
    uint32_t value;
} ws_cursor_t;

/* Boot scan of the two sectors starting at first_sector. Returns false on a flash read error. */
bool ws_cursor_mount(ws_cursor_t *c, uint32_t first_sector);

/* Stores `value` if it is higher than the current one. Returns false on a flash error. */
bool ws_cursor_save(ws_cursor_t *c, uint32_t value);

#endif
