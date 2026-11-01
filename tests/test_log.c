/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The one seam this library has with its host. The parser says what it could
 * not read; where that goes is the host's to decide, and until it decides,
 * nowhere — a library that writes to a terminal nobody is watching is a
 * library that has to be worked around. */

#include <stdlib.h>

#include "rm_damaged.h"
#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

typedef struct collected {
    rm_log_level levels[32];
    char lines[32][512];
    size_t count;
} collected;

static void collect(rm_log_level level, const char *message, void *context) {
    collected *into = context;
    if (into->count >= 32) return;
    into->levels[into->count] = level;
    snprintf(into->lines[into->count], sizeof into->lines[0], "%s", message);
    into->count += 1;
}

static bool saw(const collected *seen, rm_log_level level, const char *needle) {
    for (size_t i = 0; i < seen->count; i += 1) {
        if (seen->levels[i] == level && strstr(seen->lines[i], needle) != nullptr) return true;
    }
    return false;
}

static void an_installed_sink_receives_the_line_with_its_level(void) {
    collected seen = { 0 };
    rm_log_install(collect, &seen);
    rm_log_writef(RM_LOG_ERROR, "page: a stroke was lost");
    rm_log_writef(RM_LOG_DEBUG, "render: unknown paragraph style %d", 10);
    rm_log_silence();

    RM_CHECK(saw(&seen, RM_LOG_ERROR, "a stroke was lost"), "the error arrived");
    RM_CHECK(saw(&seen, RM_LOG_DEBUG, "unknown paragraph style 10"),
             "the debug line arrived, formatted");
}

static size_t longest_length;

static void measure(rm_log_level level, const char *message, void *context) {
    (void)level;
    (void)context;
    size_t length = strlen(message);
    if (length > longest_length) longest_length = length;
}

/* A line longer than the stack buffer reaches the sink whole, as every line
 * does. */
static void a_long_line_arrives_whole(void) {
    char word[2001];
    memset(word, 'x', sizeof word - 1u);
    word[sizeof word - 1u] = '\0';
    longest_length = 0;
    rm_log_install(measure, nullptr);
    rm_log_writef(RM_LOG_DEBUG, "long: %s.", word);
    rm_log_silence();
    RM_CHECK_EQ_SIZE(longest_length, strlen("long: ") + 2000u + 1u, "every character arrived");
}

/* With no sink installed nothing is built and nothing is written. The message
 * is behind `rm_log_wanted`: with no sink there is not even a call to
 * `snprintf`, which is what makes a debug line on the parsing path free. */
static void silence_is_the_default(void) {
    collected seen = { 0 };
    rm_log_install(collect, &seen);
    rm_log_silence();

    RM_CHECK(!rm_log_wanted(), "nothing wants a line");
    RM_LOG_DEBUGF("never %s", "built");
    RM_CHECK_EQ_SIZE(seen.count, 0, "and none was written");
}

static void the_latest_sink_stands(void) {
    collected first = { 0 };
    collected second = { 0 };
    rm_log_install(collect, &first);
    rm_log_install(collect, &second);
    rm_log_writef(RM_LOG_ERROR, "page: an image without a name in the attachment map is lost");
    rm_log_silence();

    RM_CHECK_EQ_SIZE(first.count, 0, "the sink that was replaced hears nothing");
    RM_CHECK_EQ_SIZE(second.count, 1, "and the one that replaced it hears everything");
}

/* The one place a parse and a sink are exercised together: a stroke block whose
 * payload is not a stroke. The page survives, the loss is counted, and — the
 * point of this test — it is said out loud. */
static void a_lost_stroke_reaches_the_sink_as_an_error(void) {
    collected seen = { 0 };
    rm_log_install(collect, &seen);

    rm_buf file;
    rm_buf_init(&file);
    rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
    uint8_t payload[24] = { 0 };
    rm_write_top_block(&file, 0x05, 1, 1, payload, sizeof payload);

    rm_page *page = nullptr;
    rm_error error;
    rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
    rm_log_silence();

    RM_CHECK(status == RM_OK, "the page still parses");
    if (status == RM_OK) {
        RM_CHECK_EQ_SIZE(page->skipped_blocks, 1, "the loss is counted");
        rm_page_free(page);
    }
    RM_CHECK(saw(&seen, RM_LOG_ERROR, "unreadable"), "and said out loud");
    rm_buf_free(&file);
}

/* The two losses that are easy to miss: an image item and a group item whose
 * value is there but not of its shape. Read as tombstones, the page would take
 * them for erasures, with no line written for either. */
static void a_lost_image_reaches_the_sink_as_an_error(void) {
    size_t size = 0;
    uint8_t *bytes = rm_damaged_image_with_three_corners(&size);
    RM_CHECK(bytes != nullptr, "the damaged page is built");
    if (bytes == nullptr) return;
    collected seen = { 0 };
    rm_log_install(collect, &seen);
    rm_page *page = nullptr;
    rm_status status = rm_parse_page(bytes, size, &page, nullptr);
    rm_log_silence();
    RM_CHECK(status == RM_OK && page->skipped_blocks == 1, "the loss is counted");
    RM_CHECK(saw(&seen, RM_LOG_ERROR, "1 other blocks"), "and said out loud");
    RM_CHECK(saw(&seen, RM_LOG_ERROR, "3 vertices, not 4"), "with its reason");
    rm_page_free(page);
    rm_bytes_free(bytes);
}

static void a_lost_group_item_reaches_the_sink_as_an_error(void) {
    size_t size = 0;
    uint8_t *bytes = rm_damaged_mistyped_group_item(&size);
    RM_CHECK(bytes != nullptr, "the damaged page is built");
    if (bytes == nullptr) return;
    collected seen = { 0 };
    rm_log_install(collect, &seen);
    rm_page *page = nullptr;
    rm_status status = rm_parse_page(bytes, size, &page, nullptr);
    rm_log_silence();
    RM_CHECK(status == RM_OK && page->skipped_blocks == 1, "the loss is counted");
    RM_CHECK(saw(&seen, RM_LOG_ERROR, "not a group"), "and said out loud, with its reason");
    rm_page_free(page);
    rm_bytes_free(bytes);
}

int main(void) {
    an_installed_sink_receives_the_line_with_its_level();
    a_long_line_arrives_whole();
    silence_is_the_default();
    the_latest_sink_stands();
    a_lost_stroke_reaches_the_sink_as_an_error();
    a_lost_image_reaches_the_sink_as_an_error();
    a_lost_group_item_reaches_the_sink_as_an_error();
    return rm_test_report("log");
}
