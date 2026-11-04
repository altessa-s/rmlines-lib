/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The whole of the test harness: a counter, a macro that says what failed and
 * where, and a main that answers with the count. A library of four hundred
 * lines to run twenty assertions would be more machinery than the thing it
 * tests. */

#ifndef RM_TEST_H
#define RM_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rm_test_failures = 0;
static int rm_test_checks = 0;

#define RM_CHECK(condition, ...)                                               \
  do {                                                                         \
    rm_test_checks += 1;                                                       \
    if (!(condition)) {                                                        \
      rm_test_failures += 1;                                                   \
      fprintf(stderr, "%s:%d: FAIL %s\n    ", __FILE__, __LINE__, #condition); \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

#define RM_CHECK_EQ_SIZE(actual, expected, ...)                            \
  do {                                                                     \
    size_t rm_a_ = (actual), rm_e_ = (expected);                           \
    rm_test_checks += 1;                                                   \
    if (rm_a_ != rm_e_) {                                                  \
      rm_test_failures += 1;                                               \
      fprintf(stderr, "%s:%d: FAIL %s == %s (%zu vs %zu)\n    ", __FILE__, \
              __LINE__, #actual, #expected, rm_a_, rm_e_);                 \
      fprintf(stderr, __VA_ARGS__);                                        \
      fprintf(stderr, "\n");                                               \
    }                                                                      \
  } while (0)

#define RM_CHECK_EQ_STR(actual, actual_len, expected, ...)              \
  do {                                                                  \
    const char* rm_a_ = (actual);                                       \
    size_t rm_al_ = (actual_len);                                       \
    const char* rm_e_ = (expected);                                     \
    rm_test_checks += 1;                                                \
    if (rm_al_ != strlen(rm_e_) || memcmp(rm_a_, rm_e_, rm_al_) != 0) { \
      rm_test_failures += 1;                                            \
      fprintf(stderr, "%s:%d: FAIL %s == \"%s\" (got \"%.*s\")\n    ",  \
              __FILE__, __LINE__, #actual, rm_e_, (int)rm_al_, rm_a_);  \
      fprintf(stderr, __VA_ARGS__);                                     \
      fprintf(stderr, "\n");                                            \
    }                                                                   \
  } while (0)

[[maybe_unused]] static int rm_test_report(const char* suite) {
  if (rm_test_failures == 0) {
    printf("%s: %d checks, all passed\n", suite, rm_test_checks);
    return 0;
  }
  printf("%s: %d checks, %d FAILED\n", suite, rm_test_checks, rm_test_failures);
  return 1;
}

/* Read a whole file. The tests do their own I/O: the library takes bytes and
 * has never opened a file, which is what keeps it testable against a corpus
 * and nothing else. */
[[maybe_unused]] static unsigned char* rm_test_read_file(const char* path,
                                                         size_t* out_size) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  unsigned char* bytes = malloc((size_t)size + 1u);
  if (bytes == NULL) {
    fclose(file);
    return NULL;
  }
  size_t read = fread(bytes, 1, (size_t)size, file);
  fclose(file);
  if (read != (size_t)size) {
    free(bytes);
    return NULL;
  }
  *out_size = read;
  return bytes;
}

#ifndef RM_FIXTURE_DIR
#define RM_FIXTURE_DIR "tests/fixtures"
#endif

[[maybe_unused]] static void rm_test_fixture_path(char* out, size_t size,
                                                  const char* name) {
  snprintf(out, size, "%s/v6-samples/%s", RM_FIXTURE_DIR, name);
}

#endif /* RM_TEST_H */
