#include "native_diagnostics.h"
#include "rdp_ffi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void rates_follow_live_input(void) {
    NativeDiagnosticsWindow w = {0};
    NativeVideoCounters c = {.epoch = 1, .generation = 1};
    assert(!native_diagnostics_rates(&w, 0, c, 100).valid);
    c.received = 30; c.bytes = 1000000; c.bytes_auxiliary = 250000; c.accepted = 28; c.replayed = 100;
    NativeDiagnosticsRates r = native_diagnostics_rates(&w, 0, c, 600);
    assert(r.valid && r.input_aus == 60.0 && r.input_mbps == 16.0);
    assert(r.input_auxiliary_mbps == 4.0);
    NativeDiagnosticsWindow reset = w;
    NativeVideoCounters zero = {.epoch = c.epoch, .generation = c.generation};
    NativeDiagnosticsRates reset_rate = native_diagnostics_rates(&reset, 0, zero, 1100);
    assert(!reset_rate.valid && reset_rate.discontinuity);
    c.replayed += 200;
    r = native_diagnostics_rates(&w, 0, c, 1100);
    assert(r.valid && r.input_aus == 0 && r.input_mbps == 0);
    assert(r.input_auxiliary_mbps == 0);
    c.bytes += 125000; c.bytes_auxiliary += 125000;
    r = native_diagnostics_rates(&w, 0, c, 1600);
    assert(r.valid && r.input_mbps == 2.0 && r.input_auxiliary_mbps == 2.0);
    assert(!native_diagnostics_rates(&w, 1, c, 1600).valid);
    assert(!native_diagnostics_rates(&w, 0, c, 1700).valid);
    c.generation++;
    assert(!native_diagnostics_rates(&w, 0, c, 2200).valid);
    native_diagnostics_counter_epoch(&c, 2);
    assert(!c.received && !c.accepted && !c.replayed && !c.has_au);
    assert(!c.bytes && !c.bytes_auxiliary);
    assert(!native_diagnostics_rates(&w, 0, c, 2700).valid);
    assert(!native_diagnostics_rates(&w, 0, c, 2700).valid);
}

static void wrapping_counters_and_clock(void) {
    NativeDiagnosticsWindow w = {0};
    NativeVideoCounters c = {.epoch = 1, .received = UINT32_MAX - 9,
                            .bytes = UINT64_MAX - 99, .bytes_auxiliary = UINT64_MAX - 199,
                            .accepted = UINT32_MAX - 4};
    native_diagnostics_rates(&w, 0, c, UINT32_MAX - 249);
    c.received += 30; c.bytes += 1000000; c.bytes_auxiliary += 250000; c.accepted += 25;
    NativeDiagnosticsRates r = native_diagnostics_rates(&w, 0, c, 250);
    assert(r.valid && r.input_aus == 60.0 && r.input_mbps == 16.0);
    assert(r.input_auxiliary_mbps == 4.0);
}

static void process_parser(void) {
    char text[512] = "123 (a command ) with spaces) S";
    for (int field = 4; field <= 24; field++) {
        char word[32];
        snprintf(word, sizeof(word), " %u", field == 14 ? 19u : field == 15 ? 23u : field == 24 ? 1024u : 0u);
        strcat(text, word);
    }
    uint64_t ticks, pages;
    assert(native_diagnostics_parse_process(text, &ticks, &pages) && ticks == 42 && pages == 1024);
    assert(!native_diagnostics_parse_process("bad", &ticks, &pages));
    assert(!native_diagnostics_parse_process("1 (short) S 1 2", &ticks, &pages));
    char *rss = strrchr(text, ' ');
    strcpy(rss, " -1");
    assert(!native_diagnostics_parse_process(text, &ticks, &pages));
    strcpy(rss, " 18446744073709551616");
    assert(!native_diagnostics_parse_process(text, &ticks, &pages));
    NativeDiagnostics d = {0};
    native_diagnostics_sample_system(&d, 1000);
    assert(d.snapshot.memory_valid && !d.snapshot.cpu_valid);
    assert(d.snapshot.cores[0].present && !d.snapshot.cores[0].valid);
    native_diagnostics_sample_system(&d, 1500);
    assert(d.snapshot.cpu_valid && d.snapshot.cpu_percent >= 0);
}

static void per_core_load_uses_intervals(void) {
    NativeDiagnostics d = {0};
    native_diagnostics_update_cpus(&d,
        "cpu  100 0 0 100 0 0 0 0\n"
        "cpu0 100 20 30 100 10 5 5 0 40 10\n"
        "cpu2 0 0 0 0 0 0 0 0\n");
    assert(d.snapshot.cores[0].present && !d.snapshot.cores[0].valid);
    assert(!d.snapshot.cores[1].present && d.snapshot.cores[2].present);
    native_diagnostics_update_cpus(&d,
        "cpu0 110 20 40 170 20 5 5 0 50 10\n"
        "cpu2 100 0 0 0 0 0 0 0\n");
    assert(d.snapshot.cores[0].valid && d.snapshot.cores[0].percent == 20.0);
    assert(d.snapshot.cores[2].valid && d.snapshot.cores[2].percent == 100.0);
    native_diagnostics_update_cpus(&d, "cpu0 120 20 40 180 15 5 5 0 50 10\n");
    assert(d.snapshot.cores[0].valid && d.snapshot.cores[0].percent == 50.0);
    assert(!d.snapshot.cores[2].present);
    native_diagnostics_update_cpus(&d, "cpu0 120 20 40 280 15 5 5 0\n");
    assert(d.snapshot.cores[0].valid && d.snapshot.cores[0].percent == 0);
    native_diagnostics_update_cpus(&d, "cpu0 120 20 40 280 15 5 5 0\n");
    assert(!d.snapshot.cores[0].valid);
    native_diagnostics_update_cpus(&d, "cpu0 1 0 0 0 0 0 0 0\n");
    assert(!d.snapshot.cores[0].valid);
    native_diagnostics_update_cpus(&d, "cpu0 2 0 0 1 0 0 0 0\n");
    assert(d.snapshot.cores[0].valid && d.snapshot.cores[0].percent == 50.0);
    native_diagnostics_update_cpus(&d, NULL);
    assert(!d.snapshot.cores[0].present);
    native_diagnostics_update_cpus(&d, "cpu0 3 0 0 2 0 0 0 0\n");
    assert(d.snapshot.cores[0].present && !d.snapshot.cores[0].valid);
    native_diagnostics_update_cpus(&d,
        "cpu0 -1 0 0 0 0 0 0 0\n"
        "cpu1 18446744073709551616 0 0 0 0 0 0 0\n"
        "cpu2 18446744073709551615 1 0 0 0 0 0 0\n"
        "cpu3 1 2 3\n"
        "cpu999999999999999999999 0 0 0 0 0 0 0 0\n");
    for (int i = 0; i < NATIVE_DIAGNOSTICS_MAX_CPUS; i++) assert(!d.snapshot.cores[i].present);
}

int main(void) {
    rates_follow_live_input();
    wrapping_counters_and_clock();
    process_parser();
    per_core_load_uses_intervals();
    return 0;
}
