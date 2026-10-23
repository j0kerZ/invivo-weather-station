/*
 * ws-emulator: runs the station firmware against emulated sensors and prints every record.
 *
 *   ws-emulator [--scenario normal|frost] [--hours N] [--start UNIX_TS] [--seed N]
 *               [--i2c-errors P] [--crc-errors P] [--hex] [--quiet]
 */

#define _POSIX_C_SOURCE 200809L /* gmtime_r */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "emu.h"

typedef struct {
    bool hex;
    bool quiet;
    uint32_t aggregates, events, flagged, bad_frames;
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
}

static int usage(void)
{
    fprintf(stderr, "usage: ws-emulator [--scenario normal|frost] [--hours N] [--start UNIX_TS]\n"
                    "                   [--seed N] [--i2c-errors P] [--crc-errors P] [--hex] "
                    "[--quiet]\n");
    return 2;
}

int main(int argc, char **argv)
{
    emu_config_t cfg = {.scenario = EMU_SCENARIO_NORMAL, .seed = 1};
    uint32_t start = 1792713600u; /* 2026-10-23T00:00:00Z */
    double hours = 24.0;
    run_t run = {0};

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
        } else if (strcmp(arg, "--start") == 0) {
            start = (uint32_t)strtoul(val, NULL, 10);
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
    if (hours <= 0 || hours > 24 * 400) {
        return usage();
    }

    ws_station_t st;
    const ws_event_config_t events = WS_EVENT_CONFIG_DEFAULT;
    emu_init(&cfg);
    ws_station_init(&st, 1, &events, on_record, &run);
    emu_run(&st, start, (uint32_t)(hours * 3600.0));

    fprintf(stderr,
            "\n%lu aggregates, %lu events, %lu records with error flags, %lu bad frames\n"
            "sensor read failures: temperature %lu, humidity %lu; encode errors %lu\n"
            "%zu bytes as live frames (%.1f bytes per record)\n",
            (unsigned long)run.aggregates, (unsigned long)run.events, (unsigned long)run.flagged,
            (unsigned long)run.bad_frames, (unsigned long)st.temp_errors,
            (unsigned long)st.rh_errors, (unsigned long)st.encode_errors, run.bytes,
            run.aggregates + run.events ? (double)run.bytes / (run.aggregates + run.events) : 0.0);
    return run.bad_frames == 0 && st.encode_errors == 0 ? 0 : 1;
}
