/* SPDX-License-Identifier: MIT */
#ifndef CATEGORY_LOG_H
#define CATEGORY_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_PRINTF_FORMAT(format_index_, first_argument_) \
    __attribute__((format(printf, format_index_, first_argument_)))
#else
#define CLOGX_PRINTF_FORMAT(format_index_, first_argument_)
#endif

#define CLOGX_LEVEL_VALUE_TRACE 0
#define CLOGX_LEVEL_VALUE_DEBUG 1
#define CLOGX_LEVEL_VALUE_INFO 2
#define CLOGX_LEVEL_VALUE_NOTICE 3
#define CLOGX_LEVEL_VALUE_WARN 4
#define CLOGX_LEVEL_VALUE_ERROR 5
#define CLOGX_LEVEL_VALUE_FATAL 6
#define CLOGX_LEVEL_VALUE_OFF 7

typedef enum ClogxLevel {
    CLOGX_LEVEL_TRACE = CLOGX_LEVEL_VALUE_TRACE,
    CLOGX_LEVEL_DEBUG = CLOGX_LEVEL_VALUE_DEBUG,
    CLOGX_LEVEL_INFO = CLOGX_LEVEL_VALUE_INFO,
    CLOGX_LEVEL_NOTICE = CLOGX_LEVEL_VALUE_NOTICE,
    CLOGX_LEVEL_WARN = CLOGX_LEVEL_VALUE_WARN,
    CLOGX_LEVEL_ERROR = CLOGX_LEVEL_VALUE_ERROR,
    CLOGX_LEVEL_FATAL = CLOGX_LEVEL_VALUE_FATAL,
    CLOGX_LEVEL_OFF = CLOGX_LEVEL_VALUE_OFF
} ClogxLevel;

typedef struct ClogxCategory {
    /* Public for static initialization; treat every field as logger-owned after definition. */
    const char *name;
    ClogxLevel default_level;
    atomic_int effective_level;
    atomic_bool registered;
    struct ClogxCategory *next;
} ClogxCategory;

/* Categories and their name strings must have process lifetime (normally file scope):
 * first use permanently registers the object for live reconfiguration. */
#define CLOGX_CATEGORY_INITIALIZER(name_, default_level_) \
    { (name_), (default_level_), (default_level_), false, NULL }

typedef struct ClogxEvent {
    ClogxLevel level;
    const char *category;
    const char *message;
    const char *file;
    const char *function;
    int line;
    uint64_t wall_time_ns;
    uint64_t monotonic_time_ns;
    bool message_truncated;
} ClogxEvent;

/* monotonic_time_ns uses a steady platform clock when one is available. The ISO C
 * fallback is nondecreasing and sleep-aware, but wall-clock adjustments can move it
 * forward; configure-time feature detection should enable the steady clock normally. */

enum {
    CLOGX_CONFIG_OK = 0,
    CLOGX_CONFIG_INVALID = -1,
    CLOGX_CONFIG_TOO_MANY_RULES = -2
};

const char *clogx_level_name(ClogxLevel level);
bool clogx_enabled(ClogxCategory *category, ClogxLevel level);
bool clogx_enabled_at_or_above(ClogxCategory *category, ClogxLevel level, int compiled_min_level);

/* Apply up to 32 comma-separated, case-sensitive selector=level rules transactionally.
 * A selector also matches dot-separated descendants; later rules win. Empty resets all
 * categories to their compiled defaults. */
int clogx_configure(const char *spec);
/* Apply LGNOME_LOG, or reset to defaults when it is absent. */
int clogx_configure_env(void);

void clogx_vlogf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                  const char *function, const char *format, va_list args)
    CLOGX_PRINTF_FORMAT(6, 0);
void clogx_logf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                 const char *function, const char *format, ...)
    CLOGX_PRINTF_FORMAT(6, 7);

#ifndef CLOGX_COMPILED_MIN_LEVEL
#define CLOGX_COMPILED_MIN_LEVEL CLOGX_LEVEL_VALUE_TRACE
#endif
/* Override with a preprocessor value, for example
 * -DCLOGX_COMPILED_MIN_LEVEL=CLOGX_LEVEL_VALUE_INFO. Enum identifiers cannot be used in
 * preprocessor comparisons. Fixed-level macros below that floor compile to no-ops. */

typedef struct ClogxRateLimit {
    atomic_flag lock;
    uint64_t window_start_ns;
    uint32_t emitted;
    uint32_t suppressed;
} ClogxRateLimit;

#define CLOGX_RATE_LIMIT_INITIALIZER { ATOMIC_FLAG_INIT, 0, 0, 0 }

bool clogx_rate_limit_allow(ClogxRateLimit *limit, uint32_t burst, uint64_t interval_ms,
                            uint32_t *suppressed);
void clogx_logf_suppressed(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                           const char *function, uint32_t suppressed, const char *format, ...)
    CLOGX_PRINTF_FORMAT(7, 8);

#ifdef CLOGX_TESTING
typedef uint64_t (*ClogxTestClock)(void);
void clogx_test_set_clocks(ClogxTestClock wall_clock, ClogxTestClock monotonic_clock);
size_t clogx_test_format_event(const ClogxEvent *event, char *output, size_t capacity);
/* Install or reset only while no logging threads are running. */
typedef void (*ClogxTestSink)(const ClogxEvent *event, void *context);
void clogx_test_set_sink(ClogxTestSink sink, void *context);
#endif

#endif
