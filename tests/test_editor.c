/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The editor against pages it did not write: the sample corpus, which the
 * tablet did. An erase has to leave the page reading as the tablet would have
 * left it, and every block it was not asked about exactly as it was. And the
 * marker on a layer the user hid: not drawn, so not on the page
 * `rm_parse_page` answers with — and still on the tablet, where showing the
 * layer brings it back. An erase of every mark has to find it. */

#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

static const char *const SAMPLES[] = {
    "Bold_Heading_Bullet_Normal.rm",
    "Color_and_tool_v3.14.4.rm",
    "Lines_v2.rm",
    "Lines_v2_updated.rm",
    "More_color_highlight_shader_v3.15.4.2.rm",
    "Normal_AB.rm",
    "Normal_A_stroke_2_layers.rm",
    "Normal_A_stroke_2_layers_v3.2.2.rm",
    "Normal_A_stroke_2_layers_v3.3.2.rm",
    "Wikipedia_highlighted_p1.rm",
    "Wikipedia_highlighted_p2.rm",
    "With_SceneInfo_Block.rm",
    "test-crdt-ordering.rm",
};
constexpr size_t SAMPLE_COUNT = sizeof SAMPLES / sizeof *SAMPLES;

static uint8_t *sample(const char *name, size_t *out_size) {
    char path[1024];
    rm_test_fixture_path(path, sizeof path, name);
    return rm_test_read_file(path, out_size);
}

static rm_page *parse(const uint8_t *bytes, size_t size) {
    rm_page *page = nullptr;
    rm_error error;
    if (rm_parse_page(bytes, size, &page, &error) != RM_OK) return nullptr;
    return page;
}

/* ------------------------------------------------------------------ */
/* The blocks of a page as they lie in the bytes                       */
/* ------------------------------------------------------------------ */

typedef struct block_view {
    uint8_t unknown, min_version, current_version, type;
    const uint8_t *body;
    size_t length;
} block_view;

static size_t blocks_of(const uint8_t *page, size_t size, block_view *out, size_t capacity) {
    size_t offset = RM_HEADER_LENGTH;
    size_t count = 0;
    while (offset + 8 <= size && count < capacity) {
        size_t length = (size_t)page[offset] | (size_t)page[offset + 1] << 8 |
                        (size_t)page[offset + 2] << 16 | (size_t)page[offset + 3] << 24;
        size_t start = offset + 8;
        if (start + length > size) break;
        out[count++] = (block_view){
            .unknown = page[offset + 4],
            .min_version = page[offset + 5],
            .current_version = page[offset + 6],
            .type = page[offset + 7],
            .body = page + start,
            .length = length,
        };
        offset = start + length;
    }
    return count;
}

static bool same_block(const block_view *a, const block_view *b) {
    return a->unknown == b->unknown && a->min_version == b->min_version &&
           a->current_version == b->current_version && a->type == b->type &&
           a->length == b->length && memcmp(a->body, b->body, a->length) == 0;
}

/* Every block of `edited` equal to its counterpart in `original`, save the
 * ones that were erased — and those in the shape the tablet leaves: the same
 * header, the body cut after the deleted length, and that length one. */
static void expect_only_erased(size_t erased, const uint8_t *original, size_t original_size,
                               const uint8_t *edited, size_t edited_size, const char *name) {
    enum { CAPACITY = 4096 };
    static block_view before[CAPACITY], after[CAPACITY];
    size_t before_count = blocks_of(original, original_size, before, CAPACITY);
    size_t after_count = blocks_of(edited, edited_size, after, CAPACITY);
    RM_CHECK_EQ_SIZE(after_count, before_count, "%s: as many blocks as before", name);
    size_t changed = 0;
    for (size_t i = 0; i < before_count && i < after_count; i += 1) {
        const block_view *old = &before[i];
        const block_view *new = &after[i];
        if (same_block(old, new)) continue;
        changed += 1;
        RM_CHECK(new->type == old->type && new->unknown == old->unknown, "%s", name);
        RM_CHECK(new->min_version == old->min_version &&
                 new->current_version == old->current_version, "%s", name);
        RM_CHECK(new->type == 0x05 || new->type == 0x03, "%s: only marks change", name);
        /* The preamble kept whole, the count set to one, nothing behind it. */
        RM_CHECK(new->length < old->length && new->length >= 4, "%s: the block shrinks", name);
        if (new->length >= 4 && new->length < old->length) {
            size_t kept = new->length - 4;
            static const uint8_t one[4] = { 1, 0, 0, 0 };
            static const uint8_t none[4] = { 0, 0, 0, 0 };
            RM_CHECK(memcmp(new->body, old->body, kept) == 0, "%s: the preamble is kept", name);
            RM_CHECK(memcmp(new->body + kept, one, 4) == 0, "%s: deleted length one", name);
            RM_CHECK(kept + 4 <= old->length && memcmp(old->body + kept, none, 4) == 0,
                     "%s: where the living item said nought", name);
        }
    }
    RM_CHECK_EQ_SIZE(changed, erased, "%s: exactly the erased blocks changed", name);
}

/* ------------------------------------------------------------------ */
/* Pages built by hand                                                 */
/* ------------------------------------------------------------------ */

static void append_block(rm_buf *out, uint8_t type, uint8_t min_version, uint8_t current_version,
                         const uint8_t *body, size_t length) {
    rm_write_top_block(out, type, min_version, current_version, body, length);
}

/* A scene item's preamble: parent, item, left, right, deleted length. */
static void append_raw_preamble(rm_buf *out, uint8_t item, uint8_t deleted) {
    const uint8_t bytes[] = { 0x1F, 0, 11, 0x2F, 1, item, 0x3F, 0, 0, 0x4F, 0, 0,
                              0x54, deleted, 0, 0, 0 };
    rm_buf_append(out, bytes, sizeof bytes);
}

static void start_page(rm_buf *page) {
    rm_buf_init(page);
    rm_buf_append(page, RM_HEADER, RM_HEADER_LENGTH);
}

static void append_zeros_block(rm_buf *page, uint8_t type, size_t length) {
    uint8_t *zeros = calloc(length == 0 ? 1 : length, 1);
    append_block(page, type, 0, 1, zeros, length);
    free(zeros);
}

/* A value of a highlight: type 1, colour 3, and the rectangles field as given. */
static void append_glyph(rm_buf *page, const uint8_t *rectangles, size_t length) {
    rm_buf value = { 0 }, body = { 0 };
    const uint8_t head[] = { 0x01, 0x44, 3, 0, 0, 0 };
    rm_buf_append(&value, head, sizeof head);
    rm_buf_append(&value, rectangles, length);
    append_raw_preamble(&body, 20, 0);
    rm_write_block(&body, 6, value.bytes, value.count);
    append_block(page, 0x03, 1, 1, body.bytes, body.count);
    rm_buf_free(&value);
    rm_buf_free(&body);
}

/* ------------------------------------------------------------------ */
/* Reading what an edit leaves                                         */
/* ------------------------------------------------------------------ */

/* A tombstone's block ends right behind its preamble, and the next block
 * begins with its length: 108 is 0x6C, the tag of a value. Read past the
 * block, the tombstone came back as a stroke whose value was the next block's
 * header, and was counted as lost. */
static void a_tombstone_before_a_108_byte_block_is_a_tombstone(void) {
    const uint8_t types[] = { 0x05, 0x03 };
    for (size_t i = 0; i < 2; i += 1) {
        rm_buf page, body = { 0 };
        start_page(&page);
        append_raw_preamble(&body, 20, 1);
        append_block(&page, types[i], 2, 2, body.bytes, body.count);
        /* The page counters: stepped over by length, never counted. */
        append_zeros_block(&page, 0x0A, 108);
        rm_page *parsed = parse(page.bytes, page.count);
        RM_CHECK(parsed != nullptr, "type %u: the page parses", types[i]);
        if (parsed != nullptr) {
            RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "type %u: nothing lost", types[i]);
            RM_CHECK_EQ_SIZE(parsed->stroke_count, 0, "type %u: no stroke", types[i]);
            RM_CHECK_EQ_SIZE(parsed->highlight_count, 0, "type %u: no highlight", types[i]);
            rm_page_free(parsed);
        }
        rm_buf_free(&body);
        rm_buf_free(&page);
    }
}

/* A value that ends before its optional colour: the byte behind it is the
 * next block's length, and 420 spells the colour's tag, A4 01. */
static void a_highlight_takes_no_colour_from_the_block_after_it(void) {
    uint8_t rectangles[6 + 32] = { 0x6C, 33, 0, 0, 0, 1 };
    rm_buf page;
    start_page(&page);
    append_glyph(&page, rectangles, sizeof rectangles);
    append_zeros_block(&page, 0x0A, 420);
    rm_page *parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr, "the page parses");
    if (parsed != nullptr) {
        RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "nothing lost");
        RM_CHECK_EQ_SIZE(parsed->highlight_count, 1, "the highlight is read");
        if (parsed->highlight_count == 1) {
            RM_CHECK(!parsed->highlights[0].has_argb, "and takes no colour it does not carry");
            RM_CHECK(rm_item_id_equal(parsed->highlights[0].id,
                                      (rm_item_id){ .author = 1, .counter = 20 }),
                     "and carries its own identifier");
        }
        rm_page_free(parsed);
    }
    rm_buf_free(&page);
}

static void a_highlight_whose_rectangles_run_past_its_value_is_a_counted_loss(void) {
    /* Two rectangles declared, room for one. */
    uint8_t rectangles[6 + 32] = { 0x6C, 33, 0, 0, 0, 2 };
    rm_buf page;
    start_page(&page);
    append_glyph(&page, rectangles, sizeof rectangles);
    append_zeros_block(&page, 0x0A, 64);
    rm_page *parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1, "the highlight is counted lost");
    rm_page_free(parsed);
    rm_buf_free(&page);
}

static void a_highlight_with_neither_value_nor_deletion_is_a_counted_loss(void) {
    rm_buf page, body = { 0 };
    start_page(&page);
    append_raw_preamble(&body, 20, 0);
    append_block(&page, 0x03, 1, 1, body.bytes, body.count);
    rm_page *parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1, "a loss, not an erasure");
    rm_page_free(parsed);
    rm_buf_free(&body);
    rm_buf_free(&page);
}

static void a_highlight_whose_value_will_not_read_is_a_counted_loss(void) {
    /* A value tag, a length, and then nothing a highlight is made of. */
    rm_buf page, body = { 0 };
    start_page(&page);
    append_raw_preamble(&body, 20, 0);
    const uint8_t value[] = { 0x6C, 2, 0, 0, 0, 1, 0 };
    rm_buf_append(&body, value, sizeof value);
    append_block(&page, 0x03, 1, 1, body.bytes, body.count);
    rm_page *parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1, "the broken highlight is counted");
    rm_page_free(parsed);
    rm_buf_free(&body);
    rm_buf_free(&page);
}

static bool contains_id(const rm_item_id *ids, size_t count, rm_item_id id) {
    for (size_t i = 0; i < count; i += 1) {
        if (rm_item_id_equal(ids[i], id)) return true;
    }
    return false;
}

static void every_mark_read_out_of_a_page_carries_its_own_identifier(void) {
    for (size_t s = 0; s < SAMPLE_COUNT; s += 1) {
        size_t size = 0;
        uint8_t *bytes = sample(SAMPLES[s], &size);
        RM_CHECK(bytes != nullptr, "%s reads", SAMPLES[s]);
        rm_page *page = bytes == nullptr ? nullptr : parse(bytes, size);
        if (page != nullptr) {
            size_t total = page->stroke_count + page->highlight_count;
            rm_item_id *ids = calloc(total == 0 ? 1 : total, sizeof *ids);
            size_t distinct = 0;
            for (size_t i = 0; i < total; i += 1) {
                rm_item_id id = i < page->stroke_count
                                    ? page->strokes[i].id
                                    : page->highlights[i - page->stroke_count].id;
                if (!contains_id(ids, distinct, id)) ids[distinct++] = id;
            }
            RM_CHECK_EQ_SIZE(distinct, total, "%s: no two marks share one", SAMPLES[s]);
            free(ids);
            rm_page_free(page);
        }
        free(bytes);
    }
}

static void an_identifier_spells_as_author_colon_counter(void) {
    rm_item_id id = { .author = 2, .counter = 1234 };
    char spelled[32];
    RM_CHECK(rm_item_id_format(id, spelled, sizeof spelled) == 6, "six characters");
    RM_CHECK_EQ_STR(spelled, strlen(spelled), "2:1234", "the spelling");
    rm_item_id back = { 0 };
    RM_CHECK(rm_item_id_parse("2:1234", &back) && rm_item_id_equal(back, id), "reads back");
    RM_CHECK(rm_item_id_parse("255:18446744073709551615", &back) && back.author == 255 &&
                 back.counter == UINT64_MAX, "the widest reads back");
    char widest[25];
    RM_CHECK(rm_item_id_format(back, widest, sizeof widest) == 24 &&
                 strcmp(widest, "255:18446744073709551615") == 0,
             "and spells whole in 25 bytes");
    /* One leading sign is allowed on each unsigned number: a plus on anything,
     * a minus only on a nought. */
    RM_CHECK(rm_item_id_parse("+2:+1", &back) && back.author == 2 && back.counter == 1,
             "a plus is no part of the number");
    RM_CHECK(rm_item_id_parse("-0:-00", &back) && back.author == 0 && back.counter == 0,
             "and a minus on a nought is a nought");
    const char *const refused[] = { "2:", ":1", "256:1", "2:1:3", "-1:2", " 2:1", "2:+",
                                    "2:+-1", "2:-1", "2:18446744073709551616", "", "21" };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i += 1) {
        RM_CHECK(!rm_item_id_parse(refused[i], &back), "\"%s\" is not an identifier", refused[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Erasing                                                             */
/* ------------------------------------------------------------------ */

static void a_highlight_is_erased_and_the_rest_stays(void) {
    size_t size = 0;
    uint8_t *data = sample("Wikipedia_highlighted_p1.rm", &size);
    rm_page *page = data == nullptr ? nullptr : parse(data, size);
    RM_CHECK(page != nullptr && page->highlight_count > 1, "the sample has highlights");
    if (page != nullptr && page->highlight_count > 1) {
        rm_item_id target = page->highlights[1].id;
        uint8_t *edited = nullptr;
        size_t edited_size = 0;
        rm_error error;
        rm_status status = rm_erase_items(data, size, &target, 1, &edited, &edited_size, &error);
        RM_CHECK(status == RM_OK, "the erase goes through (%s: %s)", rm_status_string(status),
                 error.message);
        rm_page *after = status == RM_OK ? parse(edited, edited_size) : nullptr;
        if (after != nullptr) {
            RM_CHECK_EQ_SIZE(after->highlight_count, page->highlight_count - 1, "one fewer");
            size_t j = 0;
            for (size_t i = 0; i < page->highlight_count; i += 1) {
                if (rm_item_id_equal(page->highlights[i].id, target)) continue;
                RM_CHECK(j < after->highlight_count &&
                             rm_item_id_equal(after->highlights[j].id, page->highlights[i].id),
                         "the others in their order");
                j += 1;
            }
            /* A highlight taken off is a tombstone, not a loss. */
            RM_CHECK_EQ_SIZE(after->skipped_blocks, 0, "and nothing is lost");
            expect_only_erased(1, data, size, edited, edited_size, "Wikipedia_highlighted_p1.rm");
            rm_page_free(after);
        }
        rm_bytes_free(edited);
    }
    rm_page_free(page);
    free(data);
}

static void a_stroke_is_erased_as_the_tablet_erases_one(void) {
    size_t size = 0;
    uint8_t *data = sample("Color_and_tool_v3.14.4.rm", &size);
    rm_page *page = data == nullptr ? nullptr : parse(data, size);
    RM_CHECK(page != nullptr && page->stroke_count > 2, "the sample has strokes");
    if (page != nullptr && page->stroke_count > 2) {
        /* One from the middle of the chain: the strokes after it hang on it. */
        rm_item_id target = page->strokes[page->stroke_count / 2].id;
        uint8_t *edited = nullptr;
        size_t edited_size = 0;
        rm_status status = rm_erase_items(data, size, &target, 1, &edited, &edited_size, nullptr);
        RM_CHECK(status == RM_OK, "the erase goes through (%s)", rm_status_string(status));
        rm_page *after = status == RM_OK ? parse(edited, edited_size) : nullptr;
        if (after != nullptr) {
            RM_CHECK_EQ_SIZE(after->stroke_count, page->stroke_count - 1, "one fewer stroke");
            size_t j = 0;
            for (size_t i = 0; i < page->stroke_count; i += 1) {
                if (rm_item_id_equal(page->strokes[i].id, target)) continue;
                RM_CHECK(j < after->stroke_count &&
                             rm_item_id_equal(after->strokes[j].id, page->strokes[i].id),
                         "the others in their order");
                j += 1;
            }
            RM_CHECK_EQ_SIZE(after->highlight_count, page->highlight_count, "highlights stay");
            RM_CHECK_EQ_SIZE(after->skipped_blocks, 0, "and nothing is lost");
            expect_only_erased(1, data, size, edited, edited_size, "Color_and_tool_v3.14.4.rm");
            rm_page_free(after);
        }
        rm_bytes_free(edited);
    }
    rm_page_free(page);
    free(data);
}

static void everything_on_every_sample_erased_at_once_reads_back_empty(void) {
    for (size_t s = 0; s < SAMPLE_COUNT; s += 1) {
        const char *name = SAMPLES[s];
        size_t size = 0;
        uint8_t *data = sample(name, &size);
        rm_page *page = data == nullptr ? nullptr : parse(data, size);
        RM_CHECK(page != nullptr, "%s parses", name);
        size_t total = page == nullptr ? 0 : page->stroke_count + page->highlight_count;
        if (total == 0) {
            rm_page_free(page);
            free(data);
            continue;
        }
        rm_item_id *ids = calloc(total, sizeof *ids);
        for (size_t i = 0; i < total; i += 1) {
            ids[i] = i < page->stroke_count ? page->strokes[i].id
                                            : page->highlights[i - page->stroke_count].id;
        }
        uint8_t *edited = nullptr;
        size_t edited_size = 0;
        rm_error error;
        rm_status status = rm_erase_items(data, size, ids, total, &edited, &edited_size, &error);
        RM_CHECK(status == RM_OK, "%s: the erase goes through (%s: %s)", name,
                 rm_status_string(status), error.message);
        rm_page *after = status == RM_OK ? parse(edited, edited_size) : nullptr;
        if (after != nullptr) {
            RM_CHECK_EQ_SIZE(after->stroke_count, 0, "%s: no stroke left", name);
            RM_CHECK_EQ_SIZE(after->highlight_count, 0, "%s: no highlight left", name);
            RM_CHECK_EQ_SIZE(after->skipped_blocks, page->skipped_blocks, "%s: no new loss", name);
            /* The typed text is not the eraser's business. */
            RM_CHECK(after->has_text == page->has_text, "%s: the text stays", name);
            if (after->has_text && page->has_text) {
                RM_CHECK_EQ_SIZE(after->text.paragraph_count, page->text.paragraph_count,
                                 "%s: every paragraph", name);
                for (size_t p = 0; p < page->text.paragraph_count &&
                                   p < after->text.paragraph_count; p += 1) {
                    RM_CHECK_EQ_STR(after->text.paragraphs[p].text.bytes,
                                    after->text.paragraphs[p].text.length,
                                    page->text.paragraphs[p].text.bytes, "%s: line %zu", name, p);
                }
            }
            expect_only_erased(total, data, size, edited, edited_size, name);
            rm_page_free(after);
        }
        rm_bytes_free(edited);
        free(ids);
        rm_page_free(page);
        free(data);
    }
}

static void an_identifier_the_page_does_not_hold_refuses_the_whole_edit(void) {
    size_t size = 0;
    uint8_t *data = sample("Wikipedia_highlighted_p1.rm", &size);
    rm_page *page = data == nullptr ? nullptr : parse(data, size);
    RM_CHECK(page != nullptr && page->highlight_count > 0, "the sample has a highlight");
    if (page != nullptr && page->highlight_count > 0) {
        rm_item_id ids[] = { page->highlights[0].id, { .author = 9, .counter = 999999 } };
        uint8_t *edited = nullptr;
        size_t edited_size = 0;
        rm_error error;
        rm_status status = rm_erase_items(data, size, ids, 2, &edited, &edited_size, &error);
        RM_CHECK(status == RM_ERR_ITEMS_NOT_FOUND, "refused (%s)", rm_status_string(status));
        RM_CHECK(error.status == status && error.value == 1, "one missing (%ld)", error.value);
        RM_CHECK_EQ_STR(error.message, strlen(error.message), "not on the page: 9:999999",
                        "and named");
        RM_CHECK(edited == nullptr, "nothing is handed over");
        rm_bytes_free(edited);
    }
    rm_page_free(page);
    free(data);
}

static void an_item_already_erased_is_not_there_to_erase_again(void) {
    size_t size = 0;
    uint8_t *data = sample("Wikipedia_highlighted_p1.rm", &size);
    rm_page *page = data == nullptr ? nullptr : parse(data, size);
    if (page != nullptr && page->highlight_count > 0) {
        rm_item_id target = page->highlights[0].id;
        uint8_t *once = nullptr, *twice = nullptr;
        size_t once_size = 0, twice_size = 0;
        RM_CHECK(rm_erase_items(data, size, &target, 1, &once, &once_size, nullptr) == RM_OK,
                 "the first erase goes through");
        rm_error error;
        rm_status status = once == nullptr
                               ? RM_OK
                               : rm_erase_items(once, once_size, &target, 1, &twice, &twice_size,
                                                &error);
        RM_CHECK(status == RM_ERR_ITEMS_NOT_FOUND, "the second is refused (%s)",
                 rm_status_string(status));
        rm_bytes_free(once);
        rm_bytes_free(twice);
    } else {
        RM_CHECK(false, "the sample has a highlight");
    }
    rm_page_free(page);
    free(data);
}

static void nothing_asked_nothing_changed(void) {
    size_t size = 0;
    uint8_t *data = sample("Color_and_tool_v3.14.4.rm", &size);
    RM_CHECK(data != nullptr, "the sample reads");
    if (data == nullptr) return;
    uint8_t *edited = nullptr;
    size_t edited_size = 0;
    RM_CHECK(rm_erase_items(data, size, nullptr, 0, &edited, &edited_size, nullptr) == RM_OK,
             "an empty edit goes through");
    RM_CHECK(edited != nullptr && edited_size == size && memcmp(edited, data, size) == 0,
             "and changes nothing");
    rm_bytes_free(edited);
    free(data);
}

static void a_page_that_is_not_a_page_is_refused_as_the_parser_refuses_it(void) {
    const char garbage[] = "not a page";
    rm_item_id id = { .author = 1, .counter = 1 };
    uint8_t *edited = nullptr;
    size_t edited_size = 0;
    rm_error error;
    rm_status status = rm_erase_items((const uint8_t *)garbage, strlen(garbage), &id, 1, &edited,
                                      &edited_size, &error);
    /* Shorter than a header: the stream ends inside the first field, as the
     * parser says of it. */
    RM_CHECK(status == RM_ERR_UNEXPECTED_EOF, "refused as a stream cut short (%s)",
             rm_status_string(status));
    RM_CHECK(edited == nullptr, "nothing is handed over");
}

/* ------------------------------------------------------------------ */
/* The marker on hidden layers                                         */
/* ------------------------------------------------------------------ */

/* A layer node, hidden or shown, and its link into the root. */
static void append_layer(rm_buf *page, rm_crdt_id node, bool hidden, rm_crdt_id link) {
    rm_buf state = { 0 }, group = { 0 }, value = { 0 };
    rm_write_id(&state, 1, node);
    if (hidden) rm_write_stamped_u8(&state, 3, rm_crdt_make(1, 90), 0);
    rm_write_id(&group, 1, rm_crdt_make(0, 1));
    rm_write_id(&group, 2, link);
    rm_write_id(&group, 3, RM_CRDT_NONE);
    rm_write_id(&group, 4, RM_CRDT_NONE);
    rm_write_tag(&group, 5, RM_TAG_BYTE4);
    rm_write_u32(&group, 0);
    rm_buf_append_byte(&value, 2);
    rm_write_id(&value, 2, node);
    rm_write_block(&group, 6, value.bytes, value.count);
    append_block(page, 2, 1, 1, state.bytes, state.count);
    append_block(page, 4, 1, 1, group.bytes, group.count);
    rm_buf_free(&state);
    rm_buf_free(&group);
    rm_buf_free(&value);
}

/* A group nested under `parent`, linked by `link`. */
static void append_nested_group(rm_buf *page, rm_crdt_id parent, rm_crdt_id link, rm_crdt_id node) {
    rm_buf group = { 0 }, value = { 0 };
    rm_write_id(&group, 1, parent);
    rm_write_id(&group, 2, link);
    rm_write_id(&group, 3, RM_CRDT_NONE);
    rm_write_id(&group, 4, RM_CRDT_NONE);
    rm_write_tag(&group, 5, RM_TAG_BYTE4);
    rm_write_u32(&group, 0);
    rm_buf_append_byte(&value, 2);
    rm_write_id(&value, 2, node);
    rm_write_block(&group, 6, value.bytes, value.count);
    append_block(page, 4, 1, 1, group.bytes, group.count);
    rm_buf_free(&group);
    rm_buf_free(&value);
}

/* A stroke of `tool` under `parent`, two points long. */
static void append_stroke(rm_buf *page, rm_crdt_id id, rm_crdt_id parent, rm_pen_tool tool) {
    rm_buf value = { 0 }, points = { 0 }, line = { 0 };
    rm_buf_append_byte(&value, 0x03);
    rm_write_tag(&value, 1, RM_TAG_BYTE4);
    rm_write_u32(&value, (uint32_t)tool);
    rm_write_tag(&value, 2, RM_TAG_BYTE4);
    rm_write_u32(&value, 0);
    rm_write_tag(&value, 3, RM_TAG_BYTE8);
    rm_write_f64(&value, 1);
    rm_write_tag(&value, 4, RM_TAG_BYTE4);
    rm_write_f32(&value, 0);
    const float xs[] = { 0, 100 };
    for (size_t i = 0; i < 2; i += 1) {
        rm_write_f32(&points, xs[i]);
        rm_write_f32(&points, 200);
        rm_write_u16(&points, 0);
        rm_write_u16(&points, 60);
        rm_buf_append_byte(&points, 0);
        rm_buf_append_byte(&points, 255);
    }
    rm_write_block(&value, 5, points.bytes, points.count);
    rm_write_id(&line, 1, parent);
    rm_write_id(&line, 2, id);
    rm_write_id(&line, 3, RM_CRDT_NONE);
    rm_write_id(&line, 4, RM_CRDT_NONE);
    rm_write_tag(&line, 5, RM_TAG_BYTE4);
    rm_write_u32(&line, 0);
    rm_write_block(&line, 6, value.bytes, value.count);
    append_block(page, 5, 2, 2, line.bytes, line.count);
    rm_buf_free(&value);
    rm_buf_free(&points);
    rm_buf_free(&line);
}

/* A layer taken out of the tree: the record that removes it. */
static void append_removal(rm_buf *page, rm_crdt_id node) {
    rm_buf record = { 0 };
    rm_write_id(&record, 1, rm_crdt_make(1, 95));
    rm_write_id(&record, 2, node);
    rm_write_tag(&record, 3, RM_TAG_BYTE1);
    rm_buf_append_byte(&record, 0);
    append_block(page, 1, 1, 1, record.bytes, record.count);
    rm_buf_free(&record);
}

static const rm_crdt_id SHOWN = { .counter = 11, .author = 1 };
static const rm_crdt_id HIDDEN = { .counter = 21, .author = 1 };

static void append_two_layers(rm_buf *page, bool second_hidden) {
    append_layer(page, SHOWN, false, rm_crdt_make(1, 12));
    append_layer(page, HIDDEN, second_hidden, rm_crdt_make(1, 22));
}

static void append_three_strokes(rm_buf *page) {
    append_stroke(page, rm_crdt_make(1, 30), SHOWN, RM_TOOL_HIGHLIGHTER_2);
    append_stroke(page, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_HIGHLIGHTER_2);
    append_stroke(page, rm_crdt_make(1, 32), HIDDEN, RM_TOOL_FINELINER_2);
}

static size_t highlighter_items(const uint8_t *bytes, size_t size, rm_item_id **out) {
    size_t count = 0;
    *out = nullptr;
    rm_status status = rm_highlighter_items(bytes, size, out, &count, nullptr);
    RM_CHECK(status == RM_OK, "the marks are found (%s)", rm_status_string(status));
    return count;
}

static void the_marker_on_a_hidden_layer_is_found_and_the_pen_is_not(void) {
    rm_buf page;
    start_page(&page);
    append_two_layers(&page, true);
    append_three_strokes(&page);

    rm_page *drawn = parse(page.bytes, page.count);
    RM_CHECK(drawn != nullptr, "the page parses");
    if (drawn != nullptr) {
        /* Drawn: the shown layer's marker alone. */
        RM_CHECK(drawn->stroke_count == 1 &&
                     rm_item_id_equal(drawn->strokes[0].id, (rm_item_id){ 1, 30 }),
                 "only the shown marker is drawn");
        RM_CHECK_EQ_SIZE(drawn->skipped_blocks, 0, "nothing lost");
        rm_page_free(drawn);
    }
    rm_item_id *ids;
    size_t count = highlighter_items(page.bytes, page.count, &ids);
    RM_CHECK(count == 2 && contains_id(ids, count, (rm_item_id){ 1, 30 }) &&
                 contains_id(ids, count, (rm_item_id){ 1, 31 }),
             "both markers, and not the pen (%zu found)", count);
    rm_item_ids_free(ids);
    rm_buf_free(&page);
}

static void the_marker_on_a_hidden_layer_is_erased_and_the_layer_keeps_its_pen(void) {
    rm_buf page;
    start_page(&page);
    append_two_layers(&page, true);
    size_t layers_end = page.count;
    append_three_strokes(&page);

    rm_item_id *marks;
    size_t mark_count = highlighter_items(page.bytes, page.count, &marks);
    uint8_t *edited = nullptr;
    size_t edited_size = 0;
    rm_status status = rm_erase_items(page.bytes, page.count, marks, mark_count, &edited,
                                      &edited_size, nullptr);
    RM_CHECK(status == RM_OK, "the erase goes through (%s)", rm_status_string(status));
    rm_item_ids_free(marks);
    if (status != RM_OK) { rm_buf_free(&page); return; }

    rm_item_id *left;
    RM_CHECK_EQ_SIZE(highlighter_items(edited, edited_size, &left), 0, "no marker is left");
    rm_item_ids_free(left);
    rm_page *after = parse(edited, edited_size);
    RM_CHECK(after != nullptr && after->stroke_count == 0 && after->skipped_blocks == 0,
             "nothing drawn and nothing lost");
    rm_page_free(after);

    /* Shown again, the layer holds the pen and nothing of the marker. */
    rm_buf shown_again;
    start_page(&shown_again);
    append_two_layers(&shown_again, false);
    rm_buf_append(&shown_again, edited + layers_end, edited_size - layers_end);
    rm_page *again = parse(shown_again.bytes, shown_again.count);
    RM_CHECK(again != nullptr && again->stroke_count == 1 &&
                 rm_item_id_equal(again->strokes[0].id, (rm_item_id){ 1, 32 }),
             "the pen alone is back");
    rm_page_free(again);
    rm_buf_free(&shown_again);
    rm_bytes_free(edited);
    rm_buf_free(&page);
}

static void a_layer_taken_out_of_the_tree_is_not_searched(void) {
    rm_buf page;
    start_page(&page);
    append_layer(&page, HIDDEN, false, rm_crdt_make(1, 22));
    append_removal(&page, HIDDEN);
    append_stroke(&page, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_HIGHLIGHTER_2);
    rm_item_id *ids;
    RM_CHECK_EQ_SIZE(highlighter_items(page.bytes, page.count, &ids), 0, "nothing brings it back");
    rm_item_ids_free(ids);
    rm_buf_free(&page);
}

static void a_group_nested_in_a_removed_layer_is_as_gone_as_the_layer(void) {
    const rm_crdt_id nested = rm_crdt_make(1, 41);
    rm_buf page;
    start_page(&page);
    append_layer(&page, HIDDEN, false, rm_crdt_make(1, 22));
    append_nested_group(&page, HIDDEN, rm_crdt_make(1, 42), nested);
    append_removal(&page, HIDDEN);
    append_stroke(&page, rm_crdt_make(1, 31), nested, RM_TOOL_HIGHLIGHTER_2);
    rm_item_id *ids;
    RM_CHECK_EQ_SIZE(highlighter_items(page.bytes, page.count, &ids), 0, "the nested goes too");
    rm_item_ids_free(ids);
    rm_buf_free(&page);
}

static void a_page_with_a_block_that_will_not_read_is_refused_not_answered_in_part(void) {
    rm_buf page, broken;
    start_page(&page);
    append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&page, rm_crdt_make(1, 30), SHOWN, RM_TOOL_HIGHLIGHTER_2);
    /* The value's type byte turned into something no stroke is: the byte
     * right behind the value's tag and length. */
    rm_buf_init(&broken);
    append_stroke(&broken, rm_crdt_make(1, 33), SHOWN, RM_TOOL_HIGHLIGHTER_2);
    bool corrupted = false;
    for (size_t i = 8; !corrupted && i + 5 < broken.count; i += 1) {
        if (broken.bytes[i] == 0x6C && broken.bytes[i + 5] == 0x03) {
            broken.bytes[i + 5] = 0x7E;
            corrupted = true;
        }
    }
    RM_CHECK(corrupted, "the stroke's value type is found");
    rm_buf_append(&page, broken.bytes, broken.count);

    rm_page *parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1, "the page loses one block");
    rm_page_free(parsed);

    rm_item_id *ids = nullptr;
    size_t count = 0;
    rm_error error;
    rm_status status = rm_highlighter_items(page.bytes, page.count, &ids, &count, &error);
    RM_CHECK(status == RM_ERR_INCOMPLETE_PAGE, "refused (%s)", rm_status_string(status));
    RM_CHECK(error.status == status && error.value == 1, "one block lost (%ld)", error.value);
    RM_CHECK(ids == nullptr && count == 0, "nothing is handed over");
    rm_item_ids_free(ids);
    rm_buf_free(&broken);
    rm_buf_free(&page);
}

static void highlights_over_a_text_layer_are_found_with_the_strokes(void) {
    size_t size = 0;
    uint8_t *data = sample("Wikipedia_highlighted_p1.rm", &size);
    rm_page *drawn = data == nullptr ? nullptr : parse(data, size);
    RM_CHECK(drawn != nullptr, "the sample parses");
    if (drawn != nullptr) {
        rm_item_id *ids;
        size_t count = highlighter_items(data, size, &ids);
        RM_CHECK_EQ_SIZE(count, drawn->highlight_count, "every highlight, and nothing else");
        for (size_t i = 0; i < drawn->highlight_count; i += 1) {
            RM_CHECK(contains_id(ids, count, drawn->highlights[i].id), "highlight %zu found", i);
        }
        rm_item_ids_free(ids);
        rm_page_free(drawn);
    }
    free(data);
}

int main(void) {
    a_tombstone_before_a_108_byte_block_is_a_tombstone();
    a_highlight_takes_no_colour_from_the_block_after_it();
    a_highlight_whose_rectangles_run_past_its_value_is_a_counted_loss();
    a_highlight_with_neither_value_nor_deletion_is_a_counted_loss();
    a_highlight_whose_value_will_not_read_is_a_counted_loss();
    every_mark_read_out_of_a_page_carries_its_own_identifier();
    an_identifier_spells_as_author_colon_counter();
    a_highlight_is_erased_and_the_rest_stays();
    a_stroke_is_erased_as_the_tablet_erases_one();
    everything_on_every_sample_erased_at_once_reads_back_empty();
    an_identifier_the_page_does_not_hold_refuses_the_whole_edit();
    an_item_already_erased_is_not_there_to_erase_again();
    nothing_asked_nothing_changed();
    a_page_that_is_not_a_page_is_refused_as_the_parser_refuses_it();
    the_marker_on_a_hidden_layer_is_found_and_the_pen_is_not();
    the_marker_on_a_hidden_layer_is_erased_and_the_layer_keeps_its_pen();
    a_layer_taken_out_of_the_tree_is_not_searched();
    a_group_nested_in_a_removed_layer_is_as_gone_as_the_layer();
    a_page_with_a_block_that_will_not_read_is_refused_not_answered_in_part();
    highlights_over_a_text_layer_are_found_with_the_strokes();
    return rm_test_report("editor");
}
