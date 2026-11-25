/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "rm_log_internal.h"

/* The sink and its context move together. A writer copies the pair under the
 * guard, then releases it before calling the sink so callbacks can change the
 * registration. */
typedef struct rm_log_binding {
  rm_log_sink sink;
  void* context;
} rm_log_binding;

static atomic_flag rm_log_guard = ATOMIC_FLAG_INIT;
static _Atomic bool rm_log_has_sink = false;
static rm_log_binding rm_log_active = {nullptr, nullptr};

static void rm_log_lock(void) {
  while (
      atomic_flag_test_and_set_explicit(&rm_log_guard, memory_order_acquire)) {
  }
}

static void rm_log_unlock(void) {
  atomic_flag_clear_explicit(&rm_log_guard, memory_order_release);
}

void rm_log_writef(rm_log_level level, const char* format, ...) {
  if (!atomic_load_explicit(&rm_log_has_sink, memory_order_acquire)) return;
  rm_log_lock();
  rm_log_binding binding = rm_log_active;
  rm_log_unlock();
  if (binding.sink == nullptr) return;

  /* One line, one stack buffer. The messages are counts, block types and
   * raw style numbers — never a page's text — so a line that does not fit
   * is rare; it goes to the heap rather than out cut short, so that every
   * line is handed over whole. */
  char message[512];
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  int written = vsnprintf(message, sizeof message, format, args);
  va_end(args);
  if (written < 0) {
    va_end(again);
    return;
  }
  if ((size_t)written < sizeof message) {
    va_end(again);
    binding.sink(level, message, binding.context);
    return;
  }
  char* whole = malloc((size_t)written + 1u);
  if (whole == nullptr) {
    va_end(again);

    /* Out of memory: the line cut short beats no line at all. */
    binding.sink(level, message, binding.context);
    return;
  }
  (void)vsnprintf(whole, (size_t)written + 1u, format, again);
  va_end(again);
  binding.sink(level, whole, binding.context);
  free(whole);
}

/* Registration and silencing must remain allocation-free (R235 / RV43-R2-14).
 */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC poison malloc calloc realloc free
#endif

void rm_log_install(rm_log_sink sink, void* context) {
  if (sink == nullptr) {
    rm_log_silence();
    return;
  }
  rm_log_lock();
  rm_log_active = (rm_log_binding){.sink = sink, .context = context};
  atomic_store_explicit(&rm_log_has_sink, true, memory_order_release);
  rm_log_unlock();
}

void rm_log_silence(void) {
  rm_log_lock();
  rm_log_active = (rm_log_binding){nullptr, nullptr};
  atomic_store_explicit(&rm_log_has_sink, false, memory_order_release);
  rm_log_unlock();
}

bool rm_log_wanted(void) {
  return atomic_load_explicit(&rm_log_has_sink, memory_order_acquire);
}
