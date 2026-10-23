#include "check.h"
#include "emu.h"
#include "ws_cursor.h"
#include "ws_hal.h"

#define FIRST 2u   /* cursor sectors 2 and 3 of a 4-sector flash */
#define PER_SECTOR (WS_FLASH_SECTOR_SIZE / WS_CURSOR_ENTRY_SIZE)

static void test_blank_save_and_remount(void)
{
    ws_cursor_t c;

    emu_flash_init(4);
    CHECK(ws_cursor_mount(&c, FIRST));
    CHECK(!c.has_value);

    CHECK(ws_cursor_save(&c, 10));
    CHECK(ws_cursor_save(&c, 11));
    CHECK(ws_cursor_save(&c, 5)); /* moving backwards is ignored, not an error */
    CHECK(c.value == 11);

    CHECK(ws_cursor_mount(&c, FIRST));
    CHECK(c.has_value && c.value == 11);
    CHECK(emu_flash_erases() == 0); /* nothing erased until a sector fills */
}

static void test_alternates_sectors(void)
{
    ws_cursor_t c;

    emu_flash_init(4);
    ws_cursor_mount(&c, FIRST);
    /* Two and a half sectors of entries: the sectors take turns. */
    for (uint32_t v = 1; v <= 5 * PER_SECTOR / 2; v++) {
        CHECK(ws_cursor_save(&c, v));
    }
    CHECK(emu_flash_erases() == 2);
    CHECK(ws_cursor_mount(&c, FIRST));
    CHECK(c.value == 5 * PER_SECTOR / 2);
    CHECK(ws_cursor_save(&c, c.value + 1));
    CHECK(ws_cursor_mount(&c, FIRST) && c.value == 5 * PER_SECTOR / 2 + 1);
}

/* Whatever byte the power fails at, the last successfully saved value survives a reboot. */
static void test_power_cuts(void)
{
    const size_t limit = 3 * WS_FLASH_SECTOR_SIZE; /* includes two sector switches */
    int failures_before = check_failures;

    for (size_t cut = 0; cut <= limit && check_failures == failures_before; cut++) {
        size_t from_boundary = cut % WS_FLASH_SECTOR_SIZE;
        if (from_boundary > 32 && from_boundary < WS_FLASH_SECTOR_SIZE - 32 && cut % 3 != 0) {
            continue; /* every byte near sector switches, every 3rd elsewhere (3 and 8 coprime) */
        }
        ws_cursor_t c;
        uint32_t last_ok = 0;

        emu_flash_init(4);
        ws_cursor_mount(&c, FIRST);
        emu_flash_power_cut_after(cut);
        for (uint32_t v = 1; !emu_flash_is_dead(); v++) {
            if (ws_cursor_save(&c, v)) {
                last_ok = v;
            }
        }

        emu_flash_power_restore();
        CHECK(ws_cursor_mount(&c, FIRST));
        /* The last saved value, or the one being saved when the power failed: a torn entry can
         * look complete when the unwritten bytes happen to be 0xFF already. Either is a value
         * that IoT Core acknowledged, so both are safe. */
        if (last_ok == 0) {
            CHECK(!c.has_value || c.value == 1);
        } else {
            CHECK(c.has_value && (c.value == last_ok || c.value == last_ok + 1));
        }
        CHECK(ws_cursor_save(&c, last_ok + 100));
        CHECK(ws_cursor_mount(&c, FIRST) && c.value == last_ok + 100);
        if (check_failures != failures_before) {
            fprintf(stderr, "  power cut after %zu bytes\n", cut);
        }
    }
}

int main(void)
{
    test_blank_save_and_remount();
    test_alternates_sectors();
    test_power_cuts();
    CHECK(emu_flash_violations() == 0);
    return check_report("test_cursor");
}
