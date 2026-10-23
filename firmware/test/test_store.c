/*
 * Flash log tests on the emulated NOR flash. Small flashes (3-4 sectors) make the ring wrap
 * quickly; the logic is the same for the 4096-sector production size.
 */

#include <string.h>

#include "check.h"
#include "emu.h"
#include "ws_hal.h"
#include "ws_store.h"

#define T0 1792713600u

/* A valid record whose content identifies its sequence number. */
static void make_record(uint32_t seq, uint8_t out[WS_RECORD_SIZE])
{
    int16_t t = (int16_t)(seq % 10000);
    ws_record_t r = {.ts = T0 + 60 * seq, .t_mean = t, .t_min = t, .t_max = t,
                     .rh_mean = 5000, .rh_min = 5000, .rh_max = 5000, .n_temp = 30, .n_rh = 30};
    ws_record_encode(&r, out);
}

static bool holds(const ws_store_t *st, uint32_t seq)
{
    uint8_t expected[WS_RECORD_SIZE], actual[WS_RECORD_SIZE];
    make_record(seq, expected);
    return ws_store_read(st, seq, actual) == WS_STORE_OK &&
           memcmp(expected, actual, WS_RECORD_SIZE) == 0;
}

static ws_store_status_t append(ws_store_t *st, uint32_t seq)
{
    uint8_t rec[WS_RECORD_SIZE];
    make_record(seq, rec);
    return ws_store_append(st, seq, rec);
}

/* Appends n records, doing the idle-time housekeeping after each, as the station does. */
static void append_n(ws_store_t *st, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        CHECK(append(st, ws_store_next_seq(st)) == WS_STORE_OK);
        CHECK(ws_store_service(st) == WS_STORE_OK);
    }
}

static void test_blank_flash(void)
{
    ws_store_t st;
    uint8_t rec[WS_RECORD_SIZE];

    emu_flash_init(4);
    CHECK(ws_store_mount(&st, 4, 1) == WS_STORE_OK);
    CHECK(ws_store_next_seq(&st) == 1);
    CHECK(st.valid_sectors == 0);
    CHECK(ws_store_read(&st, 1, rec) == WS_STORE_NOT_FOUND);
    CHECK(ws_store_mount(&st, 1, 1) == WS_STORE_ERR_CONFIG);
}

static void test_append_read_and_reboot(void)
{
    ws_store_t st;
    uint8_t rec[WS_RECORD_SIZE];

    emu_flash_init(4);
    ws_store_mount(&st, 4, 1);
    append_n(&st, 300); /* crosses from sector 0 into sector 1 */
    CHECK(ws_store_next_seq(&st) == 301);
    CHECK(append(&st, 999) == WS_STORE_ERR_SEQ);
    CHECK(ws_store_read(&st, 0, rec) == WS_STORE_NOT_FOUND);
    CHECK(ws_store_read(&st, 301, rec) == WS_STORE_NOT_FOUND);

    /* Reboot: all state is rebuilt from flash. */
    CHECK(ws_store_mount(&st, 4, 1) == WS_STORE_OK);
    CHECK(ws_store_next_seq(&st) == 301);
    CHECK(st.valid_sectors == 2 && st.torn_records == 0);
    for (uint32_t seq = 1; seq <= 300; seq++) {
        CHECK(holds(&st, seq));
    }
    append_n(&st, 5);
    CHECK(holds(&st, 305));
}

static void test_ring_overwrites_oldest(void)
{
    const uint32_t sectors = 4;
    ws_store_t st;
    uint8_t rec[WS_RECORD_SIZE];

    emu_flash_init(sectors);
    ws_store_mount(&st, sectors, 1);
    append_n(&st, 2 * sectors * WS_STORE_RECORDS_PER_SECTOR + 17); /* around the ring twice */

    /* One sector is always kept erased ahead, so N-2 full sectors plus the head remain. */
    uint32_t next = ws_store_next_seq(&st);
    CHECK(next - st.oldest_seq == (sectors - 2) * WS_STORE_RECORDS_PER_SECTOR + st.head_used);
    CHECK(ws_store_read(&st, st.oldest_seq - 1, rec) == WS_STORE_NOT_FOUND);
    for (uint32_t seq = st.oldest_seq; seq < next; seq++) {
        CHECK(holds(&st, seq));
    }

    uint32_t oldest = st.oldest_seq;
    CHECK(ws_store_mount(&st, sectors, 1) == WS_STORE_OK);
    CHECK(ws_store_next_seq(&st) == next);
    CHECK(st.oldest_seq == oldest);
}

static void test_corruption_is_detected(void)
{
    ws_store_t st;
    uint8_t rec[WS_RECORD_SIZE];

    emu_flash_init(4);
    ws_store_mount(&st, 4, 1);
    append_n(&st, 3);
    /* Bit rot in record 2's temperature: slot 1 of sector 0, byte 4 */
    emu_flash_memory()[WS_STORE_HEADER_SIZE + WS_RECORD_SIZE + 4] ^= 0x01;
    CHECK(ws_store_read(&st, 2, rec) == WS_STORE_CORRUPT);
    CHECK(holds(&st, 1) && holds(&st, 3));
}

/*
 * A write that fails while power stays on (e.g. a worn cell) must be reported, even when the
 * flash itself claims success, and must not stop the log.
 */
static void test_failed_write_with_power_on(void)
{
    ws_store_t st;
    uint8_t rec[WS_RECORD_SIZE];

    for (int case_no = 0; case_no < 6; case_no++) {
        bool silent = case_no >= 3;
        size_t written = (size_t)(case_no % 3) * 7; /* 0, 7 or 14 of the 20 bytes stick */

        emu_flash_init(4);
        ws_store_mount(&st, 4, 1);
        append_n(&st, 5);

        emu_flash_fail_next_program(written, silent);
        CHECK(append(&st, 6) == WS_STORE_ERR_FLASH);
        /* The failed slot is used up: a partly programmed slot cannot be programmed again. */
        CHECK(append(&st, 7) == WS_STORE_OK);
        CHECK(holds(&st, 7));
        CHECK(ws_store_read(&st, 6, rec) ==
              (written == 0 ? WS_STORE_NOT_FOUND : WS_STORE_CORRUPT));

        /* After a reboot the scan sees past the failed slot, even a blank one. */
        CHECK(ws_store_mount(&st, 4, 1) == WS_STORE_OK);
        CHECK(ws_store_next_seq(&st) == 8);
        CHECK(holds(&st, 5) && holds(&st, 7));
    }
}

/* ws_store_service() keeps erases off the append path, but correctness must not depend on it. */
static void test_works_without_service(void)
{
    const uint32_t sectors = 3;
    ws_store_t st;

    emu_flash_init(sectors);
    ws_store_mount(&st, sectors, 1);
    for (uint32_t i = 0; i < 3 * sectors * WS_STORE_RECORDS_PER_SECTOR; i++) {
        CHECK(append(&st, ws_store_next_seq(&st)) == WS_STORE_OK);
    }
    for (uint32_t seq = st.oldest_seq; seq < ws_store_next_seq(&st); seq++) {
        CHECK(holds(&st, seq));
    }
}

/*
 * Each sector takes exactly 4096 programmed bytes (header + 204 records), so sector changes, with
 * their header writes and erases, happen near multiples of 4096. Cut at every byte there, and at
 * every 7th byte elsewhere: 7 shares no factor with the 20-byte record, so the sweep still cuts
 * at every offset inside a record.
 */
static bool interesting_cut(size_t cut)
{
    size_t from_boundary = cut % WS_FLASH_SECTOR_SIZE;
    return from_boundary < 64 || from_boundary > WS_FLASH_SECTOR_SIZE - 64 || cut % 7 == 0;
}

/*
 * The crash-safety property: wherever the power fails, after a reboot every record whose append
 * reported success is intact, and numbering continues after the last record written.
 */
static void test_power_cuts(void)
{
    const uint32_t sectors = 3;
    const size_t limit = (size_t)(sectors + 1) * WS_FLASH_SECTOR_SIZE; /* wraps the ring */
    uint32_t failures_before = (uint32_t)check_failures;

    for (size_t cut = 0; cut <= limit && check_failures == (int)failures_before; cut++) {
        if (!interesting_cut(cut)) {
            continue;
        }
        ws_store_t st;
        uint32_t last_ok = 0;

        emu_flash_init(sectors);
        ws_store_mount(&st, sectors, 1);
        emu_flash_power_cut_after(cut);
        while (!emu_flash_is_dead()) {
            uint32_t seq = ws_store_next_seq(&st);
            if (append(&st, seq) == WS_STORE_OK) {
                last_ok = seq;
            }
            ws_store_service(&st);
        }

        emu_flash_power_restore();
        CHECK(ws_store_mount(&st, sectors, 1) == WS_STORE_OK);
        CHECK(ws_store_next_seq(&st) > last_ok);
        CHECK(st.torn_records <= 1);
        for (uint32_t seq = st.oldest_seq; seq <= last_ok; seq++) {
            CHECK(holds(&st, seq));
        }

        /* The log keeps working after the reboot, including across the next sector change,
         * which is where a skipped erase would show up. */
        append_n(&st, WS_STORE_RECORDS_PER_SECTOR + 3);
        CHECK(holds(&st, ws_store_next_seq(&st) - 1));
        CHECK(emu_flash_violations() == 0);
        if (check_failures != (int)failures_before) {
            fprintf(stderr, "  power cut after %zu bytes\n", cut);
        }
    }
}

int main(void)
{
    test_blank_flash();
    test_append_read_and_reboot();
    test_ring_overwrites_oldest();
    test_corruption_is_detected();
    test_failed_write_with_power_on();
    test_works_without_service();
    test_power_cuts();
    CHECK(emu_flash_violations() == 0);
    return check_report("test_store");
}
