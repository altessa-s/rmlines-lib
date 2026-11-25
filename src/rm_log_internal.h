/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Internal diagnostics helpers. */

#ifndef RM_LOG_INTERNAL_H
#define RM_LOG_INTERNAL_H

#include <stdbool.h>

#include "rmlines/log.h"

/* The message is built only when a sink is installed, which is what makes a
 * debug line on a hot parsing path free. */
bool rm_log_wanted(void);

/* `format(printf, ...)` is a GNU spelling. MSVC has no `__attribute__` at all
 * — unguarded it is a syntax error, not a warning — and the library declares
 * Windows a target in `export.h` and in the build. GCC targeting MinGW reads
 * the plain `printf` archetype as Microsoft's, which has no `%zu`; the C
 * runtime the library is built against does format it, so GCC there is told
 * `gnu_printf`. */
#if defined(__MINGW32__) && !defined(__clang__)
#define RM_PRINTF(format_index, first_argument) \
  __attribute__((format(gnu_printf, format_index, first_argument)))
#elif defined(__GNUC__) || defined(__clang__)
#define RM_PRINTF(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define RM_PRINTF(format_index, first_argument)
#endif

/* Format a line and hand it to the installed sink. A no-op when none is
 * installed — callers guard with `rm_log_wanted` so the formatting itself is
 * skipped on the common path. */
void rm_log_writef(rm_log_level level, const char* format, ...) RM_PRINTF(2, 3);

#define RM_LOG_ERRORF(...)                                         \
  do {                                                             \
    if (rm_log_wanted()) rm_log_writef(RM_LOG_ERROR, __VA_ARGS__); \
  } while (false)
#define RM_LOG_DEBUGF(...)                                         \
  do {                                                             \
    if (rm_log_wanted()) rm_log_writef(RM_LOG_DEBUG, __VA_ARGS__); \
  } while (false)

#endif /* RM_LOG_INTERNAL_H */
