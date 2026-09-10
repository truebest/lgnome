/* SPDX-License-Identifier: MIT */
#ifndef CLOG_H
#define CLOG_H

/* Include after system headers: clog is also the C99 complex logarithm. */

#include "category_log.h"

typedef ClogxLevel cLogLevel;
typedef ClogxCategory cLogCategory;
typedef ClogxEvent cLogEvent;

#define cLogLevelTrace CLOGX_LEVEL_TRACE
#define cLogLevelDebug CLOGX_LEVEL_DEBUG
#define cLogLevelInfo CLOGX_LEVEL_INFO
#define cLogLevelNotice CLOGX_LEVEL_NOTICE
#define cLogLevelWarning CLOGX_LEVEL_WARN
#define cLogLevelError CLOGX_LEVEL_ERROR
#define cLogLevelFatal CLOGX_LEVEL_FATAL
#define cLogLevelOff CLOGX_LEVEL_OFF

#if defined(__GNUC__) || defined(__clang__)
#define CLOG_PRIVATE_UNUSED __attribute__((unused))
#else
#define CLOG_PRIVATE_UNUSED
#endif

#if defined(__GNUC__) || defined(__clang__)
/* Keep constant levels below the compiled floor entirely out of the object even
 * without optimizer passes. __builtin_constant_p never evaluates its operand. */
#define CLOG_PRIVATE_LEVEL_MAY_LOG(level_)                                                                \
    (!__builtin_constant_p(level_) || (int)(level_) >= (int)CLOGX_COMPILED_MIN_LEVEL)
#else
#define CLOG_PRIVATE_LEVEL_MAY_LOG(level_) true
#endif

/* The fixed internal pointer is what lets clog() find the arbitrary symbol
 * supplied to clog_define(). Its file-local definition also makes a second
 * clog_define() in the same translation unit a compile-time error. */
#define clog_define(symbol_, level_, prefix_)                                                             \
    static cLogCategory symbol_ CLOG_PRIVATE_UNUSED =                                                     \
        CLOGX_CATEGORY_INITIALIZER((prefix_), (level_));                                                  \
    static cLogCategory *const clog_private_file_category CLOG_PRIVATE_UNUSED = &(symbol_)

#define clog(level_, ...)                                                                                 \
    do {                                                                                                  \
        if (CLOG_PRIVATE_LEVEL_MAY_LOG(level_)) {                                                         \
            cLogLevel clog_private_level = (cLogLevel)(level_);                                           \
            if ((int)clog_private_level >= (int)CLOGX_COMPILED_MIN_LEVEL &&                               \
                clogx_enabled(clog_private_file_category, clog_private_level)) {                          \
                clogx_logf(clog_private_file_category, clog_private_level, __FILE__, __LINE__, __func__,  \
                           __VA_ARGS__);                                                                  \
            }                                                                                             \
        }                                                                                                 \
    } while (0)

#define clog_is_enabled(level_)                                                                           \
    (CLOG_PRIVATE_LEVEL_MAY_LOG(level_) &&                                                                \
     clogx_enabled_at_or_above(clog_private_file_category, (cLogLevel)(level_),                           \
                               CLOGX_COMPILED_MIN_LEVEL))

/* va_list/source bridge for adapters that forward another library's callback. */
#define clog_vlogf_at(level_, file_, line_, function_, format_, args_)                                    \
    do {                                                                                                  \
        if (CLOG_PRIVATE_LEVEL_MAY_LOG(level_)) {                                                         \
            cLogLevel clog_private_level = (cLogLevel)(level_);                                           \
            if ((int)clog_private_level >= (int)CLOGX_COMPILED_MIN_LEVEL) {                               \
                clogx_vlogf(clog_private_file_category, clog_private_level, (file_), (line_),             \
                            (function_), (format_), (args_));                                             \
            }                                                                                             \
        }                                                                                                 \
    } while (0)

#define clog_limited(level_, burst_, interval_ms_, ...)                                                   \
    do {                                                                                                  \
        static ClogxRateLimit clog_private_rate_limit = CLOGX_RATE_LIMIT_INITIALIZER;                     \
        if (CLOG_PRIVATE_LEVEL_MAY_LOG(level_)) {                                                         \
            cLogLevel clog_private_level = (cLogLevel)(level_);                                           \
            uint32_t clog_private_suppressed = 0;                                                         \
            if ((int)clog_private_level >= (int)CLOGX_COMPILED_MIN_LEVEL &&                               \
                clogx_enabled(clog_private_file_category, clog_private_level) &&                          \
                clogx_rate_limit_allow(&clog_private_rate_limit, (burst_), (interval_ms_),                \
                                       &clog_private_suppressed)) {                                       \
                clogx_logf_suppressed(clog_private_file_category, clog_private_level, __FILE__, __LINE__, \
                                      __func__, clog_private_suppressed, __VA_ARGS__);                    \
            }                                                                                             \
        }                                                                                                 \
    } while (0)

/* One-shot per call site; use explicit state for resettable diagnostics. */
#define clog_once(level_, ...)                                                                            \
    do {                                                                                                  \
        static bool clog_private_once_latched = false;                                                    \
        if (!clog_private_once_latched) {                                                                 \
            clog_private_once_latched = true;                                                             \
            clog((level_), __VA_ARGS__);                                                                  \
        }                                                                                                 \
    } while (0)

/* Lowercase management API. The engine names stay private implementation details. */
#define clog_level_name clogx_level_name
#define clog_configure clogx_configure
#define clog_configure_env clogx_configure_env

#define cLogConfigOK CLOGX_CONFIG_OK
#define cLogConfigInvalid CLOGX_CONFIG_INVALID
#define cLogConfigTooManyRules CLOGX_CONFIG_TOO_MANY_RULES

#endif
