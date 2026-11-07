#include "native_diagnostics.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "rdp_ffi.h"

NativeDiagnosticsRates native_diagnostics_rates(NativeDiagnosticsWindow *w, int slot,
                                                 NativeVideoCounters counters, uint32_t now_ms) {
    NativeDiagnosticsRates out = {0};
    if (w->started && w->slot == slot && w->before.epoch == counters.epoch &&
        w->before.generation == counters.generation) {
        uint32_t elapsed = now_ms - w->at_ms;
        out.discontinuity = (uint32_t)(counters.received - w->before.received) > INT32_MAX ||
            (uint64_t)(counters.bytes - w->before.bytes) > INT64_MAX ||
            (uint64_t)(counters.bytes_auxiliary - w->before.bytes_auxiliary) > INT64_MAX;
        if (elapsed && elapsed <= 2 * NATIVE_DIAGNOSTICS_INTERVAL_MS && !out.discontinuity) {
            out.valid = true;
            out.input_aus = (double)(uint32_t)(counters.received - w->before.received) * 1000.0 / elapsed;
            out.input_mbps = (double)(uint64_t)(counters.bytes - w->before.bytes) * 0.008 / elapsed;
            out.input_auxiliary_mbps =
                (double)(uint64_t)(counters.bytes_auxiliary - w->before.bytes_auxiliary) * 0.008 / elapsed;
        }
    }
    *w = (NativeDiagnosticsWindow){true, slot, now_ms, counters};
    return out;
}

bool native_diagnostics_parse_process(const char *text, uint64_t *ticks, uint64_t *rss_pages) {
    if (!text || !ticks || !rss_pages) return false;
    /* comm may contain spaces and ')'; numeric fields follow its last closing bracket. */
    const char *p = strrchr(text, ')');
    if (!p || p[1] != ' ') return false;
    p += 2;
    uint64_t user = 0, system = 0, pages = 0;
    for (unsigned field = 3; field <= 24; field++) {
        while (*p == ' ') p++;
        if (!*p) return false;
        if (field == 14 || field == 15 || field == 24) {
            if (*p < '0' || *p > '9') return false;
            errno = 0;
            char *end = NULL;
            unsigned long long value = strtoull(p, &end, 10);
            if (errno || end == p || (*end && *end != ' ' && *end != '\n')) return false;
            if (field == 14) user = value;
            if (field == 15) system = value;
            if (field == 24) pages = value;
            p = end;
        } else {
            while (*p && *p != ' ' && *p != '\n') p++;
            if (*p == '\n') return false;
        }
    }
    if (UINT64_MAX - user < system) return false;
    *ticks = user + system;
    *rss_pages = pages;
    return true;
}

static bool cpu_number(const char **p, uint64_t *value) {
    while (**p == ' ' || **p == '\t') (*p)++;
    if (**p < '0' || **p > '9') return false;
    errno = 0;
    char *end;
    *value = strtoull(*p, &end, 10);
    if (errno || (*end && *end != ' ' && *end != '\t' && *end != '\n')) return false;
    *p = end;
    return true;
}

static bool parse_cpu(const char *p, unsigned *index, NativeCpuCounters *out) {
    if (strncmp(p, "cpu", 3) || p[3] < '0' || p[3] > '9') return false;
    p += 3;
    uint64_t id;
    if (!cpu_number(&p, &id) || id >= NATIVE_DIAGNOSTICS_MAX_CPUS) return false;
    NativeCpuCounters c = {.present = true};
    /* guest/guest_nice are already included in user/nice, so read only the first eight. */
    for (int i = 0; i < 8; i++) {
        uint64_t value;
        if (!cpu_number(&p, &value)) return false;
        if (i == 3) c.idle = value;
        else if (i == 4) c.iowait = value;
        else {
            if (UINT64_MAX - c.busy < value) return false;
            c.busy += value;
        }
    }
    *index = (unsigned)id;
    *out = c;
    return true;
}

void native_diagnostics_update_cpus(NativeDiagnostics *d, const char *text) {
    NativeCpuCounters current[NATIVE_DIAGNOSTICS_MAX_CPUS] = {0};
    while (text && *text) {
        unsigned index;
        NativeCpuCounters c;
        if (parse_cpu(text, &index, &c)) current[index] = c;
        text = strchr(text, '\n');
        if (text) text++;
    }
    for (int i = 0; i < NATIVE_DIAGNOSTICS_MAX_CPUS; i++) {
        NativeCpuCounters c = current[i], before = d->cores_before[i];
        NativeCpuUsage *out = &d->snapshot.cores[i];
        *out = (NativeCpuUsage){.present = c.present};
        if (c.present && before.present && c.busy >= before.busy && c.idle >= before.idle) {
            uint64_t busy = c.busy - before.busy, idle = c.idle - before.idle;
            /* Linux can decrease iowait; that must not wrap into a huge idle interval. */
            uint64_t wait = c.iowait >= before.iowait ? c.iowait - before.iowait : 0;
            double total = (double)busy + (double)idle + (double)wait;
            if (total > 0) {
                out->valid = true;
                out->percent = (double)busy * 100.0 / total;
            }
        }
        d->cores_before[i] = c;
    }
}

void native_diagnostics_sample_system(NativeDiagnostics *d, uint32_t now_ms) {
    char text[4096];
    FILE *f = fopen("/proc/stat", "r");
    size_t size = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f) fclose(f);
    text[size] = '\0';
    native_diagnostics_update_cpus(d, text);
    NativeDiagnosticsSnapshot *s = &d->snapshot;
    s->cpu_valid = false;
    s->memory_valid = false;
    f = fopen("/proc/self/stat", "r");
    size = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f) fclose(f);
    text[size] = '\0';
    uint64_t ticks, pages;
    long hz = sysconf(_SC_CLK_TCK), page_size = sysconf(_SC_PAGESIZE);
    if (!size || !native_diagnostics_parse_process(text, &ticks, &pages)) {
        d->cpu_started = false;
        return;
    }
    if (page_size > 0 && pages <= UINT64_MAX / (uint64_t)page_size) {
        s->memory_valid = true;
        s->rss_bytes = pages * (uint64_t)page_size;
    }
    uint32_t elapsed = now_ms - d->cpu_ms;
    if (d->cpu_started && hz > 0 && elapsed && ticks >= d->cpu_ticks) {
        s->cpu_valid = true;
        s->cpu_percent = (double)(ticks - d->cpu_ticks) * 100000.0 / ((double)hz * elapsed);
    }
    d->cpu_started = hz > 0;
    d->cpu_ticks = ticks;
    d->cpu_ms = now_ms;
}
