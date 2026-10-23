/*
 * ws-emulator: runs the station firmware against emulated sensors and prints every record.
 *
 *   ws-emulator [--scenario normal|frost] [--hours N] [--start UNIX_TS] [--seed N]
 *               [--i2c-errors P] [--crc-errors P] [--hex] [--quiet]
 *               [--flash FILE] [--sectors N] [--power-cut-after MINUTES]
 *               [--mqtt STATION_ID [--certs DIR] [--speed X] [--outage AT_MIN:FOR_MIN]
 *                [--authz-check OTHER_STATION_ID]]
 *
 * With --flash, the emulated flash is loaded from and saved to FILE, so a second run continues
 * where the first stopped, like a station rebooting.
 *
 * With --mqtt, the station runs in real time (or --speed times faster) and publishes to AWS IoT
 * Core with its X.509 certificate from --certs (default: certs).
 */

#define _POSIX_C_SOURCE 200809L /* gmtime_r */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu.h"
#include "online.h"
#include "ws_cursor.h"
#include "ws_store.h"
#include "ws_uplink.h"

typedef struct {
    bool hex;
    bool quiet;
    ws_store_t *store;
    ws_uplink_t *uplink; /* NULL when not publishing */
    uint32_t aggregates, events, flagged, bad_frames, store_errors;
    size_t bytes;
} run_t;

static void print_pair(const char *label, int32_t value, bool valid, const char *unit)
{
    if (valid) {
        printf("%s %7.2f%s", label, value / 100.0, unit);
    } else {
        printf("%s %7s%s", label, "--", unit);
    }
}

static void on_record(void *ctx, uint32_t seq, const uint8_t record[WS_RECORD_SIZE])
{
    run_t *run = ctx;
    uint8_t frame[WS_FRAME_SIZE(1)];
    size_t len = 0;
    ws_frame_t decoded;
    ws_record_t r;

    /* Store first, then publish: nothing is published that a power cut could lose. */
    if (ws_store_append(run->store, seq, record) != WS_STORE_OK) {
        run->store_errors++;
        if (emu_flash_is_dead()) {
            return; /* the station has lost power: nothing is published */
        }
    }
    if (run->uplink != NULL) {
        ws_uplink_record_stored(run->uplink, seq, record); /* published now if connected */
    }

    /* Exactly what the device would publish live: a one-record frame. Decode it as a self-check. */
    if (ws_frame_encode(0, seq, record, 1, frame, sizeof frame, &len) != WS_OK ||
        ws_frame_decode(frame, len, &decoded, NULL) != WS_OK ||
        ws_record_decode(decoded.records, &r) != WS_OK) {
        run->bad_frames++;
        return;
    }
    run->bytes += len;
    bool event = (r.flags & WS_FLAG_EVENT) != 0;
    if (event) {
        run->events++;
    } else {
        run->aggregates++;
    }
    if (r.flags & ~WS_FLAG_EVENT) {
        run->flagged++;
    }
    if (run->quiet) {
        return;
    }

    char when[32];
    time_t t = (time_t)r.ts;
    struct tm tm;
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&t, &tm));

    printf("%s  #%-5lu %-5s", when, (unsigned long)seq, event ? "EVENT" : "AGG");
    if (event) {
        print_pair("  T", r.t_mean, r.n_temp > 0, " °C");
        print_pair("   RH", r.rh_mean, r.n_rh > 0, " %");
    } else {
        print_pair("  T", r.t_min, r.n_temp > 0, "");
        print_pair(" /", r.t_mean, r.n_temp > 0, "");
        print_pair(" /", r.t_max, r.n_temp > 0, " °C");
        print_pair("   RH", r.rh_min, r.n_rh > 0, "");
        print_pair(" /", r.rh_mean, r.n_rh > 0, "");
        print_pair(" /", r.rh_max, r.n_rh > 0, " %");
        printf("   n %2u/%2u", r.n_temp, r.n_rh);
    }
    if (r.flags & WS_FLAG_TEMP_ERROR) printf("  TEMP_ERROR");
    if (r.flags & WS_FLAG_RH_ERROR) printf("  RH_ERROR");
    if (r.flags & WS_FLAG_TIME_UNSYNCED) printf("  TIME_UNSYNCED");
    printf("\n");

    if (run->hex) {
        printf("    ");
        for (size_t i = 0; i < len; i++) {
            printf("%02x", frame[i]);
        }
        printf("\n");
    }

    /* Idle time after publishing: erase the next flash sector ahead of need. */
    if (ws_store_service(run->store) != WS_STORE_OK) {
        run->store_errors++;
    }
}

/* After a reboot, continue a minute after the newest stored record. */
static bool resume_time(const ws_store_t *store, uint32_t *start)
{
    uint32_t next = ws_store_next_seq(store);

    /* The newest slot may be torn by a power cut, so try the one before it too. */
    for (uint32_t back = 1; back <= 2 && back <= next - store->oldest_seq; back++) {
        uint8_t rec[WS_RECORD_SIZE];
        ws_record_t r;
        if (ws_store_read(store, next - back, rec) == WS_STORE_OK &&
            ws_record_decode(rec, &r) == WS_OK) {
            *start = r.ts - r.ts % 60 + 60;
            return true;
        }
    }
    return false;
}

static int usage(void)
{
    fprintf(stderr, "usage: ws-emulator [--scenario normal|frost] [--hours N] [--start UNIX_TS]\n"
                    "                   [--seed N] [--i2c-errors P] [--crc-errors P] [--hex] "
                    "[--quiet]\n"
                    "                   [--flash FILE] [--sectors N] [--power-cut-after MINUTES]\n"
                    "                   [--mqtt STATION_ID [--certs DIR] [--speed X] "
                    "[--outage AT_MIN:FOR_MIN]\n"
                    "                    [--authz-check OTHER_STATION_ID]]\n");
    return 2;
}

int main(int argc, char **argv)
{
    emu_config_t cfg = {.scenario = EMU_SCENARIO_NORMAL, .seed = 1};
    uint32_t start = 1792713600u; /* 2026-10-23T00:00:00Z */
    bool start_given = false;
    double hours = 24.0;
    bool hours_given = false;
    const char *flash_path = NULL;
    online_config_t online = {.certs_dir = "certs", .speed = 1.0, .outage_at_min = -1.0};
    const char *authz_other = NULL;
    uint32_t sectors = WS_STORE_SECTORS_16MB;
    double cut_minutes = -1.0;
    run_t run = {0};

    setvbuf(stdout, NULL, _IOLBF, 0); /* keep records and status messages in order */

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--hex") == 0) {
            run.hex = true;
            continue;
        }
        if (strcmp(arg, "--quiet") == 0) {
            run.quiet = true;
            continue;
        }

        /* Every other option takes a value. */
        if (i + 1 >= argc) {
            return usage();
        }
        const char *val = argv[++i];
        if (strcmp(arg, "--scenario") == 0) {
            if (strcmp(val, "frost") == 0) {
                cfg.scenario = EMU_SCENARIO_FROST;
            } else if (strcmp(val, "normal") != 0) {
                return usage();
            }
        } else if (strcmp(arg, "--hours") == 0) {
            hours = atof(val);
            hours_given = true;
        } else if (strcmp(arg, "--start") == 0) {
            start = (uint32_t)strtoul(val, NULL, 10);
            start_given = true;
        } else if (strcmp(arg, "--flash") == 0) {
            flash_path = val;
        } else if (strcmp(arg, "--sectors") == 0) {
            sectors = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(arg, "--power-cut-after") == 0) {
            cut_minutes = atof(val);
        } else if (strcmp(arg, "--mqtt") == 0) {
            online.station_id = val;
        } else if (strcmp(arg, "--certs") == 0) {
            online.certs_dir = val;
        } else if (strcmp(arg, "--speed") == 0) {
            online.speed = atof(val);
        } else if (strcmp(arg, "--outage") == 0) {
            if (sscanf(val, "%lf:%lf", &online.outage_at_min, &online.outage_min) != 2) {
                return usage();
            }
        } else if (strcmp(arg, "--authz-check") == 0) {
            authz_other = val;
        } else if (strcmp(arg, "--seed") == 0) {
            cfg.seed = strtoull(val, NULL, 10);
        } else if (strcmp(arg, "--i2c-errors") == 0) {
            cfg.i2c_error_rate = atof(val);
        } else if (strcmp(arg, "--crc-errors") == 0) {
            cfg.crc_error_rate = atof(val);
        } else {
            return usage();
        }
    }
    if (online.station_id != NULL && !hours_given) {
        hours = 10.0 / 60.0; /* online runs are real time: 10 minutes by default */
    }
    if (hours <= 0 || hours > 24 * 400 || online.speed <= 0) {
        return usage();
    }

    /* Boot: mount the flash log (which recovers the next sequence number) and the upload
     * cursor, which occupies the last two sectors. */
    ws_store_t store;
    ws_cursor_t cursor;
    uint32_t store_sectors = sectors - WS_CURSOR_SECTORS;
    emu_flash_init(sectors);
    if (flash_path != NULL && emu_flash_load(flash_path)) {
        fprintf(stderr, "flash: loaded %s\n", flash_path);
    }
    if (sectors <= WS_CURSOR_SECTORS + 1 || ws_store_mount(&store, store_sectors, 1) != WS_STORE_OK ||
        !ws_cursor_mount(&cursor, store_sectors)) {
        fprintf(stderr, "flash: mount failed\n");
        return 1;
    }
    if (online.station_id != NULL) {
        if (!start_given) {
            start = (uint32_t)time(NULL) / WS_SAMPLE_PERIOD_S * WS_SAMPLE_PERIOD_S; /* real time */
        }
    } else if (!start_given && resume_time(&store, &start)) {
        fprintf(stderr, "flash: resuming a minute after the newest stored record\n");
    }
    fprintf(stderr, "flash: %lu sectors, %lu in use, next seq %lu, oldest seq %lu, "
                    "torn records %lu, uploaded through %s%lu\n\n",
            (unsigned long)sectors, (unsigned long)store.valid_sectors,
            (unsigned long)ws_store_next_seq(&store), (unsigned long)store.oldest_seq,
            (unsigned long)store.torn_records, cursor.has_value ? "#" : "(none) ",
            (unsigned long)(cursor.has_value ? cursor.value : 0));
    run.store = &store;

    if (online.station_id != NULL) {
        run.uplink = online_init(&online, &store, &cursor);
        if (run.uplink == NULL) {
            return 1;
        }
        if (authz_other != NULL) {
            return online_authz_check(authz_other);
        }
    }

    ws_station_t st;
    const ws_event_config_t events = WS_EVENT_CONFIG_DEFAULT;
    emu_init(&cfg);
    ws_station_init(&st, ws_store_next_seq(&store), &events, on_record, &run);

    uint32_t duration = (uint32_t)(hours * 3600.0);
    int online_result = 0;
    if (run.uplink != NULL) {
        online_result = online_run(&st, start, duration);
    } else if (cut_minutes >= 0 && cut_minutes * 60.0 < duration) {
        /* Run normally, then cut the power 7 bytes into the next flash write. */
        uint32_t before = (uint32_t)(cut_minutes * 60.0);
        emu_run(&st, start, before);
        emu_flash_power_cut_after(7);
        emu_run(&st, start + before + WS_SAMPLE_PERIOD_S, duration - before);
        fprintf(stderr, "\n*** power cut during a flash write (seq %lu) ***\n",
                (unsigned long)(st.next_seq - 1));
    } else {
        emu_run(&st, start, duration);
    }
    if (flash_path != NULL) {
        emu_flash_power_restore();
        if (!emu_flash_save(flash_path)) {
            fprintf(stderr, "flash: could not save %s\n", flash_path);
            return 1;
        }
    }

    fprintf(stderr,
            "\n%lu aggregates, %lu events, %lu records with error flags, %lu bad frames\n"
            "sensor read failures: temperature %lu, humidity %lu; encode errors %lu; "
            "store errors %lu\n"
            "%zu bytes as live frames (%.1f bytes per record)\n",
            (unsigned long)run.aggregates, (unsigned long)run.events, (unsigned long)run.flagged,
            (unsigned long)run.bad_frames, (unsigned long)st.temp_errors,
            (unsigned long)st.rh_errors, (unsigned long)st.encode_errors,
            (unsigned long)run.store_errors, run.bytes,
            run.aggregates + run.events ? (double)run.bytes / (run.aggregates + run.events) : 0.0);
    if (emu_flash_violations() != 0) {
        fprintf(stderr, "flash: %lu NOR rule violations by the firmware\n",
                (unsigned long)emu_flash_violations());
        return 1;
    }
    return run.bad_frames == 0 && st.encode_errors == 0 && online_result == 0 ? 0 : 1;
}
