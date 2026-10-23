#ifndef WS_STORE_H
#define WS_STORE_H

/*
 * Circular record log on SPI NOR flash (docs/PROTOCOL.md §6): 365 days of history that survives
 * power cuts. A record's position in flash implies its sequence number, so sequence numbers
 * survive reboots without a separate counter.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ws_protocol.h"

#define WS_STORE_HEADER_SIZE 16u
#define WS_STORE_RECORDS_PER_SECTOR 204u /* 16 + 204 * 20 = 4096 */
#define WS_STORE_SECTORS_16MB 4096u

typedef enum {
    WS_STORE_OK = 0,
    WS_STORE_ERR_FLASH,     /* flash operation or read-back verification failed */
    WS_STORE_ERR_SEQ,       /* caller's sequence number is not the next one */
    WS_STORE_ERR_CONFIG,    /* fewer than 2 sectors */
    WS_STORE_NOT_FOUND,     /* never written, overwritten, or lost to a power cut */
    WS_STORE_CORRUPT,       /* slot written but its CRC fails (torn write or bit rot) */
} ws_store_status_t;

typedef struct {
    uint32_t sector_count;
    bool has_head;
    uint32_t head;            /* sector being filled */
    uint32_t head_seq_first;  /* sequence number of the head's slot 0 */
    uint32_t head_used;       /* slots used in the head, including torn ones */
    uint32_t oldest_seq;      /* lowest sequence number still in flash */
    uint32_t first_seq;       /* sequence number used on a blank flash */
    bool erase_ahead_pending; /* the sector after the head still needs erasing */

    /* Found by the boot scan */
    uint32_t valid_sectors;
    uint32_t torn_records;
} ws_store_t;

/* Boot scan. On a blank flash, numbering starts at first_seq. */
ws_store_status_t ws_store_mount(ws_store_t *st, uint32_t sector_count, uint32_t first_seq);

uint32_t ws_store_next_seq(const ws_store_t *st);

/* Appends the record with sequence number `seq`, which must equal ws_store_next_seq(). */
ws_store_status_t ws_store_append(ws_store_t *st, uint32_t seq,
                                  const uint8_t record[WS_RECORD_SIZE]);

/* Housekeeping for idle time: erases the next sector ahead of need. Never required for
 * correctness; it keeps erases off the latency-critical append path. */
ws_store_status_t ws_store_service(ws_store_t *st);

ws_store_status_t ws_store_read(const ws_store_t *st, uint32_t seq,
                                uint8_t record[WS_RECORD_SIZE]);

#endif
