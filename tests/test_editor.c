/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The editor against pages it did not write: the sample corpus, which the
 * tablet did. An erase has to leave the page reading as the tablet would have
 * left it, and every block it was not asked about exactly as it was. And the
 * marker on a layer the user hid: not drawn, so not on the page
 * `rm_parse_page` answers with — and still on the tablet, where showing the
 * layer brings it back. An erase of every mark has to find it. */

#include <float.h>
#include <math.h>

#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

static const char* const SAMPLES[] = {
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

static uint8_t* sample(const char* name, size_t* out_size) {
  char path[1024];
  rm_test_fixture_path(path, sizeof path, name);
  return rm_test_read_file(path, out_size);
}

static rm_page* parse(const uint8_t* bytes, size_t size) {
  rm_page* page = nullptr;
  rm_error error;
  if (rm_parse_page(bytes, size, &page, &error) != RM_OK) return nullptr;
  return page;
}

static rm_page* parse_session_bytes(rm_edit_session* session) {
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(session, &size);
  return parse(bytes, size);
}

/* ------------------------------------------------------------------ */
/* The blocks of a page as they lie in the bytes                       */
/* ------------------------------------------------------------------ */

typedef struct block_view {
  uint8_t unknown, min_version, current_version, type;
  const uint8_t* body;
  size_t length;
} block_view;

static size_t blocks_of(const uint8_t* page, size_t size, block_view* out,
                        size_t capacity) {
  size_t offset = RM_HEADER_LENGTH;
  size_t count = 0;
  while (offset + 8 <= size && count < capacity) {
    size_t length = (size_t)page[offset] | (size_t)page[offset + 1] << 8 |
                    (size_t)page[offset + 2] << 16 |
                    (size_t)page[offset + 3] << 24;
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

static bool same_block(const block_view* a, const block_view* b) {
  return a->unknown == b->unknown && a->min_version == b->min_version &&
         a->current_version == b->current_version && a->type == b->type &&
         a->length == b->length && memcmp(a->body, b->body, a->length) == 0;
}

/* Every block of `edited` equal to its counterpart in `original`, save the
 * ones that were erased — and those in the shape the tablet leaves: the same
 * header, the body cut after the deleted length, and that length one. */
static void expect_only_erased(size_t erased, const uint8_t* original,
                               size_t original_size, const uint8_t* edited,
                               size_t edited_size, const char* name) {
  enum { CAPACITY = 4096 };
  static block_view before[CAPACITY], after[CAPACITY];
  size_t before_count = blocks_of(original, original_size, before, CAPACITY);
  size_t after_count = blocks_of(edited, edited_size, after, CAPACITY);
  RM_CHECK_EQ_SIZE(after_count, before_count, "%s: as many blocks as before",
                   name);
  size_t changed = 0;
  for (size_t i = 0; i < before_count && i < after_count; i += 1) {
    const block_view* old = &before[i];
    const block_view* new = &after[i];
    if (same_block(old, new)) continue;
    changed += 1;
    RM_CHECK(new->type == old->type && new->unknown == old->unknown, "%s",
             name);
    RM_CHECK(new->min_version == old->min_version &&
                 new->current_version == old->current_version,
             "%s", name);
    RM_CHECK(new->type == 0x05 || new->type == 0x03, "%s: only marks change",
             name);
    /* The preamble kept whole, the count set to one, nothing behind it. */
    RM_CHECK(new->length < old->length && new->length >= 4,
             "%s: the block shrinks", name);
    if (new->length >= 4 && new->length < old->length) {
      size_t kept = new->length - 4;
      static const uint8_t one[4] = {1, 0, 0, 0};
      static const uint8_t none[4] = {0, 0, 0, 0};
      RM_CHECK(memcmp(new->body, old->body, kept) == 0,
               "%s: the preamble is kept", name);
      RM_CHECK(memcmp(new->body + kept, one, 4) == 0, "%s: deleted length one",
               name);
      RM_CHECK(
          kept + 4 <= old->length && memcmp(old->body + kept, none, 4) == 0,
          "%s: where the living item said nought", name);
    }
  }
  RM_CHECK_EQ_SIZE(changed, erased, "%s: exactly the erased blocks changed",
                   name);
}

/* ------------------------------------------------------------------ */
/* Pages built by hand                                                 */
/* ------------------------------------------------------------------ */

static void append_block(rm_buf* out, uint8_t type, uint8_t min_version,
                         uint8_t current_version, const uint8_t* body,
                         size_t length) {
  rm_write_top_block(out, type, min_version, current_version, body, length);
}

/* A scene item's preamble: parent, item, left, right, deleted length. */
static void append_raw_preamble(rm_buf* out, uint8_t item, uint8_t deleted) {
  const uint8_t bytes[] = {0x1F, 0, 11, 0x2F, 1,       item, 0x3F, 0, 0,
                           0x4F, 0, 0,  0x54, deleted, 0,    0,    0};
  rm_buf_append(out, bytes, sizeof bytes);
}

static void start_page(rm_buf* page) {
  rm_buf_init(page);
  rm_buf_append(page, RM_HEADER, RM_HEADER_LENGTH);
}

static void append_zeros_block(rm_buf* page, uint8_t type, size_t length) {
  uint8_t* zeros = calloc(length == 0 ? 1 : length, 1);
  append_block(page, type, 0, 1, zeros, length);
  free(zeros);
}

/* A value of a highlight: type 1, colour 3, and the rectangles field as given.
 */
static void append_glyph(rm_buf* page, const uint8_t* rectangles,
                         size_t length) {
  rm_buf value = {0}, body = {0};
  const uint8_t head[] = {0x01, 0x44, 3, 0, 0, 0};
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
  const uint8_t types[] = {0x05, 0x03};
  for (size_t i = 0; i < 2; i += 1) {
    rm_buf page, body = {0};
    start_page(&page);
    append_raw_preamble(&body, 20, 1);
    uint8_t version = types[i] == 5 ? 2 : 1;
    append_block(&page, types[i], version, version, body.bytes, body.count);
    /* The page counters: stepped over by length, never counted. */
    append_zeros_block(&page, 0x0A, 108);
    rm_page* parsed = parse(page.bytes, page.count);
    RM_CHECK(parsed != nullptr, "type %u: the page parses", types[i]);
    if (parsed != nullptr) {
      RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "type %u: nothing lost",
                       types[i]);
      RM_CHECK_EQ_SIZE(parsed->stroke_count, 0, "type %u: no stroke", types[i]);
      RM_CHECK_EQ_SIZE(parsed->highlight_count, 0, "type %u: no highlight",
                       types[i]);
      rm_page_free(parsed);
    }
    rm_buf_free(&body);
    rm_buf_free(&page);
  }
}

/* A deleted length removes a highlight even with a value behind it, the way
 * the editor reads it: the page does not show what the editor cannot erase,
 * and the removal is no loss. */
static void a_deleted_highlight_with_a_value_is_not_shown(void) {
  uint8_t rectangles[6 + 32] = {0x6C, 33, 0, 0, 0, 1};
  rm_buf value = {0}, body = {0}, page;
  const uint8_t head[] = {0x01, 0x44, 3, 0, 0, 0};
  rm_buf_append(&value, head, sizeof head);
  rm_buf_append(&value, rectangles, sizeof rectangles);
  append_raw_preamble(&body, 20, 1);
  rm_write_block(&body, 6, value.bytes, value.count);
  start_page(&page);
  append_block(&page, 0x03, 1, 1, body.bytes, body.count);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr, "the page parses");
  if (parsed != nullptr) {
    RM_CHECK_EQ_SIZE(parsed->highlight_count, 0,
                     "the deleted highlight is not shown");
    RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "and is no loss");
    rm_page_free(parsed);
  }
  rm_item_id* ids = nullptr;
  size_t id_count = 0;
  RM_CHECK(rm_highlighter_items(page.bytes, page.count, &ids, &id_count,
                                nullptr) == RM_OK &&
               id_count == 0,
           "nor listed as a mark to erase");
  rm_item_ids_free(ids);
  rm_buf_free(&value);
  rm_buf_free(&body);
  rm_buf_free(&page);
}

/* A value that ends before its optional colour: the byte behind it is the
 * next block's length, and 420 spells the colour's tag, A4 01. */
static void a_highlight_takes_no_colour_from_the_block_after_it(void) {
  uint8_t rectangles[6 + 32] = {0x6C, 33, 0, 0, 0, 1};
  rm_buf page;
  start_page(&page);
  append_glyph(&page, rectangles, sizeof rectangles);
  append_zeros_block(&page, 0x0A, 420);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr, "the page parses");
  if (parsed != nullptr) {
    RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "nothing lost");
    RM_CHECK_EQ_SIZE(parsed->highlight_count, 1, "the highlight is read");
    if (parsed->highlight_count == 1) {
      RM_CHECK(!parsed->highlights[0].has_argb,
               "and takes no colour it does not carry");
      RM_CHECK(rm_item_id_equal(parsed->highlights[0].id,
                                (rm_item_id){.author = 1, .counter = 20}),
               "and carries its own identifier");
    }
    rm_page_free(parsed);
  }
  rm_buf_free(&page);
}

static void a_highlight_whose_rectangles_run_past_its_value_is_a_counted_loss(
    void) {
  /* Two rectangles declared, room for one. */
  uint8_t rectangles[6 + 32] = {0x6C, 33, 0, 0, 0, 2};
  rm_buf page;
  start_page(&page);
  append_glyph(&page, rectangles, sizeof rectangles);
  append_zeros_block(&page, 0x0A, 64);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1,
           "the highlight is counted lost");
  rm_page_free(parsed);
  rm_buf_free(&page);
}

static void a_highlight_with_neither_value_nor_deletion_is_a_counted_loss(
    void) {
  rm_buf page, body = {0};
  start_page(&page);
  append_raw_preamble(&body, 20, 0);
  append_block(&page, 0x03, 1, 1, body.bytes, body.count);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1,
           "a loss, not an erasure");
  rm_page_free(parsed);
  rm_buf_free(&body);
  rm_buf_free(&page);
}

static void a_highlight_whose_value_will_not_read_is_a_counted_loss(void) {
  /* A value tag, a length, and then nothing a highlight is made of. */
  rm_buf page, body = {0};
  start_page(&page);
  append_raw_preamble(&body, 20, 0);
  const uint8_t value[] = {0x6C, 2, 0, 0, 0, 1, 0};
  rm_buf_append(&body, value, sizeof value);
  append_block(&page, 0x03, 1, 1, body.bytes, body.count);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1,
           "the broken highlight is counted");
  rm_page_free(parsed);
  rm_buf_free(&body);
  rm_buf_free(&page);
}

static bool contains_id(const rm_item_id* ids, size_t count, rm_item_id id) {
  for (size_t i = 0; i < count; i += 1) {
    if (rm_item_id_equal(ids[i], id)) return true;
  }
  return false;
}

static void every_mark_read_out_of_a_page_carries_its_own_identifier(void) {
  for (size_t s = 0; s < SAMPLE_COUNT; s += 1) {
    size_t size = 0;
    uint8_t* bytes = sample(SAMPLES[s], &size);
    RM_CHECK(bytes != nullptr, "%s reads", SAMPLES[s]);
    rm_page* page = bytes == nullptr ? nullptr : parse(bytes, size);
    if (page != nullptr) {
      size_t total = page->stroke_count + page->highlight_count;
      rm_item_id* ids = calloc(total == 0 ? 1 : total, sizeof *ids);
      size_t distinct = 0;
      for (size_t i = 0; i < total; i += 1) {
        rm_item_id id = i < page->stroke_count
                            ? page->strokes[i].id
                            : page->highlights[i - page->stroke_count].id;
        if (!contains_id(ids, distinct, id)) ids[distinct++] = id;
      }
      RM_CHECK_EQ_SIZE(distinct, total, "%s: no two marks share one",
                       SAMPLES[s]);
      free(ids);
      rm_page_free(page);
    }
    free(bytes);
  }
}

static void an_identifier_spells_as_author_colon_counter(void) {
  rm_item_id id = {.author = 2, .counter = 1234};
  char spelled[32];
  RM_CHECK(rm_item_id_format(id, spelled, sizeof spelled) == 6,
           "six characters");
  RM_CHECK_EQ_STR(spelled, strlen(spelled), "2:1234", "the spelling");
  rm_item_id back = {0};
  RM_CHECK(rm_item_id_parse("2:1234", &back) && rm_item_id_equal(back, id),
           "reads back");
  RM_CHECK(rm_item_id_parse("255:18446744073709551615", &back) &&
               back.author == 255 && back.counter == UINT64_MAX,
           "the widest reads back");
  char widest[25];
  RM_CHECK(rm_item_id_format(back, widest, sizeof widest) == 24 &&
               strcmp(widest, "255:18446744073709551615") == 0,
           "and spells whole in 25 bytes");
  /* One leading sign is allowed on each unsigned number: a plus on anything,
   * a minus only on a nought. */
  RM_CHECK(
      rm_item_id_parse("+2:+1", &back) && back.author == 2 && back.counter == 1,
      "a plus is no part of the number");
  RM_CHECK(rm_item_id_parse("-0:-00", &back) && back.author == 0 &&
               back.counter == 0,
           "and a minus on a nought is a nought");
  const char* const refused[] = {
      "2:",   ":1",  "256:1", "2:1:3", "-1:2",
      " 2:1", "2:+", "2:+-1", "2:-1",  "2:18446744073709551616",
      "",     "21"};
  for (size_t i = 0; i < sizeof refused / sizeof *refused; i += 1) {
    RM_CHECK(!rm_item_id_parse(refused[i], &back),
             "\"%s\" is not an identifier", refused[i]);
  }
}

/* ------------------------------------------------------------------ */
/* Erasing                                                             */
/* ------------------------------------------------------------------ */

static void a_highlight_is_erased_and_the_rest_stays(void) {
  size_t size = 0;
  uint8_t* data = sample("Wikipedia_highlighted_p1.rm", &size);
  rm_page* page = data == nullptr ? nullptr : parse(data, size);
  RM_CHECK(page != nullptr && page->highlight_count > 1,
           "the sample has highlights");
  if (page != nullptr && page->highlight_count > 1) {
    rm_item_id target = page->highlights[1].id;
    uint8_t* edited = nullptr;
    size_t edited_size = 0;
    rm_error error;
    rm_status status =
        rm_erase_items(data, size, &target, 1, &edited, &edited_size, &error);
    RM_CHECK(status == RM_OK, "the erase goes through (%s: %s)",
             rm_status_string(status), error.message);
    rm_page* after = status == RM_OK ? parse(edited, edited_size) : nullptr;
    if (after != nullptr) {
      RM_CHECK_EQ_SIZE(after->highlight_count, page->highlight_count - 1,
                       "one fewer");
      size_t j = 0;
      for (size_t i = 0; i < page->highlight_count; i += 1) {
        if (rm_item_id_equal(page->highlights[i].id, target)) continue;
        RM_CHECK(j < after->highlight_count &&
                     rm_item_id_equal(after->highlights[j].id,
                                      page->highlights[i].id),
                 "the others in their order");
        j += 1;
      }
      /* A highlight taken off is a tombstone, not a loss. */
      RM_CHECK_EQ_SIZE(after->skipped_blocks, 0, "and nothing is lost");
      expect_only_erased(1, data, size, edited, edited_size,
                         "Wikipedia_highlighted_p1.rm");
      rm_page_free(after);
    }
    rm_bytes_free(edited);
  }
  rm_page_free(page);
  free(data);
}

static void a_stroke_is_erased_as_the_tablet_erases_one(void) {
  size_t size = 0;
  uint8_t* data = sample("Color_and_tool_v3.14.4.rm", &size);
  rm_page* page = data == nullptr ? nullptr : parse(data, size);
  RM_CHECK(page != nullptr && page->stroke_count > 2, "the sample has strokes");
  if (page != nullptr && page->stroke_count > 2) {
    /* One from the middle of the chain: the strokes after it hang on it. */
    rm_item_id target = page->strokes[page->stroke_count / 2].id;
    uint8_t* edited = nullptr;
    size_t edited_size = 0;
    rm_status status =
        rm_erase_items(data, size, &target, 1, &edited, &edited_size, nullptr);
    RM_CHECK(status == RM_OK, "the erase goes through (%s)",
             rm_status_string(status));
    rm_page* after = status == RM_OK ? parse(edited, edited_size) : nullptr;
    if (after != nullptr) {
      RM_CHECK_EQ_SIZE(after->stroke_count, page->stroke_count - 1,
                       "one fewer stroke");
      size_t j = 0;
      for (size_t i = 0; i < page->stroke_count; i += 1) {
        if (rm_item_id_equal(page->strokes[i].id, target)) continue;
        RM_CHECK(
            j < after->stroke_count &&
                rm_item_id_equal(after->strokes[j].id, page->strokes[i].id),
            "the others in their order");
        j += 1;
      }
      RM_CHECK_EQ_SIZE(after->highlight_count, page->highlight_count,
                       "highlights stay");
      RM_CHECK_EQ_SIZE(after->skipped_blocks, 0, "and nothing is lost");
      expect_only_erased(1, data, size, edited, edited_size,
                         "Color_and_tool_v3.14.4.rm");
      rm_page_free(after);
    }
    rm_bytes_free(edited);
  }
  rm_page_free(page);
  free(data);
}

static void everything_on_every_sample_erased_at_once_reads_back_empty(void) {
  for (size_t s = 0; s < SAMPLE_COUNT; s += 1) {
    const char* name = SAMPLES[s];
    size_t size = 0;
    uint8_t* data = sample(name, &size);
    rm_page* page = data == nullptr ? nullptr : parse(data, size);
    RM_CHECK(page != nullptr, "%s parses", name);
    size_t total =
        page == nullptr ? 0 : page->stroke_count + page->highlight_count;
    if (total == 0) {
      rm_page_free(page);
      free(data);
      continue;
    }
    rm_item_id* ids = calloc(total, sizeof *ids);
    for (size_t i = 0; i < total; i += 1) {
      ids[i] = i < page->stroke_count
                   ? page->strokes[i].id
                   : page->highlights[i - page->stroke_count].id;
    }
    uint8_t* edited = nullptr;
    size_t edited_size = 0;
    rm_error error;
    rm_status status =
        rm_erase_items(data, size, ids, total, &edited, &edited_size, &error);
    RM_CHECK(status == RM_OK, "%s: the erase goes through (%s: %s)", name,
             rm_status_string(status), error.message);
    rm_page* after = status == RM_OK ? parse(edited, edited_size) : nullptr;
    if (after != nullptr) {
      RM_CHECK_EQ_SIZE(after->stroke_count, 0, "%s: no stroke left", name);
      RM_CHECK_EQ_SIZE(after->highlight_count, 0, "%s: no highlight left",
                       name);
      RM_CHECK_EQ_SIZE(after->skipped_blocks, page->skipped_blocks,
                       "%s: no new loss", name);
      /* The typed text is not the eraser's business. */
      RM_CHECK(after->has_text == page->has_text, "%s: the text stays", name);
      if (after->has_text && page->has_text) {
        RM_CHECK_EQ_SIZE(after->text.paragraph_count,
                         page->text.paragraph_count, "%s: every paragraph",
                         name);
        for (size_t p = 0;
             p < page->text.paragraph_count && p < after->text.paragraph_count;
             p += 1) {
          RM_CHECK_EQ_STR(after->text.paragraphs[p].text.bytes,
                          after->text.paragraphs[p].text.length,
                          page->text.paragraphs[p].text.bytes, "%s: line %zu",
                          name, p);
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
  uint8_t* data = sample("Wikipedia_highlighted_p1.rm", &size);
  rm_page* page = data == nullptr ? nullptr : parse(data, size);
  RM_CHECK(page != nullptr && page->highlight_count > 0,
           "the sample has a highlight");
  if (page != nullptr && page->highlight_count > 0) {
    rm_item_id ids[] = {page->highlights[0].id,
                        {.author = 9, .counter = 999999}};
    uint8_t* edited = nullptr;
    size_t edited_size = 0;
    rm_error error;
    rm_status status =
        rm_erase_items(data, size, ids, 2, &edited, &edited_size, &error);
    RM_CHECK(status == RM_ERR_ITEMS_NOT_FOUND, "refused (%s)",
             rm_status_string(status));
    RM_CHECK(error.status == status && error.value == 1, "one missing (%ld)",
             error.value);
    RM_CHECK_EQ_STR(error.message, strlen(error.message),
                    "not on the page: 9:999999", "and named");
    RM_CHECK(edited == nullptr, "nothing is handed over");
    rm_bytes_free(edited);
  }
  rm_page_free(page);
  free(data);
}

static void an_item_already_erased_is_not_there_to_erase_again(void) {
  size_t size = 0;
  uint8_t* data = sample("Wikipedia_highlighted_p1.rm", &size);
  rm_page* page = data == nullptr ? nullptr : parse(data, size);
  if (page != nullptr && page->highlight_count > 0) {
    rm_item_id target = page->highlights[0].id;
    uint8_t *once = nullptr, *twice = nullptr;
    size_t once_size = 0, twice_size = 0;
    RM_CHECK(rm_erase_items(data, size, &target, 1, &once, &once_size,
                            nullptr) == RM_OK,
             "the first erase goes through");
    rm_error error;
    rm_status status = once == nullptr
                           ? RM_OK
                           : rm_erase_items(once, once_size, &target, 1, &twice,
                                            &twice_size, &error);
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
  uint8_t* data = sample("Color_and_tool_v3.14.4.rm", &size);
  RM_CHECK(data != nullptr, "the sample reads");
  if (data == nullptr) return;
  uint8_t* edited = nullptr;
  size_t edited_size = 0;
  RM_CHECK(rm_erase_items(data, size, nullptr, 0, &edited, &edited_size,
                          nullptr) == RM_OK,
           "an empty edit goes through");
  RM_CHECK(edited != nullptr && edited_size == size &&
               memcmp(edited, data, size) == 0,
           "and changes nothing");
  rm_bytes_free(edited);
  free(data);
}

static void a_page_that_is_not_a_page_is_refused_as_the_parser_refuses_it(
    void) {
  const char garbage[] = "not a page";
  rm_item_id id = {.author = 1, .counter = 1};
  uint8_t* edited = nullptr;
  size_t edited_size = 0;
  rm_error error;
  rm_status status = rm_erase_items((const uint8_t*)garbage, strlen(garbage),
                                    &id, 1, &edited, &edited_size, &error);
  /* Shorter than a header: the stream ends inside the first field, as the
   * parser says of it. */
  RM_CHECK(status == RM_ERR_UNEXPECTED_EOF,
           "refused as a stream cut short (%s)", rm_status_string(status));
  RM_CHECK(edited == nullptr, "nothing is handed over");
}

/* ------------------------------------------------------------------ */
/* The marker on hidden layers                                         */
/* ------------------------------------------------------------------ */

/* A layer node, hidden or shown, and its link into the root. */
static void append_layer(rm_buf* page, rm_crdt_id node, bool hidden,
                         rm_crdt_id link) {
  rm_buf state = {0}, group = {0}, value = {0};
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
static void append_nested_group(rm_buf* page, rm_crdt_id parent,
                                rm_crdt_id link, rm_crdt_id node) {
  rm_buf group = {0}, value = {0};
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
static void append_stroke(rm_buf* page, rm_crdt_id id, rm_crdt_id parent,
                          rm_pen_tool tool) {
  rm_buf value = {0}, points = {0}, line = {0};
  rm_buf_append_byte(&value, 0x03);
  rm_write_tag(&value, 1, RM_TAG_BYTE4);
  rm_write_u32(&value, (uint32_t)tool);
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_write_u32(&value, 0);
  rm_write_tag(&value, 3, RM_TAG_BYTE8);
  rm_write_f64(&value, 1);
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_f32(&value, 0);
  const float xs[] = {0, 100};
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
static void append_removal(rm_buf* page, rm_crdt_id node) {
  rm_buf record = {0};
  rm_write_id(&record, 1, rm_crdt_make(1, 95));
  rm_write_id(&record, 2, node);
  rm_write_tag(&record, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&record, 0);
  append_block(page, 1, 1, 1, record.bytes, record.count);
  rm_buf_free(&record);
}

/* A node put back into the tree under `parent`: a fresh record whose tag 2
 * names the node, as the tablet writes when it moves typed text and the
 * handwriting anchored to it. */
static void append_reinsertion(rm_buf* page, rm_crdt_id node,
                               rm_crdt_id parent) {
  rm_buf record = {0}, place = {0};
  rm_write_id(&record, 1, rm_crdt_make(1, 96));
  rm_write_id(&record, 2, node);
  rm_write_tag(&record, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&record, 1);
  rm_write_id(&place, 1, parent);
  rm_write_block(&record, 4, place.bytes, place.count);
  append_block(page, 1, 1, 1, record.bytes, record.count);
  rm_buf_free(&record);
  rm_buf_free(&place);
}

static const rm_crdt_id SHOWN = {.counter = 11, .author = 1};
static const rm_crdt_id HIDDEN = {.counter = 21, .author = 1};

static void append_two_layers(rm_buf* page, bool second_hidden) {
  append_layer(page, SHOWN, false, rm_crdt_make(1, 12));
  append_layer(page, HIDDEN, second_hidden, rm_crdt_make(1, 22));
}

static void append_three_strokes(rm_buf* page) {
  append_stroke(page, rm_crdt_make(1, 30), SHOWN, RM_TOOL_HIGHLIGHTER_2);
  append_stroke(page, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_HIGHLIGHTER_2);
  append_stroke(page, rm_crdt_make(1, 32), HIDDEN, RM_TOOL_FINELINER_2);
}

static size_t highlighter_items(const uint8_t* bytes, size_t size,
                                rm_item_id** out) {
  size_t count = 0;
  *out = nullptr;
  rm_status status = rm_highlighter_items(bytes, size, out, &count, nullptr);
  RM_CHECK(status == RM_OK, "the marks are found (%s)",
           rm_status_string(status));
  return count;
}

static void the_marker_on_a_hidden_layer_is_found_and_the_pen_is_not(void) {
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, true);
  append_three_strokes(&page);

  rm_page* drawn = parse(page.bytes, page.count);
  RM_CHECK(drawn != nullptr, "the page parses");
  if (drawn != nullptr) {
    /* Drawn: the shown layer's marker alone. */
    RM_CHECK(drawn->stroke_count == 1 &&
                 rm_item_id_equal(drawn->strokes[0].id, (rm_item_id){1, 30}),
             "only the shown marker is drawn");
    RM_CHECK_EQ_SIZE(drawn->skipped_blocks, 0, "nothing lost");
    rm_page_free(drawn);
  }
  rm_item_id* ids;
  size_t count = highlighter_items(page.bytes, page.count, &ids);
  RM_CHECK(count == 2 && contains_id(ids, count, (rm_item_id){1, 30}) &&
               contains_id(ids, count, (rm_item_id){1, 31}),
           "both markers, and not the pen (%zu found)", count);
  rm_item_ids_free(ids);
  rm_buf_free(&page);
}

static void the_marker_on_a_hidden_layer_is_erased_and_the_layer_keeps_its_pen(
    void) {
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, true);
  size_t layers_end = page.count;
  append_three_strokes(&page);

  rm_item_id* marks;
  size_t mark_count = highlighter_items(page.bytes, page.count, &marks);
  uint8_t* edited = nullptr;
  size_t edited_size = 0;
  rm_status status = rm_erase_items(page.bytes, page.count, marks, mark_count,
                                    &edited, &edited_size, nullptr);
  RM_CHECK(status == RM_OK, "the erase goes through (%s)",
           rm_status_string(status));
  rm_item_ids_free(marks);
  if (status != RM_OK) {
    rm_buf_free(&page);
    return;
  }

  rm_item_id* left;
  RM_CHECK_EQ_SIZE(highlighter_items(edited, edited_size, &left), 0,
                   "no marker is left");
  rm_item_ids_free(left);
  rm_page* after = parse(edited, edited_size);
  RM_CHECK(after != nullptr && after->stroke_count == 0 &&
               after->skipped_blocks == 0,
           "nothing drawn and nothing lost");
  rm_page_free(after);

  /* Shown again, the layer holds the pen and nothing of the marker. */
  rm_buf shown_again;
  start_page(&shown_again);
  append_two_layers(&shown_again, false);
  rm_buf_append(&shown_again, edited + layers_end, edited_size - layers_end);
  rm_page* again = parse(shown_again.bytes, shown_again.count);
  RM_CHECK(again != nullptr && again->stroke_count == 1 &&
               rm_item_id_equal(again->strokes[0].id, (rm_item_id){1, 32}),
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
  rm_item_id* ids;
  RM_CHECK_EQ_SIZE(highlighter_items(page.bytes, page.count, &ids), 0,
                   "nothing brings it back");
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
  rm_item_id* ids;
  RM_CHECK_EQ_SIZE(highlighter_items(page.bytes, page.count, &ids), 0,
                   "the nested goes too");
  rm_item_ids_free(ids);
  rm_buf_free(&page);
}

/* Whether the page draws exactly the one stroke `id`. */
static bool draws_only(const rm_buf* page, rm_item_id id) {
  rm_page* drawn = parse(page->bytes, page->count);
  bool only = drawn != nullptr && drawn->stroke_count == 1 &&
              rm_item_id_equal(drawn->strokes[0].id, id);
  rm_page_free(drawn);
  return only;
}

/* Whether the highlighter search finds exactly the one mark `id`. */
static bool finds_only(const rm_buf* page, rm_item_id id) {
  rm_item_id* ids;
  size_t count = highlighter_items(page->bytes, page->count, &ids);
  bool only = count == 1 && rm_item_id_equal(ids[0], id);
  rm_item_ids_free(ids);
  return only;
}

static void a_layer_taken_out_and_put_back_is_drawn_and_searched_again(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, HIDDEN, false, rm_crdt_make(1, 22));
  append_removal(&page, HIDDEN);
  append_reinsertion(&page, HIDDEN, rm_crdt_make(0, 1));
  append_stroke(&page, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_HIGHLIGHTER_2);
  rm_page* drawn = parse(page.bytes, page.count);
  RM_CHECK(drawn != nullptr && drawn->skipped_blocks == 0,
           "the page parses and loses nothing");
  rm_page_free(drawn);
  RM_CHECK(draws_only(&page, (rm_item_id){1, 31}), "the layer is drawn again");
  RM_CHECK(finds_only(&page, (rm_item_id){1, 31}),
           "the layer is searched again");
  rm_buf_free(&page);
}

static void a_group_moved_under_a_layer_later_removed_goes_with_it(void) {
  const rm_crdt_id group = rm_crdt_make(1, 41);
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, false);
  append_nested_group(&page, SHOWN, rm_crdt_make(1, 42), group);
  append_reinsertion(&page, group, HIDDEN);
  append_removal(&page, HIDDEN);
  append_stroke(&page, rm_crdt_make(1, 31), group, RM_TOOL_HIGHLIGHTER_2);
  rm_page* drawn = parse(page.bytes, page.count);
  RM_CHECK(drawn != nullptr && drawn->stroke_count == 0,
           "the moved group is not drawn");
  rm_page_free(drawn);
  rm_item_id* ids;
  RM_CHECK_EQ_SIZE(highlighter_items(page.bytes, page.count, &ids), 0,
                   "the moved group is not searched");
  rm_item_ids_free(ids);
  rm_buf_free(&page);
}

static void a_group_moved_out_of_a_layer_later_removed_stays(void) {
  const rm_crdt_id group = rm_crdt_make(1, 41);
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, false);
  append_nested_group(&page, HIDDEN, rm_crdt_make(1, 42), group);
  append_reinsertion(&page, group, SHOWN);
  append_removal(&page, HIDDEN);
  append_stroke(&page, rm_crdt_make(1, 31), group, RM_TOOL_HIGHLIGHTER_2);
  RM_CHECK(draws_only(&page, (rm_item_id){1, 31}), "the moved group is drawn");
  RM_CHECK(finds_only(&page, (rm_item_id){1, 31}),
           "the moved group is searched");
  rm_buf_free(&page);
}

static void a_group_moved_and_linked_under_its_new_layer_is_drawn_once(void) {
  const rm_crdt_id group = rm_crdt_make(1, 41);
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, true);
  append_nested_group(&page, HIDDEN, rm_crdt_make(1, 42), group);
  append_nested_group(&page, SHOWN, rm_crdt_make(1, 43), group);
  append_reinsertion(&page, group, SHOWN);
  append_stroke(&page, rm_crdt_make(1, 31), group, RM_TOOL_FINELINER_2);
  /* Under the shown layer now: the stale link into the hidden one neither
   * hides it nor draws it a second time. */
  RM_CHECK(draws_only(&page, (rm_item_id){1, 31}),
           "drawn once, from the shown layer");
  rm_buf_free(&page);
}

static void a_tree_record_whose_parent_runs_past_its_block_does_not_read(void) {
  rm_buf page, record = {0};
  start_page(&page);
  append_layer(&page, HIDDEN, false, rm_crdt_make(1, 22));
  rm_write_id(&record, 1, rm_crdt_make(1, 96));
  rm_write_id(&record, 2, HIDDEN);
  rm_write_tag(&record, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&record, 1);
  rm_write_tag(&record, 4, RM_TAG_LENGTH4);
  append_block(&page, 1, 1, 1, record.bytes, record.count);
  append_stroke(&page, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_FINELINER_2);
  rm_page* drawn = parse(page.bytes, page.count);
  RM_CHECK(drawn != nullptr && drawn->skipped_blocks == 1,
           "the record is a block that does not read");
  rm_page_free(drawn);
  RM_CHECK(draws_only(&page, (rm_item_id){1, 31}), "the layer is still drawn");
  rm_buf_free(&record);
  rm_buf_free(&page);
}

static void
a_page_with_a_block_that_will_not_read_is_refused_not_answered_in_part(void) {
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

  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 1,
           "the page loses one block");
  rm_page_free(parsed);

  rm_item_id* ids = nullptr;
  size_t count = 0;
  rm_error error;
  rm_status status =
      rm_highlighter_items(page.bytes, page.count, &ids, &count, &error);
  RM_CHECK(status == RM_ERR_INCOMPLETE_PAGE, "refused (%s)",
           rm_status_string(status));
  RM_CHECK(error.status == status && error.value == 1, "one block lost (%ld)",
           error.value);
  RM_CHECK(ids == nullptr && count == 0, "nothing is handed over");
  rm_item_ids_free(ids);
  rm_buf_free(&broken);
  rm_buf_free(&page);
}

static void highlights_over_a_text_layer_are_found_with_the_strokes(void) {
  size_t size = 0;
  uint8_t* data = sample("Wikipedia_highlighted_p1.rm", &size);
  rm_page* drawn = data == nullptr ? nullptr : parse(data, size);
  RM_CHECK(drawn != nullptr, "the sample parses");
  if (drawn != nullptr) {
    rm_item_id* ids;
    size_t count = highlighter_items(data, size, &ids);
    RM_CHECK_EQ_SIZE(count, drawn->highlight_count,
                     "every highlight, and nothing else");
    for (size_t i = 0; i < drawn->highlight_count; i += 1) {
      RM_CHECK(contains_id(ids, count, drawn->highlights[i].id),
               "highlight %zu found", i);
    }
    rm_item_ids_free(ids);
    rm_page_free(drawn);
  }
  free(data);
}

/* Inspect the serialized registers, which the public page model omits. */
static bool line_has_move_only(const block_view* block, rm_item_id expected) {
  rm_reader body = {.data = block->body, .size = block->length}, value, points;
  rm_item_preamble it;
  if (rm_parse_item_preamble(&body, &it) != RM_OK || it.deleted ||
      !rm_match_tag(&body, 6, RM_TAG_LENGTH4) ||
      rm_read_subblock(&body, &value) != RM_OK)
    return false;
  /* Kind and the fixed-size fields for tool, color, thickness and starting
   * length. */
  if (rm_reader_remaining(&value) < 25) return false;
  value.offset += 25;
  rm_crdt_id move;
  return rm_match_tag(&value, 5, RM_TAG_LENGTH4) &&
         rm_read_subblock(&value, &points) == RM_OK &&
         rm_read_crdt(&value, 7, &move) == RM_OK &&
         rm_crdt_equal(move, rm_crdt_of(expected)) &&
         !rm_reader_remaining(&value);
}

static bool active_layer_is(const uint8_t* bytes, size_t size,
                            rm_item_id expected) {
  block_view blocks[128];
  size_t n = blocks_of(bytes, size, blocks, 128);
  for (size_t i = 0; i < n; i++) {
    if (blocks[i].type != 13) continue;
    rm_reader r = {.data = blocks[i].body, .size = blocks[i].length}, field;
    rm_crdt_id node;
    return rm_match_tag(&r, 1, RM_TAG_LENGTH4) &&
           rm_read_subblock(&r, &field) == RM_OK &&
           rm_skip_crdt(&field, 1) == RM_OK &&
           rm_read_crdt(&field, 2, &node) == RM_OK &&
           rm_crdt_equal(node, rm_crdt_of(expected));
  }
  return false;
}

static void edit_revisions(void) {
  rm_page_draft draft = {
      .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
  uint8_t* initial = nullptr;
  size_t length = 0;
  RM_CHECK(rm_write_page(&draft, &initial, &length, nullptr) == RM_OK, "base");
  rm_edit_session* session = nullptr;
  RM_CHECK(rm_edit_open(initial, length, &session, nullptr) == RM_OK, "open");
  if (!session) {
    rm_bytes_free(initial);
    return;
  }
  rm_item_id node = {1, 11}, id = {1, 100000}, none = {0};
  rm_draft_point point = rm_draft_point_make(3, 4);
  rm_draft_stroke stroke = rm_draft_stroke_make(
      RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHTER_GRAY, &point, 1);
  RM_CHECK(rm_edit_insert_stroke(session, node, id, none, none, &stroke,
                                 nullptr) == RM_OK,
           "insert");
  RM_CHECK(rm_edit_insert_stroke(session, node, id, none, none, &stroke,
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
           "id collision");
  size_t size = 0;
  const uint8_t* at = rm_edit_bytes(session, &size);
  rm_page* page = parse(at, size);
  RM_CHECK(page && page->stroke_count == 1 &&
               rm_item_id_equal(page->strokes[0].id, id),
           "inserted id");
  rm_page_free(page);
  RM_CHECK(rm_edit_set_node_visible(session, node, (rm_item_id){1, 100001},
                                    false, nullptr) == RM_OK,
           "hide");
  at = rm_edit_bytes(session, &size);
  page = parse(at, size);
  RM_CHECK(page && !page->stroke_count, "hidden");
  rm_page_free(page);
  RM_CHECK(
      rm_edit_undo(session) && rm_edit_redo(session) && rm_edit_undo(session),
      "navigation");
  RM_CHECK(rm_edit_set_node_name(session, node, (rm_item_id){1, 100002},
                                 "Layer B", nullptr) == RM_OK,
           "rename");
  RM_CHECK(!rm_edit_redo(session), "divergent edit");
  RM_CHECK(rm_edit_erase(session, &id, 1, nullptr) == RM_OK, "erase");
  RM_CHECK(
      rm_edit_undo(session) && rm_edit_undo(session) && rm_edit_undo(session),
      "restore original");
  at = rm_edit_bytes(session, &size);
  RM_CHECK(size == length && !memcmp(initial, at, size), "exact original");
  RM_CHECK(rm_edit_replace_text(session, 0, 0, "Hello λ\nworld",
                                (rm_item_id){1, 200000}, nullptr) == RM_OK,
           "new root");
  RM_CHECK(rm_edit_replace_text(session, 2, 3, "y", (rm_item_id){1, 200020},
                                nullptr) == RM_OK,
           "replace inside a run");
  at = rm_edit_bytes(session, &size);
  page = parse(at, size);
  RM_CHECK(page && page->has_text && page->text.paragraph_count == 2 &&
               page->text.paragraphs[0].text.length == 6 &&
               !memcmp(page->text.paragraphs[0].text.bytes, "Hey λ", 6),
           "edited text");
  rm_page_free(page);
  RM_CHECK(rm_edit_replace_text(session, 3, 4, "!", (rm_item_id){1, 200030},
                                nullptr) == RM_OK,
           "edit across newline");
  RM_CHECK(rm_edit_undo(session), "undo a text edit");
  rm_item_id added = {1, 300000};
  RM_CHECK(rm_edit_add_layer(session, 0, "Overlay", added, nullptr) == RM_OK,
           "add at front");
  rm_edit_layer* layers = nullptr;
  size_t layer_count = 0;
  RM_CHECK(rm_edit_layers(session, &layers, &layer_count, nullptr) == RM_OK &&
               layer_count == 2 && rm_item_id_equal(layers[0].node, added),
           "new drawing order");
  rm_edit_layers_free(layers);
  RM_CHECK(rm_edit_move_layer(session, added, 1, (rm_item_id){1, 300010},
                              nullptr) == RM_OK,
           "move to end");
  layers = nullptr;
  RM_CHECK(rm_edit_layers(session, &layers, &layer_count, nullptr) == RM_OK &&
               layer_count == 2 && rm_item_id_equal(layers[1].node, added),
           "moved order");
  rm_edit_layers_free(layers);
  rm_draft_point samples[] = {rm_draft_point_make(1, 2),
                              rm_draft_point_make(3, 4),
                              rm_draft_point_make(5, 6)};
  stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, samples, 3);
  RM_CHECK(rm_edit_insert_stroke(session, added, id, none, none, &stroke,
                                 nullptr) == RM_OK,
           "add a line to overlay");
  rm_item_id moved = {1, 300020};
  RM_CHECK(rm_edit_transform_stroke(session, id, moved,
                                    (rm_edit_transform){1, 0, 0, 1, 5, 9},
                                    nullptr) == RM_OK,
           "translate");
  at = rm_edit_bytes(session, &size);
  page = parse(at, size);
  RM_CHECK(page && page->stroke_count == 1 &&
               page->strokes[0].points[0].x == 6 &&
               page->strokes[0].points[0].y == 11,
           "translated points");
  rm_page_free(page);
  RM_CHECK(rm_edit_cut_stroke(session, moved, (rm_item_id){1, 300030}, 1, 1,
                              nullptr) == RM_OK,
           "cut middle");
  at = rm_edit_bytes(session, &size);
  page = parse(at, size);
  RM_CHECK(page && page->stroke_count == 2 &&
               page->strokes[0].point_count == 1 &&
               page->strokes[1].point_count == 1,
           "surviving pieces");
  rm_page_free(page);
  block_view blocks[128];
  size_t n = blocks_of(at, size, blocks, 128), kept_moves = 0;
  for (size_t i = 0; i < n; i++)
    if (blocks[i].type == 5 && line_has_move_only(&blocks[i], id)) kept_moves++;
  RM_CHECK(kept_moves == 2, "both pieces keep move id and absent timestamp");
  rm_item_id pieces[] = {{1, 300030}, {1, 300031}};
  const rm_edit_transform translate = {1, 0, 0, 1, -2, 7};
  rm_item_id invalid[] = {pieces[0], {1, 9999999}};
  RM_CHECK(
      rm_edit_transform_strokes(session, invalid, 2, (rm_item_id){1, 400000},
                                translate, nullptr) == RM_ERR_ITEMS_NOT_FOUND,
      "reject entire selection");
  size_t unchanged;
  RM_CHECK(rm_edit_bytes(session, &unchanged) == at && unchanged == size,
           "failed selection preserves revision");
  RM_CHECK(
      rm_edit_transform_strokes(session, pieces, 2, (rm_item_id){1, 400000},
                                translate, nullptr) == RM_OK,
      "translate both pieces");
  RM_CHECK(rm_edit_undo(session) && rm_edit_bytes(session, &unchanged) == at,
           "one undo restores selection");
  RM_CHECK(rm_edit_delete_layer(session, added, (rm_item_id){1, 300040},
                                nullptr) == RM_OK,
           "remove overlay");
  at = rm_edit_bytes(session, &size);
  page = parse(at, size);
  RM_CHECK(page && page->stroke_count == 0, "removed layer invisible");
  rm_page_free(page);
  rm_item_id top = {1, 500000}, middle = {1, 500010};
  RM_CHECK(rm_edit_add_layer(session, 1, "Top", top, nullptr) == RM_OK,
           "add top");
  RM_CHECK(rm_edit_add_layer(session, 1, "Middle", middle, nullptr) == RM_OK,
           "add active middle");
  RM_CHECK(rm_edit_delete_layer(session, middle, (rm_item_id){1, 500020},
                                nullptr) == RM_OK,
           "delete active middle");
  at = rm_edit_bytes(session, &size);
  RM_CHECK(active_layer_is(at, size, top),
           "activate successor in deleted slot");
  RM_CHECK(rm_edit_delete_layer(session, top, (rm_item_id){1, 500030},
                                nullptr) == RM_OK,
           "delete active top");
  at = rm_edit_bytes(session, &size);
  RM_CHECK(active_layer_is(at, size, node), "activate predecessor at end");
  rm_edit_close(session);
  rm_bytes_free(initial);
}

static rm_edit_session* open_page(const rm_buf* page) {
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(page->bytes, page->count, &s, nullptr) == RM_OK,
           "open test page");
  return s;
}

static void inserts_into_layer_without_metadata(void) {
  rm_buf page;
  start_page(&page);
  append_nested_group(&page, rm_crdt_make(0, 1), rm_crdt_make(1, 12), SHOWN);
  rm_edit_session* session = open_page(&page);
  rm_edit_layer* layers = nullptr;
  size_t count = 0;
  RM_CHECK(rm_edit_layers(session, &layers, &count, nullptr) == RM_OK &&
               count == 1 &&
               rm_item_id_equal(layers[0].node, rm_item_id_of(SHOWN)),
           "group link exposes a layer while metadata is pending");
  rm_item_id none = {0}, node = rm_item_id_of(SHOWN);
  rm_item_id first = {1, 500000};
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
  RM_CHECK(rm_edit_replace_text(session, 0, 0, "abc", first, nullptr) == RM_OK,
           "occupy three consecutive scalar IDs");
  rm_error error;
  RM_CHECK(
      rm_edit_insert_stroke(session, node, (rm_item_id){1, 500001}, none, none,
                            &stroke, &error) == RM_ERR_INVALID_ARGUMENT &&
          error.status == RM_ERR_INVALID_ARGUMENT,
      "stroke ID cannot reuse a scalar inside a text run");
  RM_CHECK(rm_edit_insert_stroke(session, node, node, none, none, &stroke,
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
           "cannot reuse a metadata-less layer ID");
  rm_item_id id = {1, 500010};
  RM_CHECK(
      rm_edit_insert_stroke(session, (rm_item_id){1, 600000}, id, none, none,
                            &stroke, nullptr) == RM_ERR_ITEMS_NOT_FOUND,
      "unknown parent still rejected");
  RM_CHECK(rm_edit_insert_stroke(session, node, id, none, none, &stroke,
                                 nullptr) == RM_OK,
           "insert into the enumerated layer");
  size_t length;
  const uint8_t* bytes = rm_edit_bytes(session, &length);
  rm_page* result = parse(bytes, length);
  RM_CHECK(result && result->stroke_count == 1 &&
               rm_item_id_equal(result->strokes[0].id, id),
           "inserted stroke visible");
  rm_page_free(result);
  rm_edit_layers_free(layers);
  rm_edit_close(session);
  rm_buf_free(&page);
}

static void stroke_selection_and_reservations(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  append_stroke(&page, rm_crdt_make(1, 90), SHOWN, RM_TOOL_FINELINER_2);
  rm_edit_session* s = open_page(&page);
  rm_item_id id = {1, 90}, fresh = {1, 200}, selection[] = {id, fresh};
  rm_edit_transform m = {1, 0, 0, 1, 1, 0};
  RM_CHECK(rm_edit_erase(s, &id, 1, nullptr) == RM_OK && rm_edit_undo(s),
           "seed redo");
  size_t size;
  const uint8_t* before = rm_edit_bytes(s, &size);
  RM_CHECK(rm_edit_transform_strokes(s, selection, 2, fresh, m, nullptr) ==
               RM_ERR_ITEMS_NOT_FOUND,
           "original selection");
  selection[1] = id;
  rm_error error;
  RM_CHECK(rm_edit_transform_strokes(s, selection, 2, fresh, m, &error) ==
                   RM_ERR_INVALID_ARGUMENT &&
               error.status == RM_ERR_INVALID_ARGUMENT,
           "duplicate stroke IDs are invalid arguments");
  RM_CHECK(rm_edit_bytes(s, &size) == before && rm_edit_redo(s),
           "failed selection keeps history");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(rm_edit_transform_stroke(s, id, (rm_item_id){1, UINT64_MAX}, m,
                                    nullptr) == RM_OK,
           "final transform ID");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(
      rm_edit_cut_stroke(s, id, (rm_item_id){1, 89}, 0, 1, nullptr) == RM_OK,
      "one surviving piece");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(rm_edit_cut_stroke(s, id, (rm_item_id){0}, 0, 2, nullptr) == RM_OK,
           "cut with no surviving piece needs no ID");
  rm_edit_close(s);
  append_stroke(&page, rm_crdt_make(1, 91), SHOWN, RM_TOOL_FINELINER_2);
  s = open_page(&page);
  rm_item_id both[] = {id, {1, 91}};
  RM_CHECK(rm_edit_transform_strokes(s, both, 2, (rm_item_id){1, UINT64_MAX}, m,
                                     nullptr) == RM_ERR_INVALID_ARGUMENT,
           "batch overflows");
  RM_CHECK(
      rm_edit_transform_strokes(s, both, 2, (rm_item_id){1, UINT64_MAX - 1}, m,
                                nullptr) == RM_OK,
      "batch ends on final ID");
  rm_edit_close(s);
  block_view blocks[8];
  size_t n = blocks_of(page.bytes, page.count, blocks, 8);
  page.bytes[blocks[n - 2].body - page.bytes - 2] = 3;
  s = open_page(&page);
  RM_CHECK(rm_edit_transform_stroke(s, id, fresh, m, nullptr) == RM_OK,
           "compatible extended line");
  rm_edit_close(s);
  rm_buf_free(&page);
}

static void deletion_preserves_live_active_layer(void) {
  rm_buf page, info = {0}, field = {0};
  start_page(&page);
  append_two_layers(&page, false);
  rm_write_id(&field, 1, rm_crdt_make(1, 40));
  rm_write_id(&field, 2, rm_crdt_make(1, 999));
  rm_write_block(&info, 1, field.bytes, field.count);
  append_block(&page, 13, 0, 1, info.bytes, info.count);
  rm_edit_session* s = open_page(&page);
  RM_CHECK(rm_edit_delete_layer(s, rm_item_id_of(SHOWN), (rm_item_id){1, 500},
                                nullptr) == RM_ERR_UNVERIFIED,
           "refuse dangling active layer");
  rm_edit_close(s);
  rm_buf_free(&page);
  rm_buf_free(&info);
  rm_buf_free(&field);
}
static void layer_links_and_registers(void) {
  rm_buf page;
  start_page(&page);
  rm_item_id node = rm_item_id_of(SHOWN), other = {1, 300}, none = {0};
  append_layer(&page, SHOWN, true, rm_crdt_make(1, 12));
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 13));
  rm_edit_session* s = open_page(&page);
  size_t size;
  const uint8_t* before = rm_edit_bytes(s, &size);
  RM_CHECK(
      rm_edit_delete_layer(s, node, (rm_item_id){1, 301}, nullptr) != RM_OK &&
          rm_edit_bytes(s, &size) == before,
      "duplicate links cannot delete sole node");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(rm_edit_set_node_visible(s, node, (rm_item_id){2, 90}, true,
                                    nullptr) == RM_OK,
           "historical visibility author tie");
  RM_CHECK(rm_edit_set_node_visible(s, node, (rm_item_id){2, 90}, false,
                                    nullptr) == RM_ERR_INVALID_ARGUMENT &&
               rm_edit_set_node_visible(s, node, (rm_item_id){3, 89}, false,
                                        nullptr) == RM_ERR_INVALID_ARGUMENT,
           "identical and older stamps");
  RM_CHECK(rm_edit_set_node_visible(s, node, (rm_item_id){1, 90}, false,
                                    nullptr) == RM_ERR_INVALID_ARGUMENT,
           "lower author");
  rm_edit_close(s);
  rm_buf_free(&page);
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  s = open_page(&page);
  RM_CHECK(rm_edit_add_layer(s, 0, "collision", (rm_item_id){1, 10}, nullptr) ==
               RM_ERR_INVALID_ARGUMENT,
           "layer reservation overlaps link");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(rm_edit_add_layer(s, 0, "new", other, nullptr) == RM_OK,
           "add layer");
  RM_CHECK(rm_edit_add_layer(s, 1, "equal counter", (rm_item_id){2, 300},
                             nullptr) == RM_OK &&
               rm_edit_undo(s),
           "active register author tie");
  RM_CHECK(
      rm_edit_move_layer(s, node, 0, (rm_item_id){1, 100}, nullptr) == RM_OK,
      "move before larger ID");
  rm_edit_layer* layers = nullptr;
  size_t count = 0;
  RM_CHECK(rm_edit_layers(s, &layers, &count, nullptr) == RM_OK && count == 2 &&
               rm_item_id_equal(layers[0].node, node),
           "right layer neighbour");
  rm_edit_layers_free(layers);
  RM_CHECK(rm_edit_move_layer(s, node, 1, (rm_item_id){1, UINT64_MAX},
                              nullptr) == RM_OK,
           "final move ID");
  RM_CHECK(rm_edit_delete_layer(s, other, (rm_item_id){2, UINT64_MAX - 1},
                                nullptr) == RM_OK,
           "final two delete IDs");
  rm_edit_close(s);
  append_removal(&page, SHOWN);
  append_layer(&page, HIDDEN, false, rm_crdt_make(1, 23));
  append_reinsertion(&page, HIDDEN, SHOWN);
  s = open_page(&page);
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(HIDDEN), other, "gone",
                                 nullptr) == RM_ERR_ITEMS_NOT_FOUND,
           "removed ancestor");
  rm_draft_point pt = rm_draft_point_make(1, 2);
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
  RM_CHECK(rm_edit_set_node_name(s, node, other, "gone", nullptr) ==
               RM_ERR_ITEMS_NOT_FOUND,
           "removed node name");
  RM_CHECK(rm_edit_insert_stroke(s, node, other, none, none, &stroke,
                                 nullptr) == RM_ERR_ITEMS_NOT_FOUND,
           "removed parent");
  rm_edit_close(s);
  rm_buf_free(&page);
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  append_stroke(&page, rm_crdt_make(1, 800), SHOWN, RM_TOOL_FINELINER_2);
  s = open_page(&page);
  rm_item_id tail = {1, 800};
  RM_CHECK(rm_edit_insert_stroke(s, node, other, none, tail, &stroke,
                                 nullptr) == RM_OK,
           "insert before larger ID");
  const uint8_t* data = rm_edit_bytes(s, &size);
  rm_page* p = parse(data, size);
  RM_CHECK(
      p && p->stroke_count == 2 && rm_item_id_equal(p->strokes[0].id, other),
      "right stroke neighbour");
  rm_page_free(p);
  RM_CHECK(rm_edit_insert_stroke(s, node, (rm_item_id){1, 900}, tail, other,
                                 &stroke, nullptr) == RM_ERR_INVALID_ARGUMENT,
           "reversed neighbours");
  RM_CHECK(
      rm_edit_erase(s, &tail, 1, nullptr) == RM_OK &&
          rm_edit_insert_stroke(s, node, (rm_item_id){1, 901}, tail, none,
                                &stroke, nullptr) == RM_ERR_ITEMS_NOT_FOUND,
      "dead neighbour");
  rm_edit_close(s);
  rm_buf_free(&page);

  /* Low-counter MOVE_LAYER keeps 0x0D byte-identical (R57) & stale-stamp
   * ADD_LAYER/DELETE_LAYER refusal when logical.present (R8-1). */
  {
    rm_buf b, info = {0}, lref = {0};
    start_page(&b);
    append_two_layers(&b, false);
    rm_write_id(&lref, 1, rm_crdt_make(1, 500));
    rm_write_id(&lref, 2, SHOWN);
    rm_write_block(&info, 1, lref.bytes, lref.count);
    append_block(&b, 13, 0, 1, info.bytes, info.count);
    rm_edit_session* se = open_page(&b);
    size_t pre_sz = 0;
    const uint8_t* pre_b = rm_edit_bytes(se, &pre_sz);
    uint8_t* pre_0d = nullptr;
    size_t pre_0d_len = 0;
    rm_reader sr = {.data = pre_b, .size = pre_sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&sr) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&sr, &hdr) != RM_OK) break;
      if (hdr.type == 13) {
        pre_0d_len = hdr.end - hdr.start;
        pre_0d = malloc(pre_0d_len);
        memcpy(pre_0d, pre_b + hdr.start, pre_0d_len);
        break;
      }
      sr.offset = hdr.end;
    }
    RM_CHECK(rm_edit_move_layer(se, rm_item_id_of(SHOWN), 1,
                                (rm_item_id){1, 200}, nullptr) == RM_OK,
             "low-counter move_layer succeeds (R57)");
    size_t post_sz = 0;
    const uint8_t* post_b = rm_edit_bytes(se, &post_sz);
    bool kept_0d = false;
    rm_reader pr = {
        .data = post_b, .size = post_sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&pr) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&pr, &hdr) != RM_OK) break;
      if (hdr.type == 13 && pre_0d != nullptr &&
          hdr.end - hdr.start == pre_0d_len &&
          memcmp(post_b + hdr.start, pre_0d, pre_0d_len) == 0) {
        kept_0d = true;
      }
      pr.offset = hdr.end;
    }
    free(pre_0d);
    RM_CHECK(kept_0d, "MOVE_LAYER preserves 0x0D byte-for-byte (R57)");
    RM_CHECK(rm_edit_add_layer(se, 0, "Stale", (rm_item_id){1, 400}, nullptr) ==
                 RM_ERR_INVALID_ARGUMENT,
             "stale ADD_LAYER refused against 0x0D stamp (R8-1)");
    RM_CHECK(
        rm_edit_delete_layer(se, rm_item_id_of(SHOWN), (rm_item_id){1, 401},
                             nullptr) == RM_ERR_INVALID_ARGUMENT,
        "stale DELETE_LAYER refused against 0x0D stamp (R8-1)");
    rm_edit_close(se);
    rm_buf_free(&lref);
    rm_buf_free(&info);
    rm_buf_free(&b);
  }

  /* MOVE_LAYER with tombstoned 0x04 sharing old_link in BOTH block orders
   * (R223). */
  for (int tomb_first = 0; tomb_first < 2; ++tomb_first) {
    rm_buf b, n1 = {0}, n2 = {0}, tg = {0}, g1 = {0}, g1v = {0}, g2 = {0},
              g2v = {0};
    start_page(&b);
    rm_write_id(&n1, 1, SHOWN);
    append_block(&b, 2, 1, 1, n1.bytes, n1.count);
    rm_write_id(&n2, 1, HIDDEN);
    append_block(&b, 2, 1, 1, n2.bytes, n2.count);

    rm_write_id(&tg, 1, rm_crdt_make(0, 1));
    rm_write_id(&tg, 2, rm_crdt_make(1, 12)); /* SAME ID as g1! */
    rm_write_id(&tg, 3, RM_CRDT_NONE);
    rm_write_id(&tg, 4, RM_CRDT_NONE);
    rm_write_tag(&tg, 5, RM_TAG_BYTE4);
    rm_write_u32(&tg, 1);

    rm_write_id(&g1, 1, rm_crdt_make(0, 1));
    rm_write_id(&g1, 2, rm_crdt_make(1, 12));
    rm_write_id(&g1, 3, RM_CRDT_NONE);
    rm_write_id(&g1, 4, RM_CRDT_NONE);
    rm_write_tag(&g1, 5, RM_TAG_BYTE4);
    rm_write_u32(&g1, 0);
    rm_buf_append_byte(&g1v, 2);
    rm_write_id(&g1v, 2, SHOWN);
    rm_write_block(&g1, 6, g1v.bytes, g1v.count);

    rm_write_id(&g2, 1, rm_crdt_make(0, 1));
    rm_write_id(&g2, 2, rm_crdt_make(1, 22));
    rm_write_id(&g2, 3, rm_crdt_make(1, 12));
    rm_write_id(&g2, 4, RM_CRDT_NONE);
    rm_write_tag(&g2, 5, RM_TAG_BYTE4);
    rm_write_u32(&g2, 0);
    rm_buf_append_byte(&g2v, 2);
    rm_write_id(&g2v, 2, HIDDEN);
    rm_write_block(&g2, 6, g2v.bytes, g2v.count);

    if (tomb_first) append_block(&b, 4, 1, 1, tg.bytes, tg.count);
    append_block(&b, 4, 1, 1, g1.bytes, g1.count);
    append_block(&b, 4, 1, 1, g2.bytes, g2.count);
    if (!tomb_first) append_block(&b, 4, 1, 1, tg.bytes, tg.count);

    rm_edit_session* se = open_page(&b);
    RM_CHECK(rm_edit_move_layer(se, rm_item_id_of(SHOWN), 1,
                                (rm_item_id){1, 500}, nullptr) == RM_OK,
             "move_layer succeeds with duplicate tombstoned link order=%d",
             tomb_first);
    rm_edit_layer* ls = nullptr;
    size_t lc = 0;
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 2 &&
                 rm_item_id_equal(ls[0].node, rm_item_id_of(HIDDEN)) &&
                 rm_item_id_equal(ls[1].node, rm_item_id_of(SHOWN)),
             "reordered layers match [HIDDEN, SHOWN] order=%d", tomb_first);
    rm_edit_layers_free(ls);
    rm_edit_close(se);
    rm_buf bufs[] = {b, n1, n2, tg, g1, g1v, g2, g2v};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* Placement-free ADD_LAYER / DELETE_LAYER materialize root 0x01 placements
   * without exceeding fresh-ID budget (R249), fallback preservation with
   * in_tree == 0 removal / nested-group 0x01 (R42-1), and mixed placed A +
   * unplaced root 0x04 B (R41-1, R246, R250). */
  {
    rm_buf b;
    start_page(&b);
    append_two_layers(&b, false);
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    append_stroke(&b, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_FINELINER_2);
    rm_edit_session* se = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(se, 1, "Mid", (rm_item_id){1, 600}, nullptr) == RM_OK,
        "placement-free add_layer succeeds (R249)");
    rm_edit_layer* ls = nullptr;
    size_t lc = 0;
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 3 &&
                 rm_item_id_equal(ls[0].node, rm_item_id_of(HIDDEN)) &&
                 rm_item_id_equal(ls[2].node, rm_item_id_of(SHOWN)),
             "placement-free add_layer preserves all layers");
    rm_edit_layers_free(ls);
    RM_CHECK(rm_edit_delete_layer(se, rm_item_id_of(SHOWN),
                                  (rm_item_id){1, 610}, nullptr) == RM_OK,
             "delete_layer preserves remaining 2 layers");
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 2,
             "2 layers remain after delete");
    rm_edit_layers_free(ls);
    rm_edit_close(se);

    /* Fallback (i): placement-free 2-layer page + in_tree == 0 removal for
     * SHOWN still lists, draws, and edits surviving HIDDEN (R42-1). */
    rm_buf b1 = {0};
    rm_buf_append(&b1, b.bytes, b.count);
    append_removal(&b1, SHOWN);
    se = open_page(&b1);
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 1 &&
                 rm_item_id_equal(ls[0].node, rm_item_id_of(HIDDEN)),
             "in_tree==0 removal alone stays in 0x04 fallback for survivor");
    rm_edit_layers_free(ls);
    rm_page* p1 = parse_session_bytes(se);
    RM_CHECK(p1 && p1->stroke_count == 1 && p1->strokes[0].id.counter == 31,
             "fallback draws surviving layer stroke");
    rm_page_free(p1);
    rm_edit_close(se);
    rm_buf_free(&b1);

    /* Fallback (ii): placement-free 2-layer page + nested-group reinsertion
     * 0x01 stays in fallback mode (R42-1). */
    rm_buf b2 = {0};
    rm_buf_append(&b2, b.bytes, b.count);
    rm_crdt_id subg = rm_crdt_make(1, 70);
    append_nested_group(&b2, SHOWN, rm_crdt_make(1, 71), subg);
    append_reinsertion(&b2, subg, HIDDEN);
    se = open_page(&b2);
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 2,
             "nested-group 0x01 alone stays in 0x04 root fallback");
    rm_edit_layers_free(ls);
    rm_edit_close(se);
    rm_buf_free(&b2);

    /* Mixed placement: placed A (SHOWN via 0x01) + unplaced root 0x04 B
     * (HIDDEN) owning nested group + stroke S_b (R41-1, R246, R250). */
    rm_buf b3 = {0};
    rm_buf_append(&b3, b.bytes, b.count);
    append_reinsertion(&b3, SHOWN, RM_CRDT_ROOT);
    rm_crdt_id sub_b = rm_crdt_make(1, 80);
    append_nested_group(&b3, HIDDEN, rm_crdt_make(1, 81), sub_b);
    append_stroke(&b3, rm_crdt_make(1, 82), sub_b, RM_TOOL_FINELINER_2);
    rm_page* p3 = parse(b3.bytes, b3.count);
    RM_CHECK(p3 && p3->stroke_count == 1 && p3->strokes[0].id.counter == 30,
             "mixed placement excludes unplaced root B strokes (R41-1)");
    rm_page_free(p3);
    se = open_page(&b3);
    RM_CHECK(rm_edit_layers(se, &ls, &lc, nullptr) == RM_OK && lc == 1 &&
                 rm_item_id_equal(ls[0].node, rm_item_id_of(SHOWN)),
             "mixed placement rm_edit_layers returns only placed A");
    rm_edit_layers_free(ls);
    RM_CHECK(
        rm_edit_delete_layer(se, rm_item_id_of(SHOWN), (rm_item_id){1, 700},
                             nullptr) == RM_ERR_INVALID_ARGUMENT,
        "cannot delete sole placed layer A when B is unplaced (R246)");
    RM_CHECK(rm_edit_insert_stroke(se, rm_item_id_of(HIDDEN),
                                   (rm_item_id){1, 710}, none, none, &stroke,
                                   nullptr) == RM_ERR_ITEMS_NOT_FOUND,
             "insert into unplaced root B refused (R250)");
    RM_CHECK(
        rm_edit_insert_stroke(se, rm_item_id_of(SHOWN), (rm_item_id){1, 711},
                              none, none, &stroke, nullptr) == RM_OK,
        "insert into placed A succeeds");
    rm_edit_close(se);
    rm_buf_free(&b3);
    rm_buf_free(&b);
  }
}
static void edit_preserves_nested_error(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  rm_edit_session* s = open_page(&page);
  /* Inject a version failure after open, at the nested read inside the edit. */
  size_t size;
  uint8_t* data = rm_test_edit_bytes_mut(s, &size);
  data[RM_HEADER_LENGTH + 5] = 3;
  data[RM_HEADER_LENGTH + 6] = 3;
  rm_error error;
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "x", (rm_item_id){1, 200}, &error) ==
                   RM_ERR_UNSUPPORTED_BLOCK_VERSION &&
               error.value == 2 && error.version == 3 && error.message[0],
           "nested version diagnostic");
  rm_edit_close(s);
  rm_buf_free(&page);
}
static void migration_version_and_string_bounds(void) {
  rm_buf page;
  start_page(&page);
  append_block(&page, 0, 2, 2, nullptr, 0);
  rm_error detail;
  rm_page* p = nullptr;
  RM_CHECK(rm_parse_page(page.bytes, page.count, &p, &detail) ==
                   RM_ERR_UNSUPPORTED_BLOCK_VERSION &&
               detail.version == 2 && detail.value == 0,
           "migration minimum");
  rm_page_free(p);
  rm_buf_free(&page);
  size_t limit = (SIZE_MAX - 1) / 3;
  RM_CHECK(rm_utf8_repair_bound(limit) == limit * 3 + 1 &&
               rm_utf8_repair_bound(limit + 1) == 0 &&
               rm_utf8_repair_bound(SIZE_MAX) == 0,
           "checked repair bound");
}

static bool text_links(rm_edit_session* s, rm_item_id target, rm_item_id left,
                       rm_item_id right) {
  size_t size;
  const uint8_t* data = rm_edit_bytes(s, &size);
  block_view blocks[64];
  size_t n = blocks_of(data, size, blocks, 64);
  for (size_t i = 0; i < n; i++) {
    if (blocks[i].type != 7) continue;
    rm_reader r = {.data = blocks[i].body, .size = blocks[i].length};
    if (rm_skip_crdt(&r, 1) != RM_OK) return false;
    const unsigned tags[] = {2, 1, 1};
    for (size_t j = 0; j < 3; j++) {
      rm_reader sub;
      if (!rm_match_tag(&r, tags[j], RM_TAG_LENGTH4) ||
          rm_read_subblock(&r, &sub) != RM_OK)
        return false;
      r = sub;
    }
    size_t count;
    if (rm_read_count(&r, &count) != RM_OK) return false;
    for (size_t j = 0; j < count; j++) {
      rm_reader row;
      rm_crdt_id id, l, h;
      if (!rm_match_tag(&r, 0, RM_TAG_LENGTH4) ||
          rm_read_subblock(&r, &row) != RM_OK ||
          rm_read_crdt(&row, 2, &id) != RM_OK ||
          rm_read_crdt(&row, 3, &l) != RM_OK ||
          rm_read_crdt(&row, 4, &h) != RM_OK)
        return false;
      if (rm_crdt_equal(id, rm_crdt_of(target)))
        return rm_crdt_equal(l, rm_crdt_of(left)) &&
               rm_crdt_equal(h, rm_crdt_of(right));
    }
  }
  return false;
}
static void text_edit_neighbours(void) {
  rm_buf page;
  start_page(&page);
  rm_edit_session* s = open_page(&page);
  rm_item_id a = {1, 100}, b = {1, 200}, x = {1, 300}, none = {0};
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "A", a, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 0, 0, "B", b, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 1, 0, "X", x, nullptr) == RM_OK &&
               text_links(s, x, b, a),
           "record boundary links");
  rm_edit_close(s);
  s = open_page(&page);
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "AB", a, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 1, 1, "", none, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 1, 0, "X", x, nullptr) == RM_OK &&
               text_links(s, x, a, none),
           "append past tombstone");
  rm_edit_close(s);
  append_zeros_block(&page, 10, 108);
  s = open_page(&page);
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "X", x, nullptr) == RM_OK,
           "opaque PageInfo tail");
  rm_edit_close(s);
  rm_buf_free(&page);
}

static bool stats_are(const uint8_t* bytes, size_t size, uint32_t chars,
                      uint32_t lines) {
  block_view blocks[64];
  size_t n = blocks_of(bytes, size, blocks, 64);
  for (size_t i = 0; i < n; i++)
    if (blocks[i].type == 10) {
      rm_reader r = {.data = blocks[i].body, .size = blocks[i].length};
      uint32_t v;
      for (uint32_t tag = 1; tag <= 4; tag++) {
        if (!rm_match_tag(&r, tag, RM_TAG_BYTE4) ||
            rm_read_u32(&r, &v) != RM_OK)
          return false;
        if ((tag == 3 && v != chars) || (tag == 4 && v != lines)) return false;
      }
      return true;
    }
  return false;
}
static void replacing_styled_paragraph_break(void) {
  rm_draft_paragraph paragraphs[] = {
      {.text = "Intro"},
      {.text = "Section", .has_style = true, .style = RM_PARA_HEADING}};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = paragraphs,
                         .paragraph_count = 2};
  size_t size;
  uint8_t* data;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK,
           "write heading");
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 4, 3, "x\nS", (rm_item_id){1, 500},
                                    nullptr) == RM_OK,
           "replace paragraph break");
  const uint8_t* bytes = rm_edit_bytes(s, &size);
  rm_page* p = parse(bytes, size);
  RM_CHECK(p && p->text.paragraph_count == 2 &&
               p->text.paragraphs[1].has_format &&
               p->text.paragraphs[1].format.raw == RM_PARA_HEADING,
           "retain heading across replacement");
  rm_page_free(p);
  rm_edit_close(s);
  rm_bytes_free(data);
}
static void written_and_edited_text_counts(void) {
  rm_draft_paragraph paragraphs[] = {{.text = "AB", .bold = true},
                                     {.text = "😀"}};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = paragraphs,
                         .paragraph_count = 2};
  size_t size;
  uint8_t* data;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK, "write text");
  RM_CHECK(stats_are(data, size, 6, 2), "written PageInfo counts");
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 0, 1, "A", (rm_item_id){1, 800},
                                    nullptr) == RM_OK,
           "edit text");
  RM_CHECK(rm_edit_add_layer(s, 0, "overlap", (rm_item_id){1, 20}, nullptr) ==
               RM_ERR_INVALID_ARGUMENT,
           "layer overlaps text ID");
  const uint8_t* edited = rm_edit_bytes(s, &size);
  RM_CHECK(stats_are(edited, size, 6, 2), "edited PageInfo counts");
  rm_edit_close(s);
  rm_bytes_free(data);
}

static void check_carriage_return_edit(const uint8_t* bytes, size_t size,
                                       const char* text,
                                       const unsigned* counters, size_t count) {
  rm_page* page = nullptr;
  rmi_text_flat_map map = {0};
  rm_status status = rmi_parse_page_with_edit_state(bytes, size, &page, &map,
                                                    nullptr, nullptr);
  RM_CHECK(status == RM_OK && page && page->has_text &&
               page->skipped_blocks == 0 && page->text.paragraph_count == 1,
           "the edited text reads as one complete paragraph");
  if (page && page->text.paragraph_count == 1) {
    rm_str actual = page->text.paragraphs[0].text;
    RM_CHECK_EQ_STR(actual.bytes, actual.length, text, "paragraph text");
  }

  RM_CHECK_EQ_SIZE(map.count, count, "one ID per Unicode scalar");
  for (size_t i = 0; i < count && i < map.count; ++i) {
    RM_CHECK(rm_crdt_equal(map.ids[i], rm_crdt_make(1, counters[i])),
             "scalar %zu keeps ID (1, %u)", i, counters[i]);
  }

  rmi_text_flat_map_free(&map);
  rm_page_free(page);
}

static void replace_text_around_carriage_return(void) {
  rm_draft_paragraph paragraph = {.text = "é\r猫"};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = &paragraph,
                         .paragraph_count = 1};
  uint8_t* data = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK,
           "write a paragraph with a carriage return");
  if (!data) return;
  check_carriage_return_edit(data, size, "é\r猫", (unsigned[]){20, 21, 22}, 3);

  rm_edit_session* session = nullptr;
  RM_CHECK(rm_edit_open(data, size, &session, nullptr) == RM_OK,
           "open the paragraph for editing");
  if (session) {
    RM_CHECK(rm_edit_replace_text(session, 2, 0, "X\rλ", (rm_item_id){1, 800},
                                  nullptr) == RM_OK,
             "insert after the carriage return");
    const uint8_t* bytes = rm_edit_bytes(session, &size);
    check_carriage_return_edit(bytes, size, "é\rX\rλ猫",
                               (unsigned[]){20, 21, 800, 801, 802, 22}, 6);

    RM_CHECK(rm_edit_replace_text(session, 1, 3, "", (rm_item_id){0},
                                  nullptr) == RM_OK,
             "remove the two carriage returns and the character between them");
    bytes = rm_edit_bytes(session, &size);
    check_carriage_return_edit(bytes, size, "éλ猫", (unsigned[]){20, 802, 22},
                               3);
  }

  rm_edit_close(session);
  rm_bytes_free(data);
}

static void editing_repaired_text_and_face_markers(void) {
  rm_draft_paragraph paragraphs[] = {{.bold = true}};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = paragraphs};
  rm_edit_session* s = nullptr;
  uint8_t* data;
  size_t size;
  paragraphs[0].text = "QZB";
  draft.paragraph_count = 1;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK,
           "write bold text");
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 3, 0, "!", (rm_item_id){1, 800},
                                    nullptr) == RM_OK &&
               text_links(s, (rm_item_id){1, 800}, (rm_item_id){1, 22},
                          (rm_item_id){0}),
           "append past face mark");
  rm_edit_close(s);
  for (size_t i = 0; i + 3 <= size; i++)
    if (!memcmp(data + i, "QZB", 3)) {
      data[i + 1] = 0x80;
      break;
    }
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 1, 1, "X", (rm_item_id){1, 800},
                                    nullptr) == RM_OK,
           "edit repaired scalar");
  const uint8_t* view = rm_edit_bytes(s, &size);
  rm_page* parsed = parse(view, size);
  RM_CHECK(parsed && parsed->has_text && parsed->text.paragraph_count == 1 &&
               parsed->text.paragraphs[0].text.length == 3 &&
               !memcmp(parsed->text.paragraphs[0].text.bytes, "QXB", 3) &&
               text_links(s, (rm_item_id){1, 800}, (rm_item_id){1, 20},
                          (rm_item_id){1, 21}) &&
               text_links(s, (rm_item_id){1, 22}, (rm_item_id){1, 21},
                          (rm_item_id){0}),
           "keep Q and B IDs");
  rm_page_free(parsed);
  rm_edit_close(s);
  rm_bytes_free(data);
}

static void stroke_replacement_order(void) {
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, false);
  rm_item_id id = {1, 800}, eraser = {1, 900}, fresh = {2, 200};
  append_stroke(&page, rm_crdt_of(id), SHOWN, RM_TOOL_FINELINER_2);
  rm_edit_session* s = open_page(&page);
  rm_draft_point pt = rm_draft_point_make(0, 200);
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_ERASER, RM_COLOR_BLACK, &pt, 1);
  RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), eraser, id,
                                 (rm_item_id){0}, &stroke, nullptr) == RM_OK,
           "insert eraser");
  for (int cut = 0; cut <= 1; cut++) {
    rm_status status =
        cut ? rm_edit_cut_stroke(s, id, fresh, 1, 1, nullptr)
            : rm_edit_transform_stroke(
                  s, id, fresh, (rm_edit_transform){1, 0, 0, 1, 0, 0}, nullptr);
    RM_CHECK(status == RM_OK, "replace ink");
    size_t size;
    const uint8_t* bytes = rm_edit_bytes(s, &size);
    rm_page* parsed = parse(bytes, size);
    RM_CHECK(parsed && parsed->stroke_count == 2 &&
                 rm_item_id_equal(parsed->strokes[0].id, fresh) &&
                 rm_item_id_equal(parsed->strokes[1].id, eraser),
             "replacement stays below eraser (cut=%d)", cut);
    rm_page_free(parsed);
    RM_CHECK(rm_edit_undo(s), "undo replacement");
  }
  rm_item_id reversed[] = {eraser, id};
  size_t before_size;
  const uint8_t* before = rm_edit_bytes(s, &before_size);
  RM_CHECK(rm_edit_transform_strokes(s, reversed, 2, fresh,
                                     (rm_edit_transform){1, 0, 0, 1, 4, 2},
                                     nullptr) == RM_OK,
           "transform adjacent items in reverse selection order");
  size_t size;
  const uint8_t* after = rm_edit_bytes(s, &size);
  rm_page* result = parse(after, size);
  RM_CHECK(
      result && result->stroke_count == 2 &&
          rm_item_id_equal(result->strokes[0].id, (rm_item_id){2, 201}) &&
          rm_item_id_equal(result->strokes[1].id, fresh) &&
          result->strokes[1].tool == RM_TOOL_ERASER &&
          result->strokes[1].points[0].x == 4,
      "fresh IDs follow selection, drawing order and geometry follow scene");
  rm_page_free(result);
  block_view old[32], updated[32];
  size_t old_count = blocks_of(before, before_size, old, 32);
  RM_CHECK(blocks_of(after, size, updated, 32) == old_count + 2,
           "one replacement block per selected stroke");
  for (size_t i = 0; i < old_count; ++i)
    if (old[i].type != 5)
      RM_CHECK(same_block(&old[i], &updated[i]),
               "unchanged non-stroke block %zu", i);
  RM_CHECK(rm_edit_undo(s) && rm_edit_bytes(s, &size) == before &&
               size == before_size,
           "one undo restores both originals");
  rm_edit_close(s);
  rm_buf_free(&page);
}

static void removed_stroke_parents(void) {
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, false);
  append_reinsertion(&page, HIDDEN, SHOWN);
  append_removal(&page, SHOWN);
  append_stroke(&page, rm_crdt_make(1, 800), SHOWN, RM_TOOL_FINELINER_2);
  append_stroke(&page, rm_crdt_make(1, 801), HIDDEN, RM_TOOL_FINELINER_2);
  for (int i = 0; i < 2; i++) {
    rm_item_id id = {1, 800 + (uint64_t)i}, fresh = {2, 200};
    for (int cut = 0; cut <= 1; cut++) {
      rm_edit_session* s = open_page(&page);
      rm_status status =
          cut ? rm_edit_cut_stroke(s, id, fresh, 1, 1, nullptr)
              : rm_edit_transform_stroke(s, id, fresh,
                                         (rm_edit_transform){1, 0, 0, 1, 0, 0},
                                         nullptr);
      RM_CHECK(status == RM_ERR_ITEMS_NOT_FOUND && !rm_edit_undo(s),
               "removed parent %d, cut=%d", i, cut);
      rm_edit_close(s);
    }
  }
  rm_buf_free(&page);
}

static void repeated_current_layer_stamps(void) {
  rm_buf page, info = {0};
  start_page(&page);
  append_two_layers(&page, false);
  for (int i = 0; i < 2; i++) {
    rm_buf field = {0};
    rm_write_id(&field, 1, rm_crdt_make(1, i ? 900 : 100));
    rm_write_id(&field, 2, SHOWN);
    rm_write_block(&info, 1, field.bytes, field.count);
    rm_buf_free(&field);
  }
  append_block(&page, 13, 0, 1, info.bytes, info.count);
  rm_edit_session* s = open_page(&page);
  RM_CHECK(rm_edit_add_layer(s, 0, "new", (rm_item_id){1, 500}, nullptr) ==
                   RM_ERR_INVALID_ARGUMENT &&
               !rm_edit_undo(s),
           "a stale update cannot replace a repeated active-layer register");
  rm_edit_close(s);
  rm_buf_free(&page);
  rm_buf_free(&info);

  /* (a) & (b): Same-block tag 1 = SHOWN, newer tag 1 = HIDDEN, trailing tag 2 =
   * backgroundVisible=false -> deleting SHOWN keeps original 0x0D verbatim
   * (firmware tag 2 gated true while rm_parse_page reads false) AND
   * normalizes only when safe (R85, R145, R156). */
  {
    rm_buf b, ci = {0};
    start_page(&b);
    append_two_layers(&b, false);
    rm_buf f1 = {0}, f2 = {0};
    rm_write_id(&f1, 1, rm_crdt_make(1, 100));
    rm_write_id(&f1, 2, SHOWN);
    rm_write_block(&ci, 1, f1.bytes, f1.count);
    rm_write_id(&f2, 1, rm_crdt_make(1, 200));
    rm_write_id(&f2, 2, HIDDEN);
    rm_write_block(&ci, 1, f2.bytes, f2.count);
    append_block(&b, 13, 0, 1, ci.bytes, ci.count);
    rm_edit_session* se = open_page(&b);
    RM_CHECK(rm_edit_delete_layer(se, rm_item_id_of(SHOWN),
                                  (rm_item_id){1, 500}, nullptr) == RM_OK,
             "delete same-block leading layer A");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(se, &sz);
    bool first_is_hidden_b = false;
    rm_reader r = {.data = eb, .size = sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&r) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&r, &hdr) != RM_OK) break;
      if (hdr.type == 13) {
        rm_reader sub = r;
        sub.size = hdr.end;
        if (rm_expect_tag(&sub, 1, RM_TAG_LENGTH4) == RM_OK) {
          rm_reader val;
          rm_crdt_id stamp, layer;
          if (rm_read_subblock(&sub, &val) == RM_OK &&
              rm_read_crdt(&val, 1, &stamp) == RM_OK &&
              rm_read_crdt(&val, 2, &layer) == RM_OK &&
              rm_crdt_equal(stamp, rm_crdt_make(1, 200)) &&
              rm_crdt_equal(layer, HIDDEN)) {
            first_is_hidden_b = true;
          }
        }
      }
      r.offset = hdr.end;
    }
    RM_CHECK(first_is_hidden_b,
             "(a) normalized 0x0D leads with B and B's existing stamp (R85)");
    rm_edit_close(se);

    /* (b): with trailing tag 2 = false behind nonleading tag 1 = HIDDEN. */
    rm_buf b2, ci2 = {0};
    start_page(&b2);
    append_two_layers(&b2, false);
    rm_buf_append(&ci2, ci.bytes, ci.count);
    rm_write_stamped_u8(&ci2, 2, rm_crdt_make(1, 50), 0);
    append_block(&b2, 13, 0, 1, ci2.bytes, ci2.count);
    rm_page* pre_p = parse(b2.bytes, b2.count);
    RM_CHECK(
        pre_p && !pre_p->background_visible,
        "(b) pre-edit public rm_parse_page reads background_visible=false");
    rm_page_free(pre_p);
    se = open_page(&b2);
    RM_CHECK(rm_edit_delete_layer(se, rm_item_id_of(SHOWN),
                                  (rm_item_id){1, 500}, nullptr) == RM_OK,
             "(b) deleting A succeeds");
    eb = rm_edit_bytes(se, &sz);
    rm_page* post_p = parse(eb, sz);
    RM_CHECK(post_p && !post_p->background_visible,
             "(b) post-edit public background_visible remains false (R156)");
    rm_page_free(post_p);
    /* Original 0x0D block bytes survived untouched + new 0x0D emitted tag 1=B.
     */
    bool orig_0d_kept = false, extra_0d_b = false;
    r = (rm_reader){.data = eb, .size = sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&r) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&r, &hdr) != RM_OK) break;
      if (hdr.type == 13) {
        if (hdr.end - (hdr.start + 8u) == ci2.count &&
            memcmp(eb + hdr.start + 8u, ci2.bytes, ci2.count) == 0) {
          orig_0d_kept = true;
        } else {
          rm_reader sub = r;
          sub.size = hdr.end;
          rm_reader val;
          rm_crdt_id stamp, layer;
          if (rm_expect_tag(&sub, 1, RM_TAG_LENGTH4) == RM_OK &&
              rm_read_subblock(&sub, &val) == RM_OK &&
              rm_read_crdt(&val, 1, &stamp) == RM_OK &&
              rm_read_crdt(&val, 2, &layer) == RM_OK &&
              rm_crdt_equal(layer, HIDDEN)) {
            extra_0d_b = true;
          }
        }
      }
      r.offset = hdr.end;
    }
    RM_CHECK(orig_0d_kept && extra_0d_b,
             "(b) original gated 0x0D preserved verbatim & firmware cur=B");
    rm_edit_close(se);
    rm_buf_free(&b2);
    rm_buf_free(&ci2);
    rm_buf_free(&f1);
    rm_buf_free(&f2);
    rm_buf_free(&ci);
    rm_buf_free(&b);
  }

  /* (c): older tag 1 register with bounded extension bytes survives verbatim.
   */
  {
    rm_buf b, ci = {0}, f_ext = {0}, f_new = {0};
    start_page(&b);
    append_two_layers(&b, false);
    rm_write_id(&f_ext, 1, rm_crdt_make(1, 100));
    rm_write_id(&f_ext, 2, SHOWN);
    rm_write_tag(&f_ext, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&f_ext, 0x2A);
    rm_write_block(&ci, 1, f_ext.bytes, f_ext.count);
    rm_write_id(&f_new, 1, rm_crdt_make(1, 200));
    rm_write_id(&f_new, 2, HIDDEN);
    rm_write_block(&ci, 1, f_new.bytes, f_new.count);
    append_block(&b, 13, 0, 1, ci.bytes, ci.count);
    rm_edit_session* se = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(se, 0, "Top", (rm_item_id){1, 500}, nullptr) == RM_OK,
        "(c) add_layer preserves extended older tag 1");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(se, &sz);
    bool kept_ext = false;
    rm_reader r = {.data = eb, .size = sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&r) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&r, &hdr) != RM_OK) break;
      if (hdr.type == 13 && hdr.end - (hdr.start + 8u) == ci.count &&
          memcmp(eb + hdr.start + 8u, ci.bytes, ci.count) == 0) {
        kept_ext = true;
      }
      r.offset = hdr.end;
    }
    RM_CHECK(kept_ext,
             "(c) older extended tag 1 survived byte-for-byte (R151)");
    rm_edit_close(se);
    rm_buf_free(&f_new);
    rm_buf_free(&f_ext);
    rm_buf_free(&ci);
    rm_buf_free(&b);
  }

  /* (d), (e), (f): separate 0x0D blocks (leading A + nonleading newer B) +
   * 3-layer [A, C, B] deleting A preserves B; settings-only 0x0D;
   * nonleading-only surviving target deletion (R50, R58). */
  {
    rm_crdt_id THIRD = rm_crdt_make(1, 31);
    rm_buf b, b1 = {0}, f1 = {0}, b2 = {0}, f2 = {0}, sset = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_layer(&b, THIRD, false, rm_crdt_make(1, 32));
    append_layer(&b, HIDDEN, false, rm_crdt_make(1, 22));
    rm_write_id(&f1, 1, rm_crdt_make(1, 100));
    rm_write_id(&f1, 2, SHOWN);
    rm_write_block(&b1, 1, f1.bytes, f1.count);
    append_block(&b, 13, 0, 1, b1.bytes, b1.count);
    rm_write_stamped_u8(&b2, 2, rm_crdt_make(1, 50), 1);
    rm_write_id(&f2, 1, rm_crdt_make(1, 200));
    rm_write_id(&f2, 2, HIDDEN);
    rm_write_block(&b2, 1, f2.bytes, f2.count);
    append_block(&b, 13, 0, 1, b2.bytes, b2.count);

    rm_edit_session* se = open_page(&b);
    RM_CHECK(rm_edit_delete_layer(se, rm_item_id_of(SHOWN),
                                  (rm_item_id){1, 500}, nullptr) == RM_OK,
             "(d) deleting leading A when nonleading newer B survives");
    RM_CHECK(
        rm_edit_delete_layer(se, rm_item_id_of(HIDDEN), (rm_item_id){1, 150},
                             nullptr) == RM_ERR_INVALID_ARGUMENT,
        "(d) stale stamp against B confirms B remained logical winner");
    rm_edit_close(se);
    rm_buf_free(&f2);
    rm_buf_free(&b2);
    rm_buf_free(&f1);
    rm_buf_free(&b1);

    /* (e) settings-only 0x0D (no tag 1) + (f) nonleading-only surviving target
     * deletion. */
    b.count = RM_HEADER_LENGTH;
    append_two_layers(&b, false);
    rm_write_stamped_u8(&sset, 2, rm_crdt_make(1, 50), 1);
    append_block(&b, 13, 0, 1, sset.bytes, sset.count);
    se = open_page(&b);
    RM_CHECK(rm_edit_delete_layer(se, rm_item_id_of(SHOWN),
                                  (rm_item_id){1, 500}, nullptr) == RM_OK,
             "(e) settings-only 0x0D deletion succeeds");
    rm_edit_close(se);

    rm_buf nonlead = {0}, fn = {0};
    rm_write_stamped_u8(&nonlead, 2, rm_crdt_make(1, 50), 1);
    rm_write_id(&fn, 1, rm_crdt_make(1, 200));
    rm_write_id(&fn, 2,
                rm_crdt_make(1, 999)); /* nonexistent surviving target */
    rm_write_block(&nonlead, 1, fn.bytes, fn.count);
    append_block(&b, 13, 0, 1, nonlead.bytes, nonlead.count);
    se = open_page(&b);
    RM_CHECK(
        rm_edit_delete_layer(se, rm_item_id_of(SHOWN), (rm_item_id){1, 500},
                             nullptr) == RM_ERR_UNVERIFIED,
        "(f) nonexistent nonleading logical target refused (R58)");
    rm_edit_close(se);
    rm_buf_free(&fn);
    rm_buf_free(&nonlead);
    rm_buf_free(&sset);
    rm_buf_free(&b);
  }
}

static void historical_active_layers(void) {
  rm_buf page;
  start_page(&page);
  append_two_layers(&page, false);
  rm_crdt_id third = rm_crdt_make(1, 31);
  append_layer(&page, third, false, rm_crdt_make(1, 32));
  for (int i = 0; i < 2; i++) {
    rm_buf field = {0}, info = {0};
    rm_write_id(&field, 1, rm_crdt_make(1, i ? 200 : 100));
    rm_write_id(&field, 2, i ? third : SHOWN);
    rm_write_block(&info, 1, field.bytes, field.count);
    append_block(&page, 13, 0, 1, info.bytes, info.count);
    rm_buf_free(&field);
    rm_buf_free(&info);
  }
  rm_edit_session* s = open_page(&page);
  RM_CHECK(rm_edit_delete_layer(s, rm_item_id_of(SHOWN), (rm_item_id){1, 500},
                                nullptr) == RM_OK,
           "delete inactive layer");
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(s, &size);
  RM_CHECK(size >= page.count && !memcmp(bytes, page.bytes, page.count),
           "deleting inactive layer preserves both historical registers");
  rm_edit_close(s);
  rm_buf_free(&page);
}

static void interleaved_page_counts(void) {
  rm_buf page, info = {0};
  start_page(&page);
  for (uint32_t i = 1; i <= 4; i++) {
    if (i == 3) {
      rm_write_tag(&info, 6, RM_TAG_BYTE1);
      rm_buf_append_byte(&info, 0x74);
    }
    rm_write_tag(&info, i, RM_TAG_BYTE4);
    rm_write_u32(&info, i == 1);
  }
  append_block(&page, 10, 0, 1, info.bytes, info.count);
  rm_edit_session* s = open_page(&page);
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "AB", (rm_item_id){1, 400}, nullptr) ==
               RM_OK,
           "insert text with mixed PageInfo");
  info.bytes[info.count - 9] = 3;
  info.bytes[info.count - 4] = 1;
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(s, &size);
  block_view blocks[8];
  size_t n = blocks_of(bytes, size, blocks, 8);
  bool matched = false;
  for (size_t i = 0; i < n; i++)
    if (blocks[i].type == 10 && blocks[i].length == info.count)
      matched = !memcmp(blocks[i].body, info.bytes, info.count);
  RM_CHECK(matched,
           "mixed PageInfo retains unknown field and updates both counts");
  rm_edit_close(s);
  rm_buf_free(&page);
  rm_buf_free(&info);
}

static void stroke_point_allocation_overflow(void) {
  rm_arena* arena = rm_arena_create();
  RM_CHECK(rm_alloc_stroke_points(
               arena, SIZE_MAX / sizeof(rm_stroke_point) + 1) == nullptr,
           "reject overflowing stroke point array");
  rm_arena_destroy(arena);
}

static uint8_t* anchored_page(const char* first, const char* second,
                              size_t* size) {
  rm_draft_paragraph paragraphs[] = {{.text = first}, {.text = second}};
  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(40, 0)};
  rm_draft_mark mark = {.anchor = {RM_ANCHOR_FIRST_CHARACTER, 1},
                        .stroke = rm_draft_stroke_make(
                            RM_TOOL_FINELINER_2, RM_COLOR_BLACK, points, 2)};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = paragraphs,
                         .paragraph_count = 2,
                         .marks = &mark,
                         .mark_count = 1};
  uint8_t* data = nullptr;
  RM_CHECK(rm_write_page(&draft, &data, size, nullptr) == RM_OK,
           "write anchored page");
  return data;
}

static void replacing_anchored_character(void) {
  size_t size;
  uint8_t* data = anchored_page("Intro", "XYZ", &size);
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK,
           "open anchored page");
  const char* replacements[] = {"X", "Q"};
  for (size_t i = 0; i < 2; ++i) {
    RM_CHECK(
        rm_edit_replace_text(s, 6, 1, replacements[i],
                             (rm_item_id){1, 900000 + i}, nullptr) == RM_OK,
        "replace anchored X with %s", replacements[i]);
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].has_anchor &&
                 p->strokes[0].anchor.paragraph == 1 &&
                 p->strokes[0].anchor.offset == 1,
             "X to %s retains paragraph 1 offset 1", replacements[i]);
    rm_page_free(p);
    RM_CHECK(rm_edit_undo(s), "undo replacement");
  }
  RM_CHECK(rm_edit_replace_text(s, 6, 1, "", (rm_item_id){0}, nullptr) ==
               RM_ERR_UNVERIFIED,
           "refuse removal of the anchor target");
  size_t after_size;
  const uint8_t* after = rm_edit_bytes(s, &after_size);
  RM_CHECK(after_size == size && !memcmp(after, data, size) && !rm_edit_undo(s),
           "refusal preserves bytes and history");
  rm_edit_close(s);
  rm_bytes_free(data);
}

static void anchor_survives_peer_rename(void) {
  size_t size;
  uint8_t* data = anchored_page("ABC", "猫", &size);
  rm_edit_session *local = nullptr, *peer = nullptr;
  RM_CHECK(rm_edit_open(data, size, &local, nullptr) == RM_OK &&
               rm_edit_open(data, size, &peer, nullptr) == RM_OK,
           "open peers");
  rm_bytes_free(data);
  RM_CHECK(
      rm_edit_set_node_name(peer, (rm_item_id){1, 100000},
                            (rm_item_id){2, 900400}, "Peer", nullptr) == RM_OK,
      "rename anchored group");
  const rm_item_id invalid[] = {{2, 90}, {1, 99998}, {2, UINT64_MAX - 1}};
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
    RM_CHECK(rm_edit_replace_text(local, 3, 2, "\n犬", invalid[i], nullptr) ==
                     RM_ERR_INVALID_ARGUMENT &&
                 !rm_edit_undo(local),
             "refuse stale, colliding, or overflowing anchor stamp (%zu)", i);
  }
  RM_CHECK(rm_edit_replace_text(local, 3, 2, "\n犬", (rm_item_id){1, 900300},
                                nullptr) == RM_OK,
           "replace anchored character and break");
  const uint8_t* renamed = rm_edit_bytes(peer, &size);
  block_view blocks[64];
  size_t n = blocks_of(renamed, size, blocks, 64);
  const uint8_t* edited = rm_edit_bytes(local, &size);
  size_t checked = 0;
  for (size_t i = 0; i < n; ++i) {
    rm_anchor_field anchor;
    if (blocks[i].type != 2 ||
        rm_read_node_anchor(
            (rm_reader){.data = blocks[i].body, .size = blocks[i].length},
            &anchor) != RM_OK ||
        !anchor.field_end)
      continue;
    for (int last = 0; last <= 1; ++last) {
      rm_buf merged = {0};
      size_t split = last ? size : RM_HEADER_LENGTH;
      rm_buf_append(&merged, edited, split);
      rm_buf_append(&merged, blocks[i].body - 8, blocks[i].length + 8);
      rm_buf_append(&merged, edited + split, size - split);
      rm_page* p = parse(merged.bytes, merged.count);
      RM_CHECK(
          p && !p->skipped_blocks && p->stroke_count == 1 &&
              p->strokes[0].has_anchor && p->strokes[0].anchor.paragraph == 1 &&
              p->strokes[0].anchor.offset == 1 &&
              p->text.paragraph_count == 2 &&
              p->text.paragraphs[1].text.length == strlen("犬") &&
              !memcmp(p->text.paragraphs[1].text.bytes, "犬", strlen("犬")),
          "peer snapshot last=%d", last);
      rm_page_free(p);
      rm_buf_free(&merged);
      ++checked;
    }
  }
  RM_CHECK(checked == 2, "tested both orders");
  rm_edit_close(peer);
  rm_edit_close(local);
}

/* Remove only the group's sequence value, retaining its live placement. */
static rm_edit_session* session_with_unlinked_group(rm_crdt_id parent) {
  size_t size;
  uint8_t* data = anchored_page("ABC", "猫", &size);
  rm_buf page;
  start_page(&page);
  block_view blocks[64];
  size_t n = blocks_of(data, size, blocks, 64);
  size_t changed = 0;
  for (size_t i = 0; i < n; ++i) {
    block_view b = blocks[i];
    rm_reader r = {.data = b.body, .size = b.length};
    rm_item_preamble item;
    if (b.type == 4 && rm_parse_item_preamble(&r, &item) == RM_OK &&
        rm_crdt_equal(item.parent, parent)) {
      rm_buf body = {0};
      rm_buf_append(&body, b.body, r.offset - 4);
      rm_write_u32(&body, 1);
      append_block(&page, 4, b.min_version, b.current_version, body.bytes,
                   body.count);
      rm_buf_free(&body);
      ++changed;
    } else
      rm_buf_append(&page, b.body - 8, b.length + 8);
  }
  RM_CHECK(changed == 1, "removed one ordering link");
  rm_edit_session* s = open_page(&page);
  rm_buf_free(&page);
  rm_bytes_free(data);
  return s;
}

static void expect_stroke_order(rm_edit_session* s, uint64_t first,
                                uint64_t second) {
  rm_page* p = parse_session_bytes(s);
  RM_CHECK(p && !p->skipped_blocks && p->stroke_count == 2 &&
               p->strokes[0].id.counter == first &&
               p->strokes[1].id.counter == second,
           "expected stroke order [%llu %llu]", (unsigned long long)first,
           (unsigned long long)second);
  rm_page_free(p);
}

static void moving_layer_with_unlinked_descendant(void) {
  rm_item_id original = {1, 11}, other = {1, 900010};
  rm_edit_session* s = session_with_unlinked_group(rm_crdt_of(original));
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &point, 1);
  RM_CHECK(rm_edit_add_layer(s, 0, "Other", other, nullptr) == RM_OK &&
               rm_edit_insert_stroke(s, other, (rm_item_id){1, 900020},
                                     (rm_item_id){0}, (rm_item_id){0}, &stroke,
                                     nullptr) == RM_OK,
           "add other ink");
  expect_stroke_order(s, 900020, 100002);
  RM_CHECK(rm_edit_move_layer(s, original, 0, (rm_item_id){1, 900030},
                              nullptr) == RM_OK,
           "move");
  expect_stroke_order(s, 100002, 900020);
  RM_CHECK(rm_edit_undo(s), "undo move");
  expect_stroke_order(s, 900020, 100002);
  RM_CHECK(rm_edit_redo(s), "redo move");
  expect_stroke_order(s, 100002, 900020);
  rm_edit_close(s);
}

static void unlinked_layer_counts_toward_limit(void) {
  rm_edit_session* s = session_with_unlinked_group(rm_crdt_make(0, 1));
  for (size_t i = 0; i <= 4; ++i) {
    rm_edit_layer* layers = nullptr;
    size_t count = 0;
    RM_CHECK(rm_edit_layers(s, &layers, &count, nullptr) == RM_OK &&
                 count == i + 1 &&
                 rm_item_id_equal(layers[i].node, (rm_item_id){1, 11}),
             "original layer follows %zu added layers", i);
    rm_edit_layers_free(layers);
    if (i < 4)
      RM_CHECK(rm_edit_add_layer(s, i, "New", (rm_item_id){1, 900100 + 10 * i},
                                 nullptr) == RM_OK,
               "add layer below original");
  }
  size_t before_size, after_size;
  const uint8_t* before = rm_edit_bytes(s, &before_size);
  RM_CHECK(rm_edit_add_layer(s, 4, "Sixth", (rm_item_id){1, 900150}, nullptr) ==
               RM_ERR_INVALID_ARGUMENT,
           "reject sixth layer");
  RM_CHECK(rm_edit_bytes(s, &after_size) == before && after_size == before_size,
           "refusal retains current revision");
  RM_CHECK(rm_edit_undo(s), "undo fourth addition");
  rm_edit_layer* layers = nullptr;
  size_t count;
  RM_CHECK(rm_edit_layers(s, &layers, &count, nullptr) == RM_OK && count == 4 &&
               rm_item_id_equal(layers[3].node, (rm_item_id){1, 11}),
           "undo skips refused edit");
  rm_edit_layers_free(layers);
  rm_edit_close(s);
}

static void expect_visible_glyphs(rm_edit_session* s, size_t count) {
  rm_page* page = parse_session_bytes(s);
  RM_CHECK(page && page->highlight_count == count && page->skipped_blocks == 0,
           "expected %zu visible glyphs, got %zu", count,
           page ? page->highlight_count : 0);
  rm_page_free(page);
}

static void glyphs_follow_layer_visibility(void) {
  size_t size;
  uint8_t* data = sample("Wikipedia_highlighted_p1.rm", &size);
  rm_item_id layer = {0, 11};
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK,
           "open highlighted page");
  expect_visible_glyphs(s, 4);
  RM_CHECK(rm_edit_set_node_visible(s, layer, (rm_item_id){1, 900001}, false,
                                    nullptr) == RM_OK,
           "hide glyph layer");
  expect_visible_glyphs(s, 0);
  RM_CHECK(rm_edit_undo(s), "undo hide");
  expect_visible_glyphs(s, 4);
  RM_CHECK(rm_edit_add_layer(s, 1, "Blank", (rm_item_id){1, 900100}, nullptr) ==
                   RM_OK &&
               rm_edit_delete_layer(s, layer, (rm_item_id){1, 900200},
                                    nullptr) == RM_OK,
           "delete visible glyph layer");
  expect_visible_glyphs(s, 0);
  RM_CHECK(rm_edit_undo(s), "undo deletion");
  expect_visible_glyphs(s, 4);
  rm_edit_close(s);
  free(data);
}

static void internal_transform_checks_empty_selections(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));

  const rm_edit_transform identity = {.a = 1, .d = 1};
  rmi_edit_candidate candidate = {0};
  rm_error error = {0};
  rm_status status =
      rmi_edit_transform_strokes(page.bytes, page.count, nullptr, 0,
                                 (rm_item_id){0}, identity, &candidate, &error);
  RM_CHECK(status == RM_OK && candidate.verified && candidate.bytes &&
               candidate.bytes != page.bytes && candidate.size == page.count &&
               memcmp(candidate.bytes, page.bytes, page.count) == 0,
           "an empty internal selection returns a verified copy");
  free(candidate.bytes);

  const rm_edit_transform invalid[] = {{0}, {.a = 1, .d = 1, .tx = NAN}};
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
    status = rmi_edit_transform_strokes(page.bytes, page.count, nullptr, 0,
                                        (rm_item_id){0}, invalid[i], &candidate,
                                        &error);
    RM_CHECK(status == RM_ERR_INVALID_ARGUMENT && error.status == status &&
                 !candidate.bytes,
             "an empty internal selection refuses invalid transform %zu", i);
    free(candidate.bytes);
  }
  rm_buf_free(&page);
}

static void id_check_reports_a_block_past_eof(void) {
  rm_buf page;
  start_page(&page);
  rm_write_u32(&page, 1);
  rm_write_u32(&page, 0);

  rm_error error = {0};
  rm_status status = rmi_scene_check_ids(page.bytes, page.count,
                                         (rm_item_id){1, 30}, 1, &error);
  RM_CHECK(status == RM_ERR_UNEXPECTED_EOF && error.status == status,
           "the ID check reports the unreadable block in the error");
  rm_buf_free(&page);
}

static void transform_rejects_nonfinite_translation_on_empty_stroke(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  /* A 0-point stroke block. */
  rm_buf value = {0}, points = {0}, line = {0};
  rm_buf_append_byte(&value, 0x03);
  rm_write_tag(&value, 1, RM_TAG_BYTE4);
  rm_write_u32(&value, (uint32_t)RM_TOOL_FINELINER_2);
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_write_u32(&value, 0);
  rm_write_tag(&value, 3, RM_TAG_BYTE8);
  rm_write_f64(&value, 1);
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_f32(&value, 0);
  rm_write_block(&value, 5, points.bytes, points.count);
  rm_write_id(&line, 1, SHOWN);
  rm_write_id(&line, 2, rm_crdt_make(1, 50));
  rm_write_id(&line, 3, RM_CRDT_NONE);
  rm_write_id(&line, 4, RM_CRDT_NONE);
  rm_write_tag(&line, 5, RM_TAG_BYTE4);
  rm_write_u32(&line, 0);
  rm_write_block(&line, 6, value.bytes, value.count);
  append_block(&page, 5, 2, 2, line.bytes, line.count);
  rm_buf_free(&value);
  rm_buf_free(&points);
  rm_buf_free(&line);

  rm_edit_session* s = open_page(&page);
  rm_edit_transform bad = {1, 0, 0, 1, NAN, 0};
  RM_CHECK(rm_edit_transform_stroke(s, (rm_item_id){1, 50}, (rm_item_id){1, 51},
                                    bad, nullptr) == RM_ERR_INVALID_ARGUMENT,
           "non-finite translation on empty stroke is refused");
  rm_edit_close(s);
  rm_buf_free(&page);
}

static void id_collision_checks_cover_all_blocks_and_node_stamps(void) {
  rm_draft_paragraph paras[] = {{.text = "Hello"}};
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
  uint8_t img_key[16] = {1};
  rm_draft_image img = {.file = "a.png", .width = 10, .height = 10};
  memcpy(img.key, img_key, 16);
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
                         .column = rm_draft_column_of(RM_COLUMN_MEDIUM),
                         .paragraphs = paras,
                         .paragraph_count = 1,
                         .strokes = &stroke,
                         .stroke_count = 1,
                         .images = &img,
                         .image_count = 1};
  draft.author[0] = 1;
  uint8_t* data = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK, "write");
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK, "open");
  rm_bytes_free(data);

  rm_item_id layer = {1, 11};
  rm_item_id none = {0};
  /* (1,1) = RootText/MigrationInfo, (1,15) = width stamp, (1,300002) =
   * attachment name stamp, (1,200000) = existing stroke. */
  RM_CHECK(rm_edit_insert_stroke(s, layer, (rm_item_id){1, 1}, none, none,
                                 &stroke, nullptr) == RM_ERR_INVALID_ARGUMENT,
           "collision with RootText/MigrationInfo id (1,1) refused");
  RM_CHECK(rm_edit_insert_stroke(s, layer, (rm_item_id){1, 15}, none, none,
                                 &stroke, nullptr) == RM_ERR_INVALID_ARGUMENT,
           "collision with RootText width stamp (1,15) refused");
  RM_CHECK(rm_edit_insert_stroke(s, layer, (rm_item_id){1, 300002}, none, none,
                                 &stroke, nullptr) == RM_ERR_INVALID_ARGUMENT,
           "collision with AttachmentMap stamp (1,300002) refused");
  RM_CHECK(rm_edit_set_node_name(s, layer, (rm_item_id){1, 200000}, "Renamed",
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
           "node stamp colliding with existing stroke id is refused");
  rm_edit_close(s);
}

static void replace_text_ignores_anchors_on_removed_layers(void) {
  size_t size = 0;
  uint8_t* data = anchored_page("Intro", "XYZ", &size);
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK, "open");
  rm_bytes_free(data);

  /* Add a second layer and delete Layer 1 (which owns the anchored mark). */
  RM_CHECK(rm_edit_add_layer(s, 1, "Layer 2", (rm_item_id){1, 900010},
                             nullptr) == RM_OK,
           "add layer 2");
  RM_CHECK(rm_edit_delete_layer(s, (rm_item_id){1, 11}, (rm_item_id){1, 900020},
                                nullptr) == RM_OK,
           "delete layer 1");
  /* Removing the anchored character on a dead layer must succeed. */
  RM_CHECK(rm_edit_replace_text(s, 6, 1, "", (rm_item_id){0}, nullptr) == RM_OK,
           "replace_text ignores anchors under removed layers");
  rm_edit_close(s);
}

static void out_of_order_and_bare_tags_in_node_and_scene_info(void) {
  /* Part 1: 0x02 with bare BYTE4 tag 10 before tag 3 (visibility). */
  rm_buf page, node = {0}, group = {0}, value = {0};
  start_page(&page);
  rm_write_id(&node, 1, SHOWN);
  rm_write_tag(&node, 10, RM_TAG_BYTE4);
  rm_write_f32(&node, 0.0f);
  rm_write_stamped_u8(&node, 3, rm_crdt_make(1, 50), 1);
  append_block(&page, 2, 1, 2, node.bytes, node.count);

  rm_write_id(&group, 1, rm_crdt_make(0, 1));
  rm_write_id(&group, 2, rm_crdt_make(1, 12));
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, SHOWN);
  rm_write_block(&group, 6, value.bytes, value.count);
  append_block(&page, 4, 1, 1, group.bytes, group.count);

  rm_edit_session* s = open_page(&page);
  RM_CHECK(
      rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 20},
                               false, nullptr) == RM_ERR_INVALID_ARGUMENT,
      "stale visibility stamp after bare tag 10 is refused");
  RM_CHECK(
      rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 60},
                               false, nullptr) == RM_OK,
      "newer visibility stamp after bare tag 10 succeeds");
  rm_edit_close(s);
  rm_buf_free(&page);
  rm_buf_free(&node);
  rm_buf_free(&group);
  rm_buf_free(&value);

  /* Part 2: 0x0D with tag 2 before tag 1 (current layer). */
  start_page(&page);
  append_two_layers(&page, false);
  rm_buf info = {0}, layer_ref = {0};
  rm_write_stamped_u8(&info, 2, rm_crdt_make(1, 30), 1);
  rm_write_id(&layer_ref, 1, rm_crdt_make(1, 40));
  rm_write_id(&layer_ref, 2, SHOWN);
  rm_write_block(&info, 1, layer_ref.bytes, layer_ref.count);
  append_block(&page, 13, 0, 1, info.bytes, info.count);

  s = open_page(&page);
  /* Stale stamp (1, 35) < (1, 40) on add_layer must be refused! */
  RM_CHECK(rm_edit_add_layer(s, 0, "New", (rm_item_id){1, 32}, nullptr) ==
               RM_ERR_INVALID_ARGUMENT,
           "stale add_layer stamp against tag 1 after tag 2 is refused");
  RM_CHECK(rm_edit_delete_layer(s, rm_item_id_of(SHOWN), (rm_item_id){1, 90},
                                nullptr) == RM_OK,
           "delete active layer succeeds");
  size_t sz = 0;
  const uint8_t* bytes = rm_edit_bytes(s, &sz);
  /* Verify current layer in 0x0D was updated to HIDDEN (1,21), not left as
   * deleted SHOWN (1,11). */
  block_view blocks[32];
  size_t nb = blocks_of(bytes, sz, blocks, 32);
  bool found_hidden = false;
  for (size_t i = 0; i < nb; ++i) {
    if (blocks[i].type != 13) continue;
    rm_reader r = {.data = blocks[i].body, .size = blocks[i].length};
    while (rm_reader_remaining(&r)) {
      uint32_t idx;
      uint8_t typ;
      if (rm_read_tag(&r, &idx, &typ) != RM_OK || typ != RM_TAG_LENGTH4) break;
      rm_reader sub;
      if (rm_read_subblock(&r, &sub) != RM_OK) break;
      if (idx == 1) {
        rm_crdt_id cur;
        if (rm_skip_crdt(&sub, 1) == RM_OK &&
            rm_read_crdt(&sub, 2, &cur) == RM_OK &&
            rm_crdt_equal(cur, HIDDEN)) {
          found_hidden = true;
        }
      }
    }
  }
  RM_CHECK(found_hidden, "deleted active layer updates tag 1 after tag 2");
  rm_edit_close(s);
  rm_buf_free(&page);
  rm_buf_free(&info);
  rm_buf_free(&layer_ref);
}

static void deleted_text_row_with_nonempty_value_is_ignored(void) {
  rm_buf page, list = {0}, inner = {0}, items = {0}, root = {0}, pos = {0};
  start_page(&page);
  rm_buf row = {0}, val = {0};
  rm_write_id(&row, 2, rm_crdt_make(1, 10));
  rm_write_id(&row, 3, RM_CRDT_NONE);
  rm_write_id(&row, 4, RM_CRDT_NONE);
  rm_write_tag(&row, 5, RM_TAG_BYTE4);
  rm_write_u32(&row, 2); /* deleted = 2 scalars */
  rm_write_string(&val, "ab");
  rm_write_block(&row, 6, val.bytes, val.count);
  rm_write_varuint(&list, 1);
  rm_write_block(&list, 0, row.bytes, row.count);
  rm_write_block(&inner, 1, list.bytes, list.count);
  rm_write_block(&items, 1, inner.bytes, inner.count);
  rm_write_id(&root, 1, RM_CRDT_NONE);
  rm_write_block(&root, 2, items.bytes, items.count);
  rm_write_f64(&pos, -468);
  rm_write_f64(&pos, 234);
  rm_write_block(&root, 3, pos.bytes, pos.count);
  rm_write_tag(&root, 4, RM_TAG_BYTE4);
  rm_write_f32(&root, 936.0f);
  append_block(&page, 0x07, 1, 1, root.bytes, root.count);

  rm_page* p = parse(page.bytes, page.count);
  RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
               p->text.paragraphs[0].text.length == 0,
           "deleted text row with payload yields empty text");
  rm_page_free(p);

  rm_edit_session* s = open_page(&page);
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "hi", (rm_item_id){1, 50}, nullptr) ==
               RM_OK,
           "inserting into page with deleted non-empty row succeeds");
  rm_edit_close(s);
  rm_buf_free(&row);
  rm_buf_free(&val);
  rm_buf_free(&list);
  rm_buf_free(&inner);
  rm_buf_free(&items);
  rm_buf_free(&root);
  rm_buf_free(&pos);
  rm_buf_free(&page);
}

static void node_and_layer_names_repair_ill_formed_utf8(void) {
  rm_page_draft draft = {
      .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
  uint8_t* data = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK, "write");
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK, "open");
  rm_bytes_free(data);

  RM_CHECK(rm_edit_set_node_name(s, (rm_item_id){1, 11}, (rm_item_id){1, 90001},
                                 "bad\x80name", nullptr) == RM_OK,
           "set_node_name");
  RM_CHECK(rm_edit_add_layer(s, 1,
                             "lay\x80"
                             "er",
                             (rm_item_id){1, 90010}, nullptr) == RM_OK,
           "add_layer");
  size_t out_size = 0;
  const uint8_t* out = rm_edit_bytes(s, &out_size);
  const char needle[] = "bad\xEF\xBF\xBDname";
  const char needle2[] =
      "lay\xEF\xBF\xBD"
      "er";
  bool has1 = false, has2 = false;
  for (size_t i = 0; i + sizeof(needle) - 1 <= out_size; ++i) {
    if (memcmp(out + i, needle, sizeof(needle) - 1) == 0) has1 = true;
  }
  for (size_t i = 0; i + sizeof(needle2) - 1 <= out_size; ++i) {
    if (memcmp(out + i, needle2, sizeof(needle2) - 1) == 0) has2 = true;
  }
  RM_CHECK(has1 && has2, "node and layer names repair ill-formed UTF-8");
  rm_edit_close(s);
}

static void replace_text_protects_anchors_on_live_hidden_layers(void) {
  size_t size = 0;
  uint8_t* data = anchored_page("Intro", "XYZ", &size);
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(data, size, &s, nullptr) == RM_OK, "open");
  rm_bytes_free(data);

  /* Hide Layer 1 (keeping it LIVE in the tree). */
  RM_CHECK(
      rm_edit_set_node_visible(s, (rm_item_id){1, 11}, (rm_item_id){1, 900010},
                               false, nullptr) == RM_OK,
      "hide live layer 1");
  /* Removing the anchored character on a live hidden layer is refused, and
   * replacing it remaps cleanly. */
  RM_CHECK(rm_edit_replace_text(s, 6, 1, "", (rm_item_id){0}, nullptr) ==
               RM_ERR_UNVERIFIED,
           "replace_text rejects deleting anchor on live hidden layer");
  RM_CHECK(rm_edit_replace_text(s, 6, 1, "Q", (rm_item_id){1, 900020},
                                nullptr) == RM_OK,
           "replace_text remaps anchor on live hidden layer");
  rm_edit_close(s);
}

static rm_page* parse_session_page(const rm_edit_session* s) {
  size_t sz = 0;
  const uint8_t* bytes = rm_edit_bytes(s, &sz);
  rm_page* p = nullptr;
  return rm_parse_page(bytes, sz, &p, nullptr) == RM_OK ? p : nullptr;
}

static void unstamped_style_with_preceding_field_survives_replace(void) {
  rm_buf b, root = {0}, both = {0}, items = {0}, styles = {0}, pos = {0};
  rm_buf list = {0}, row = {0}, val = {0}, st = {0};
  start_page(&b);
  append_layer(&b, SHOWN, true, rm_crdt_make(1, 12));
  rm_write_id(&root, 1, RM_CRDT_NONE);

  rm_write_id(&row, 2, rm_crdt_make(1, 20));
  rm_write_id(&row, 3, RM_CRDT_NONE);
  rm_write_id(&row, 4, RM_CRDT_NONE);
  rm_write_tag(&row, 5, RM_TAG_BYTE4);
  rm_write_u32(&row, 0);
  rm_write_string(&val, "\nHi");
  rm_write_block(&row, 6, val.bytes, val.count);
  rm_write_varuint(&list, 1);
  rm_write_block(&list, 0, row.bytes, row.count);
  rm_write_block(&items, 1, list.bytes, list.count);
  rm_write_block(&both, 1, items.bytes, items.count);

  /* Sibling tag 9 in TAG_TEXT_ITEMS before tag 2 (styles); style for
   * paragraph 1 uses key (1, 20) and omits optional tag 1 stamp. */
  uint8_t dummy = 7;
  rm_write_block(&both, 9, &dummy, 1);
  rm_buf sentries = {0};
  rm_write_varuint(&sentries, 1);
  rm_write_bare_id(&sentries, rm_crdt_make(1, 20));
  uint8_t fmt[] = {0x11, 0x02};
  rm_write_block(&sentries, 2, fmt, sizeof(fmt));
  rm_write_block(&st, 1, sentries.bytes, sentries.count);
  rm_write_block(&both, 2, st.bytes, st.count);

  rm_write_block(&root, 2, both.bytes, both.count);
  rm_write_f64(&pos, 0);
  rm_write_f64(&pos, 0);
  rm_write_block(&root, 3, pos.bytes, pos.count);
  append_block(&b, 7, 1, 1, root.bytes, root.count);

  rm_edit_session* s = open_page(&b);
  RM_CHECK(rm_edit_replace_text(s, 0, 1, "\n", (rm_item_id){1, 50}, nullptr) ==
               RM_OK,
           "replace_text preserves unstamped style with sibling tag");
  rm_page* p = parse_session_page(s);
  RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
               p->text.paragraphs[1].has_format &&
               p->text.paragraphs[1].format.raw == 2,
           "remapped style survives round-trip");
  rm_page_free(p);
  rm_edit_close(s);
  rm_buf_free(&b);
  rm_buf_free(&root);
  rm_buf_free(&both);
  rm_buf_free(&items);
  rm_buf_free(&styles);
  rm_buf_free(&pos);
  rm_buf_free(&list);
  rm_buf_free(&row);
  rm_buf_free(&val);
  rm_buf_free(&st);
  rm_buf_free(&sentries);
}

static void lww_extension_singular_transform_and_out_of_order_stroke(void) {
  rm_buf b, node = {0}, group = {0}, gval = {0}, lval = {0}, pts = {0},
            line = {0};
  start_page(&b);
  rm_write_id(&node, 1, SHOWN);
  /* LWW name register (tag 2) carrying extra inner extension tag 3. */
  rm_buf reg = {0}, orig_str = {0};
  rm_write_id(&reg, 1, rm_crdt_make(1, 20));
  rm_write_string(&orig_str, "Orig");
  rm_write_block(&reg, 2, orig_str.bytes, orig_str.count);
  rm_buf_free(&orig_str);
  rm_write_tag(&reg, 3, RM_TAG_BYTE4);
  rm_write_u32(&reg, 0x12345678u);
  rm_write_block(&node, 2, reg.bytes, reg.count);
  append_block(&b, 2, 1, 2, node.bytes, node.count);

  rm_write_id(&group, 1, rm_crdt_make(0, 1));
  rm_write_id(&group, 2, rm_crdt_make(1, 12));
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&gval, 2);
  rm_write_id(&gval, 2, SHOWN);
  rm_write_block(&group, 6, gval.bytes, gval.count);
  append_block(&b, 4, 1, 1, group.bytes, group.count);

  /* Out-of-order stroke tail tags: 8, 6, 9, 7 after points tag 5. */
  rm_buf_append_byte(&lval, 0x03);
  rm_write_tag(&lval, 1, RM_TAG_BYTE4);
  rm_write_u32(&lval, RM_TOOL_SHADER);
  rm_write_tag(&lval, 2, RM_TAG_BYTE4);
  rm_write_u32(&lval, RM_COLOR_BLACK);
  rm_write_tag(&lval, 3, RM_TAG_BYTE8);
  rm_write_f64(&lval, 2.0);
  rm_write_tag(&lval, 4, RM_TAG_BYTE4);
  rm_write_f32(&lval, 0.0f);
  for (int i = 0; i < 2; ++i) {
    rm_write_f32(&pts, (float)(i * 10));
    rm_write_f32(&pts, 0.0f);
    rm_write_u16(&pts, 0);
    rm_write_u16(&pts, 12);
    rm_buf_append_byte(&pts, 0);
    rm_buf_append_byte(&pts, 6);
  }
  rm_write_block(&lval, 5, pts.bytes, pts.count);
  rm_write_tag(&lval, 8, RM_TAG_BYTE4);
  rm_write_u32(&lval, 0xFF112233u);
  rm_write_id(&lval, 6, rm_crdt_make(1, 10));
  rm_write_id(&lval, 9, rm_crdt_make(1, 7));
  rm_write_id(&lval, 7, rm_crdt_make(1, 8));
  rm_write_id(&line, 1, SHOWN);
  rm_write_id(&line, 2, rm_crdt_make(1, 50));
  rm_write_id(&line, 3, RM_CRDT_NONE);
  rm_write_id(&line, 4, RM_CRDT_NONE);
  rm_write_tag(&line, 5, RM_TAG_BYTE4);
  rm_write_u32(&line, 0);
  rm_write_block(&line, 6, lval.bytes, lval.count);
  append_block(&b, 5, 2, 2, line.bytes, line.count);

  rm_edit_session* s = open_page(&b);
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 30},
                                 "Updated", nullptr) == RM_OK,
           "set_node_name preserves LWW extension");
  size_t out_sz = 0;
  const uint8_t* out = rm_edit_bytes(s, &out_sz);
  const uint8_t needle[4] = {0x78, 0x56, 0x34, 0x12};
  bool ext_kept = false;
  for (size_t i = 0; i + 4 <= out_sz; ++i) {
    if (memcmp(out + i, needle, 4) == 0) ext_kept = true;
  }
  RM_CHECK(ext_kept, "extension tag inside LWW node register survived splice");

  rm_page* orig_parsed = parse_session_page(s);
  RM_CHECK(orig_parsed && orig_parsed->stroke_count == 1 &&
               orig_parsed->strokes[0].has_argb &&
               orig_parsed->strokes[0].argb == 0xFF112233u &&
               orig_parsed->strokes[0].has_source_id &&
               rm_item_id_equal(orig_parsed->strokes[0].source_id,
                                (rm_item_id){1, 7}),
           "public parser preserves reordered stroke tail properties");
  rm_page_free(orig_parsed);

  rm_edit_transform singular = {1, 2, 2, 4, 0, 0};
  RM_CHECK(
      rm_edit_transform_stroke(s, (rm_item_id){1, 50}, (rm_item_id){1, 51},
                               singular, nullptr) == RM_ERR_INVALID_ARGUMENT,
      "singular transform rejected");
  RM_CHECK(rm_edit_transform_stroke(s, (rm_item_id){1, 50}, (rm_item_id){1, 52},
                                    (rm_edit_transform){1, 0, 0, 1, 5, 0},
                                    nullptr) == RM_OK,
           "out-of-order stroke tail tags transform cleanly");
  rm_page* p = parse_session_page(s);
  RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].has_argb &&
               p->strokes[0].argb == 0xFF112233u &&
               p->strokes[0].has_source_id &&
               rm_item_id_equal(p->strokes[0].source_id, (rm_item_id){1, 7}),
           "stroke tail properties survived transform");
  rm_page_free(p);
  rm_edit_close(s);
  rm_buf_free(&b);
  rm_buf_free(&node);
  rm_buf_free(&reg);
  rm_buf_free(&group);
  rm_buf_free(&gval);
  rm_buf_free(&lval);
  rm_buf_free(&pts);
  rm_buf_free(&line);
}

static void highlighter_order_survives_multi_item_tombstone(void) {
  rm_buf b;
  start_page(&b);
  append_two_layers(&b, false);
  append_stroke(&b, rm_crdt_make(1, 31), SHOWN, RM_TOOL_HIGHLIGHTER_1);
  append_stroke(&b, rm_crdt_make(1, 32), HIDDEN, RM_TOOL_HIGHLIGHTER_1);
  append_stroke(&b, rm_crdt_make(1, 33), SHOWN, RM_TOOL_HIGHLIGHTER_1);

  rm_buf tomb = {0}, interior = {0};
  rm_write_id(&tomb, 1, SHOWN);
  rm_write_id(&tomb, 2, rm_crdt_make(1, 80));
  rm_write_id(&tomb, 3, RM_CRDT_NONE);
  rm_write_id(&tomb, 4, RM_CRDT_NONE);
  rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
  rm_write_u32(&tomb, 2);
  append_block(&b, 8, 1, 1, tomb.bytes, tomb.count);
  /* Reference the interior of the 2-element tombstone so part_count == 2. */
  rm_write_id(&interior, 1, SHOWN);
  rm_write_id(&interior, 2, rm_crdt_make(1, 82));
  rm_write_id(&interior, 3, rm_crdt_make(1, 80));
  rm_write_id(&interior, 4, rm_crdt_make(1, 81));
  rm_write_tag(&interior, 5, RM_TAG_BYTE4);
  rm_write_u32(&interior, 1);
  append_block(&b, 8, 1, 1, interior.bytes, interior.count);
  rm_buf_free(&interior);
  rm_buf_free(&tomb);

  rm_test_reset_chain_order_calls();
  rm_item_id* ids = nullptr;
  size_t count = 0;
  RM_CHECK(
      rm_highlighter_items(b.bytes, b.count, &ids, &count, nullptr) == RM_OK &&
          count == 3 && ids[0].counter == 31 && ids[1].counter == 32 &&
          ids[2].counter == 33 && rm_test_split_output_count() >= 5,
      "highlighter items preserve file order despite split tombstone");
  rm_item_ids_free(ids);
  rm_buf_free(&b);
}

static void noop_edits_preserve_redo_history(void) {
  rm_buf b;
  start_page(&b);
  append_two_layers(&b, false);
  append_stroke(&b, rm_crdt_make(1, 31), SHOWN, RM_TOOL_FINELINER_2);

  rm_edit_session* s = open_page(&b);
  RM_CHECK(
      rm_edit_replace_text(s, 0, 0, "A", (rm_item_id){1, 50}, nullptr) == RM_OK,
      "initial text edit");
  RM_CHECK(rm_edit_undo(s), "undo creates redo branch");

  RM_CHECK(rm_edit_erase(s, nullptr, 0, nullptr) == RM_OK && rm_edit_redo(s),
           "0-id erase preserves redo");
  RM_CHECK(rm_edit_undo(s), "rewind");

  RM_CHECK(
      rm_edit_replace_text(s, 0, 0, "", (rm_item_id){0}, nullptr) == RM_OK &&
          rm_edit_redo(s),
      "empty replace_text preserves redo");
  RM_CHECK(rm_edit_undo(s), "rewind");

  RM_CHECK(rm_edit_transform_strokes(s, nullptr, 0, (rm_item_id){1, 90},
                                     (rm_edit_transform){1, 0, 0, 1, 0, 0},
                                     nullptr) == RM_OK &&
               rm_edit_redo(s),
           "0-count transform_strokes preserves redo");
  RM_CHECK(rm_edit_undo(s), "rewind");

  RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 31}, (rm_item_id){1, 91}, 0, 0,
                              nullptr) == RM_OK &&
               rm_edit_redo(s),
           "0-point cut_stroke preserves redo");
  RM_CHECK(rm_edit_undo(s), "rewind");

  rm_edit_layer* layers = nullptr;
  size_t lcount = 0;
  RM_CHECK(rm_edit_layers(s, &layers, &lcount, nullptr) == RM_OK && lcount > 0,
           "list layers");
  rm_item_id top_layer = layers[0].node;
  rm_edit_layers_free(layers);
  RM_CHECK(rm_edit_move_layer(s, top_layer, 0, (rm_item_id){1, 92}, nullptr) ==
                   RM_OK &&
               rm_edit_redo(s),
           "same-position move_layer preserves redo");

  rm_edit_close(s);
  rm_buf_free(&b);
}

static void duplicate_live_stroke_id_and_collision_message(void) {
  rm_buf b;
  start_page(&b);
  append_layer(&b, SHOWN, true, rm_crdt_make(1, 12));
  append_stroke(&b, rm_crdt_make(1, 31), SHOWN, RM_TOOL_FINELINER_2);
  append_stroke(&b, rm_crdt_make(1, 31), SHOWN, RM_TOOL_FINELINER_2);

  rm_edit_session* s = open_page(&b);
  rm_item_id pair[2] = {{1, 31}, {1, 999}};
  RM_CHECK(rm_edit_transform_strokes(s, pair, 2, (rm_item_id){1, 50},
                                     (rm_edit_transform){1, 0, 0, 1, 1, 0},
                                     nullptr) == RM_ERR_UNVERIFIED,
           "collect_lines rejects duplicate live id");

  rm_idmap sel = {0};
  RM_CHECK(rm_idmap_set(&sel, rm_crdt_make(1, 31), 0) &&
               rm_idmap_set(&sel, rm_crdt_make(1, 999), 1),
           "build selected idmap");
  rm_crdt_id succ[2] = {0};
  RM_CHECK(rmi_scene_stroke_successors(b.bytes, b.count, &sel, succ, nullptr) ==
               RM_ERR_ITEMS_NOT_FOUND,
           "duplicate live id cannot mask missing second selected id");
  rm_idmap_free(&sel);

  /* Reverse order {missing, dup}: still duplicate-first on transform and
   * missing-first on rmi_scene_stroke_successors (R221). */
  rm_item_id rev_pair[2] = {{1, 999}, {1, 31}};
  RM_CHECK(rm_edit_transform_strokes(s, rev_pair, 2, (rm_item_id){1, 52},
                                     (rm_edit_transform){1, 0, 0, 1, 1, 0},
                                     nullptr) == RM_ERR_UNVERIFIED,
           "reverse {missing, dup} stays RM_ERR_UNVERIFIED on transform");
  rm_idmap rev_sel = {0};
  RM_CHECK(rm_idmap_set(&rev_sel, rm_crdt_make(1, 999), 0) &&
               rm_idmap_set(&rev_sel, rm_crdt_make(1, 31), 1),
           "build reverse selected idmap");
  RM_CHECK(rmi_scene_stroke_successors(b.bytes, b.count, &rev_sel, succ,
                                       nullptr) == RM_ERR_ITEMS_NOT_FOUND,
           "reverse {missing, dup} stays RM_ERR_ITEMS_NOT_FOUND on successors");
  rm_idmap_free(&rev_sel);

  rm_error err = {0};
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 31},
                                 "Collide", &err) == RM_ERR_INVALID_ARGUMENT &&
               err.message[0] != '\0',
           "ID collision sets diagnostic error message");
  rm_edit_close(s);
  rm_buf_free(&b);
}

static void unstamped_visibility_register_is_editable(void) {
  rm_buf b, node = {0}, vis = {0}, group = {0}, gval = {0};
  start_page(&b);
  rm_write_id(&node, 1, SHOWN);
  rm_write_tag(&vis, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(&vis, 1);
  rm_write_block(&node, 3, vis.bytes, vis.count);
  append_block(&b, 2, 1, 2, node.bytes, node.count);

  rm_write_id(&group, 1, rm_crdt_make(0, 1));
  rm_write_id(&group, 2, rm_crdt_make(1, 12));
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&gval, 2);
  rm_write_id(&gval, 2, SHOWN);
  rm_write_block(&group, 6, gval.bytes, gval.count);
  append_block(&b, 4, 1, 1, group.bytes, group.count);

  rm_edit_session* s = open_page(&b);
  RM_CHECK(
      rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 50},
                               false, nullptr) == RM_OK,
      "editing unstamped visibility register succeeds");
  rm_edit_layer* layers = nullptr;
  size_t count = 0;
  RM_CHECK(rm_edit_layers(s, &layers, &count, nullptr) == RM_OK && count == 1 &&
               !layers[0].visible,
           "updated visibility takes effect");
  rm_edit_layers_free(layers);
  rm_edit_close(s);
  rm_buf_free(&b);
  rm_buf_free(&node);
  rm_buf_free(&vis);
  rm_buf_free(&group);
  rm_buf_free(&gval);
}

static void scene_info_raw_size_no_false_collision_and_rename_oom(void) {
  rm_buf b, sinfo = {0}, csize = {0}, amap = {0}, arows = {0}, arow = {0},
            aname = {0}, rt = {0}, items = {0}, lists = {0}, empty_list = {0},
            rt_items = {0}, rt_pos = {0}, settings = {0};
  start_page(&b);
  append_layer(&b, SHOWN, true, rm_crdt_make(1, 12));

  /* SceneInfo: tag 7 raw double width whose leading bytes are 0x1F 0x01 0x4D
   * (unused ID 1:77 if misread) + tag 6 extendedContentRect stamp (1, 66). */
  uint64_t raw_w = 0x40900000004D011Full;
  double width_f64 = 0.0;
  memcpy(&width_f64, &raw_w, sizeof(width_f64));
  rm_write_f64(&csize, width_f64);
  rm_write_f64(&csize, 1872.0);
  rm_write_block(&sinfo, 7, csize.bytes, csize.count);
  uint8_t rect32[32] = {0};
  rm_write_stamped_block(&sinfo, 6, rm_crdt_make(1, 66), rect32,
                         sizeof(rect32));
  append_block(&b, 13, 0, 1, sinfo.bytes, sinfo.count);

  /* AttachmentMap: tag 2 LWW tail register stamp (1, 67). */
  uint8_t akey[16] = {9};
  rm_buf_append(&arow, akey, sizeof(akey));
  rm_write_string(&aname, "att.png");
  rm_write_stamped_block(&arow, 1, rm_crdt_make(1, 65), aname.bytes,
                         aname.count);
  uint8_t tail2[2] = {0x11, 0x00};
  rm_write_stamped_block(&arow, 2, rm_crdt_make(1, 67), tail2, sizeof(tail2));
  rm_write_varuint(&arows, 1);
  rm_write_block(&arows, 0, arow.bytes, arow.count);
  rm_write_block(&amap, 1, arows.bytes, arows.count);
  append_block(&b, 14, 1, 1, amap.bytes, amap.count);

  /* RootText TAG_TEXT_SETTINGS: unknown tag 2 carrying raw bytes 0x1F 0x01 0x4E
   * (unused ID 1:78 if misread as a stamped setting). */
  rm_write_varuint(&empty_list, 0);
  rm_write_block(&rt_items, 1, empty_list.bytes, empty_list.count);
  rm_write_block(&lists, 1, rt_items.bytes, rt_items.count);
  rm_write_block(&lists, 2, empty_list.bytes, empty_list.count);
  rm_write_id(&rt, 1, RM_CRDT_NONE);
  rm_write_block(&rt, 2, lists.bytes, lists.count);
  rm_write_f64(&rt_pos, 0.0);
  rm_write_f64(&rt_pos, 0.0);
  rm_write_block(&rt, 3, rt_pos.bytes, rt_pos.count);
  rm_write_tag(&rt, 4, RM_TAG_BYTE4);
  rm_write_f32(&rt, 936.0f);
  uint8_t opaque_setting[8] = {0x1F, 0x01, 0x4E, 0, 0, 0, 0, 0};
  rm_write_block(&settings, 2, opaque_setting, sizeof(opaque_setting));
  rm_write_block(&rt, 5, settings.bytes, settings.count);
  append_block(&b, 7, 1, 1, rt.bytes, rt.count);

  rm_edit_session* s = open_page(&b);
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 66},
                                 "CollideSInfo6",
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
           "SceneInfo tag 6 extendedContentRect stamp collision is rejected");
  RM_CHECK(
      rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 67},
                            "CollideAtt2", nullptr) == RM_ERR_INVALID_ARGUMENT,
      "AttachmentMap tag 2 LWW stamp collision is rejected");
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 77},
                                 "Renamed", nullptr) == RM_OK,
           "raw SceneInfo tag 7 bytes do not cause false ID collision");
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 78},
                                 "RenamedSetting", nullptr) == RM_OK,
           "opaque RootText setting tag 2 does not cause false ID collision");

  /* Inject OOM directly in rmi_scene_node_state and across the full rename path
   * to verify RM_ERR_OUT_OF_MEMORY propagation at every allocation site. */
  size_t cur_size = 0;
  const uint8_t* cur_bytes = rm_edit_bytes(s, &cur_size);
  bool found_node = false;
  char* node_name = nullptr;
  rm_test_arena_fail_after(0);
  rm_status nst =
      rmi_scene_node_state(cur_bytes, cur_size, rm_item_id_of(SHOWN),
                           &found_node, nullptr, &node_name);
  bool node_did_fail = rm_test_arena_did_fail();
  rm_test_arena_fail_after(SIZE_MAX);
  free(node_name);
  RM_CHECK(node_did_fail && nst == RM_ERR_OUT_OF_MEMORY,
           "rmi_scene_node_state propagates RM_ERR_OUT_OF_MEMORY directly");

  size_t oom_count = 0;
  for (size_t k = 0; k <= 8; ++k) {
    rm_test_arena_fail_after(k);
    rm_error err = {0};
    rm_status st = rm_edit_set_node_name(s, rm_item_id_of(SHOWN),
                                         (rm_item_id){1, 79}, "Renamed2", &err);
    bool failed = rm_test_arena_did_fail();
    rm_test_arena_fail_after(SIZE_MAX);
    if (failed) {
      RM_CHECK(st == RM_ERR_OUT_OF_MEMORY,
               "rename arena OOM propagates RM_ERR_OUT_OF_MEMORY at k=%zu (got "
               "%s)",
               k, rm_status_string(st));
      oom_count += 1;
    }
  }
  RM_CHECK(oom_count >= 2,
           "injected OOM exercised both location and node-state steps (%zu)",
           oom_count);

  rm_edit_close(s);
  rm_buf_free(&b);
  rm_buf_free(&sinfo);
  rm_buf_free(&csize);
  rm_buf_free(&amap);
  rm_buf_free(&arows);
  rm_buf_free(&arow);
  rm_buf_free(&aname);
  rm_buf_free(&rt);
  rm_buf_free(&items);
  rm_buf_free(&lists);
  rm_buf_free(&empty_list);
  rm_buf_free(&rt_items);
  rm_buf_free(&rt_pos);
  rm_buf_free(&settings);
}

static bool buf_has_bytes(const uint8_t* hay, size_t hlen, const void* needle,
                          size_t nlen) {
  if (nlen == 0) return true;
  if (hlen < nlen) return false;
  for (size_t i = 0; i + nlen <= hlen; ++i) {
    if (memcmp(hay + i, needle, nlen) == 0) return true;
  }
  return false;
}

static void a2_deleted_group_item_reserves_only_valid_group_child(void) {
  rm_buf b, del = {0}, val = {0};
  start_page(&b);
  append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));

  /* Deleted 0x04 with kind=2 + tag 2 child (1:77) reserves 1:77. */
  rm_write_id(&del, 1, rm_crdt_make(0, 1));
  rm_write_id(&del, 2, rm_crdt_make(1, 15));
  rm_write_id(&del, 3, RM_CRDT_NONE);
  rm_write_id(&del, 4, RM_CRDT_NONE);
  rm_write_tag(&del, 5, RM_TAG_BYTE4);
  rm_write_u32(&del, 1);
  rm_buf_append_byte(&val, 2);
  rm_write_id(&val, 2, rm_crdt_make(1, 77));
  rm_write_block(&del, 6, val.bytes, val.count);
  append_block(&b, 4, 1, 1, del.bytes, del.count);

  /* Deleted 0x04 with kind=9 != RM_VALUE_GROUP_ITEM carrying 1:78 bytes must
   * NOT reserve 1:78, nor should opaque garbage bytes inside tag 6. */
  del.count = 0;
  val.count = 0;
  rm_write_id(&del, 1, rm_crdt_make(0, 1));
  rm_write_id(&del, 2, rm_crdt_make(1, 16));
  rm_write_id(&del, 3, RM_CRDT_NONE);
  rm_write_id(&del, 4, RM_CRDT_NONE);
  rm_write_tag(&del, 5, RM_TAG_BYTE4);
  rm_write_u32(&del, 1);
  rm_buf_append_byte(&val, 9);
  rm_write_id(&val, 2, rm_crdt_make(1, 78));
  rm_write_block(&del, 6, val.bytes, val.count);
  append_block(&b, 4, 1, 1, del.bytes, del.count);

  del.count = 0;
  rm_write_id(&del, 1, rm_crdt_make(0, 1));
  rm_write_id(&del, 2, rm_crdt_make(1, 17));
  rm_write_id(&del, 3, RM_CRDT_NONE);
  rm_write_id(&del, 4, RM_CRDT_NONE);
  rm_write_tag(&del, 5, RM_TAG_BYTE4);
  rm_write_u32(&del, 1);
  const uint8_t junk[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  rm_write_block(&del, 6, junk, sizeof junk);
  append_block(&b, 4, 1, 1, del.bytes, del.count);

  rm_edit_session* s = open_page(&b);
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke st =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
  RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 77},
                                 (rm_item_id){0}, (rm_item_id){0}, &st,
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
           "deleted kind=2 group reserves tag 2 child (1:77)");
  RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 78},
                                 (rm_item_id){0}, (rm_item_id){0}, &st,
                                 nullptr) == RM_OK,
           "deleted non-group kind=9 does not reserve false child (1:78)");
  rm_edit_close(s);
  rm_buf_free(&val);
  rm_buf_free(&del);
  rm_buf_free(&b);
}

static void a4_editor_limits_palette_shader_cut_transforms_and_cycles(void) {
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke good =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);

  /* 1) rm_edit_insert_stroke palette/ARGB/thickness & exact L / L-1 limit
   * precedence (R67, R104, R108, R111, R134, R168, R172). */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_edit_session* s = open_page(&b);

    rm_draft_stroke hl =
        rm_draft_stroke_make(RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHT, &pt, 1);
    hl.has_argb = true;
    hl.argb = 0x55223344u;
    RM_CHECK(hl.thickness == 1.0, "highlighter default thickness is 1.0");
    rm_draft_stroke bad_t = hl;
    bad_t.thickness = 2.0;
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 40},
                                   (rm_item_id){0}, (rm_item_id){0}, &bad_t,
                                   nullptr) == RM_ERR_INVALID_ARGUMENT,
             "insert_stroke refuses highlighter thickness 2.0");
    rm_draft_stroke bad_s =
        rm_draft_stroke_make(RM_TOOL_SHADER, RM_COLOR_BLACK, &pt, 1);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 41},
                                   (rm_item_id){0}, (rm_item_id){0}, &bad_s,
                                   nullptr) == RM_ERR_INVALID_ARGUMENT,
             "insert_stroke refuses shader without HIGHLIGHT+ARGB");

    rm_draft_point pts[60];
    for (size_t i = 0; i < 60; ++i) pts[i] = rm_draft_point_make((float)i, 0);
    hl.points = pts;
    hl.point_count = 60;
    rm_buf lb = {0};
    rm_write_line_item(&lb, &hl, SHOWN, rm_crdt_make(1, 50), RM_CRDT_NONE,
                       RM_CRDT_NONE);
    size_t L = lb.count;
    rm_buf_free(&lb);

    rm_test_block_limit(L - 1u);
    pts[0].x = NAN;
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 50},
                                   (rm_item_id){0}, (rm_item_id){0}, &hl,
                                   nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
             "insert_stroke block limit precedes NaN coord check at L-1");
    pts[0].x = 0.0f;
    rm_test_block_limit(L);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 50},
                                   (rm_item_id){0}, (rm_item_id){0}, &hl,
                                   nullptr) == RM_OK,
             "insert_stroke succeeds at exact block limit L=%zu", L);
    rm_test_block_limit(UINT32_MAX);
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* 2) add_layer long name, rename/text-replace enclosing block limit, and
   * piece() transform/cut limit (R89, R99, R133). */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_SHADER);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(s, 0, "Seed", (rm_item_id){1, 100}, nullptr) == RM_OK,
        "seed undo/redo");
    RM_CHECK(rm_edit_undo(s), "create redo");

    char long_name[200];
    memset(long_name, 'n', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    rm_test_block_limit(128);
    RM_CHECK(rm_edit_add_layer(s, 0, long_name, (rm_item_id){1, 200},
                               nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
             "add_layer long name exceeding lowered limit returns TOO_MANY");
    RM_CHECK(rm_edit_redo(s), "redo intact after add_layer limit failure");
    RM_CHECK(rm_edit_undo(s), "undo back");

    rm_test_block_limit(48);
    RM_CHECK(
        rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 210},
                              long_name, nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
        "rename exceeding lowered limit returns TOO_MANY");
    RM_CHECK(rm_edit_replace_text(s, 0, 0, long_name, (rm_item_id){1, 220},
                                  nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
             "replace_text exceeding lowered limit returns TOO_MANY");

    /* Existing stroke (size > 32) transformed/cut under tight block limit. */
    rm_test_block_limit(50);
    RM_CHECK(rm_edit_transform_strokes(s, &(rm_item_id){1, 30}, 1,
                                       (rm_item_id){1, 230},
                                       (rm_edit_transform){1, 0, 0, 1, 1, 0},
                                       nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
             "transform piece() exceeding block limit returns TOO_MANY");
    RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 30}, (rm_item_id){1, 240}, 0,
                                1, nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
             "cut piece() exceeding block limit returns TOO_MANY");
    rm_test_block_limit(UINT32_MAX);
    rm_page* orig_p = parse_session_bytes(s);
    RM_CHECK(orig_p != nullptr && orig_p->stroke_count == 1,
             "original stroke intact");
    rm_page_free(orig_p);
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* 3) Shader middle cut with explicit tag 9 = RM_CRDT_NONE inherits orig ID
   * (R135), 2x magnification preserves tag 3 + DBL_MAX tag 3 + v1 width
   * overflow (R94, R115, R123). */
  {
    rm_buf b, val = {0}, pts = {0}, line = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    for (int i = 0; i < 4; ++i) {
      rm_write_f32(&pts, (float)i * 10.0f);
      rm_write_f32(&pts, 0.0f);
      rm_write_u16(&pts, 0);
      rm_write_u16(&pts, 4);
      rm_buf_append_byte(&pts, 0);
      rm_buf_append_byte(&pts, 6);
    }
    rm_buf_append_byte(&val, 0x03);
    rm_write_tag(&val, 1, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_TOOL_SHADER);
    rm_write_tag(&val, 2, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_HIGHLIGHT);
    rm_write_tag(&val, 3, RM_TAG_BYTE8);
    rm_write_f64(&val, 2.5);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_f32(&val, 0.0f);
    rm_write_block(&val, 5, pts.bytes, pts.count);
    rm_write_id(&val, 6, RM_CRDT_NONE);
    rm_write_tag(&val, 8, RM_TAG_BYTE4);
    rm_write_u32(&val, 0x40112233u);
    rm_write_id(&val, 9, RM_CRDT_NONE); /* explicit RM_CRDT_NONE sourceId */

    rm_write_id(&line, 1, SHOWN);
    rm_write_id(&line, 2, rm_crdt_make(1, 30));
    rm_write_id(&line, 3, RM_CRDT_NONE);
    rm_write_id(&line, 4, RM_CRDT_NONE);
    rm_write_tag(&line, 5, RM_TAG_BYTE4);
    rm_write_u32(&line, 0);
    rm_write_block(&line, 6, val.bytes, val.count);
    append_block(&b, 5, 2, 2, line.bytes, line.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 30}, (rm_item_id){1, 50}, 1,
                                2, nullptr) == RM_OK,
             "middle cut of shader with explicit RM_CRDT_NONE source");
    rm_page* cp = parse_session_bytes(s);
    RM_CHECK(
        cp && cp->stroke_count == 2 && cp->strokes[0].has_source_id &&
            rm_item_id_equal(cp->strokes[0].source_id, (rm_item_id){1, 30}) &&
            cp->strokes[1].has_source_id &&
            rm_item_id_equal(cp->strokes[1].source_id, (rm_item_id){1, 30}),
        "both surviving shader pieces inherit original id (R135)");
    rm_page_free(cp);
    RM_CHECK(rm_edit_undo(s), "undo shader cut");

    RM_CHECK(rm_edit_transform_stroke(
                 s, (rm_item_id){1, 30}, (rm_item_id){1, 60},
                 (rm_edit_transform){2, 0, 0, 2, 0, 0}, nullptr) == RM_OK,
             "2x scale succeeds");
    rm_page* sp = parse_session_bytes(s);
    RM_CHECK(sp && sp->stroke_count == 1 &&
                 sp->strokes[0].thickness_scale == 2.5 &&
                 sp->strokes[0].points[0].width == 8.0f,
             "2x scale preserves tag 3=2.5 and doubles sample width (R94)");
    rm_page_free(sp);
    rm_edit_close(s);

    /* DBL_MAX tag 3 preserved on 2x scale (R115). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    val.count = 0;
    line.count = 0;
    rm_buf_append_byte(&val, 0x03);
    rm_write_tag(&val, 1, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_TOOL_FINELINER_2);
    rm_write_tag(&val, 2, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_BLACK);
    rm_write_tag(&val, 3, RM_TAG_BYTE8);
    rm_write_f64(&val, DBL_MAX);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_f32(&val, 0.0f);
    rm_write_block(&val, 5, pts.bytes, pts.count);
    rm_write_id(&line, 1, SHOWN);
    rm_write_id(&line, 2, rm_crdt_make(1, 30));
    rm_write_id(&line, 3, RM_CRDT_NONE);
    rm_write_id(&line, 4, RM_CRDT_NONE);
    rm_write_tag(&line, 5, RM_TAG_BYTE4);
    rm_write_u32(&line, 0);
    rm_write_block(&line, 6, val.bytes, val.count);
    append_block(&b, 5, 2, 2, line.bytes, line.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_transform_stroke(
                 s, (rm_item_id){1, 30}, (rm_item_id){1, 70},
                 (rm_edit_transform){2, 0, 0, 2, 0, 0}, nullptr) == RM_OK,
             "DBL_MAX tag 3 survives 2x transform");
    rm_page* dp = parse_session_bytes(s);
    RM_CHECK(dp && dp->stroke_count == 1 &&
                 dp->strokes[0].thickness_scale == DBL_MAX &&
                 dp->strokes[0].points[0].width == 8.0f,
             "DBL_MAX tag 3 exact and sample width doubled (R115)");
    rm_page_free(dp);
    rm_edit_close(s);

    /* v1 width = FLT_MAX / 4.0f transformed 2x -> RM_ERR_INVALID_ARGUMENT
     * (R123). */
    rm_buf v1b, v1val = {0}, v1pts = {0}, v1line = {0};
    start_page(&v1b);
    append_layer(&v1b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_f32(&v1pts, 0.0f);
    rm_write_f32(&v1pts, 0.0f);
    rm_write_u32(&v1pts, 0);
    rm_write_u32(&v1pts, 0);
    rm_write_f32(&v1pts, FLT_MAX / 4.0f);
    rm_write_f32(&v1pts, 0.0f);
    rm_buf_append_byte(&v1val, 0x03);
    rm_write_tag(&v1val, 1, RM_TAG_BYTE4);
    rm_write_u32(&v1val, RM_TOOL_FINELINER_2);
    rm_write_tag(&v1val, 2, RM_TAG_BYTE4);
    rm_write_u32(&v1val, RM_COLOR_BLACK);
    rm_write_tag(&v1val, 3, RM_TAG_BYTE8);
    rm_write_f64(&v1val, 1.0);
    rm_write_tag(&v1val, 4, RM_TAG_BYTE4);
    rm_write_f32(&v1val, 0.0f);
    rm_write_block(&v1val, 5, v1pts.bytes, v1pts.count);
    rm_write_id(&v1line, 1, SHOWN);
    rm_write_id(&v1line, 2, rm_crdt_make(1, 30));
    rm_write_id(&v1line, 3, RM_CRDT_NONE);
    rm_write_id(&v1line, 4, RM_CRDT_NONE);
    rm_write_tag(&v1line, 5, RM_TAG_BYTE4);
    rm_write_u32(&v1line, 0);
    rm_write_block(&v1line, 6, v1val.bytes, v1val.count);
    append_block(&v1b, 5, 1, 1, v1line.bytes, v1line.count);
    s = open_page(&v1b);
    RM_CHECK(
        rm_edit_transform_stroke(s, (rm_item_id){1, 30}, (rm_item_id){1, 70},
                                 (rm_edit_transform){2, 0, 0, 2, 0, 0},
                                 nullptr) == RM_ERR_INVALID_ARGUMENT,
        "v1 width FLT_MAX/4 scaled 2x refused INVALID_ARGUMENT (R123)");
    rm_edit_close(s);
    rm_buf bufs[] = {b, val, pts, line, v1b, v1val, v1pts, v1line};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* 4) Mixed 0x03 + 0x05 shared ID (R144) & erase target inside 0x01-removed
   * subtree (R214). */
  {
    rm_buf b, g = {0}, rects = {0}, tbuf = {0}, gval = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    rm_write_varuint(&rects, 1);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 10);
    rm_write_f64(&rects, 10);
    rm_write_string(&tbuf, "hi");
    rm_buf_append_byte(&gval, 0x01);
    rm_write_tag(&gval, 4, RM_TAG_BYTE4);
    rm_write_u32(&gval, RM_COLOR_YELLOW);
    rm_write_block(&gval, 5, tbuf.bytes, tbuf.count);
    rm_write_block(&gval, 6, rects.bytes, rects.count);
    rm_write_id(&g, 1, SHOWN);
    rm_write_id(&g, 2, rm_crdt_make(1, 30)); /* collides with stroke! */
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 0);
    rm_write_block(&g, 6, gval.bytes, gval.count);
    append_block(&b, 3, 1, 1, g.bytes, g.count);

    rm_item_id* hids = nullptr;
    size_t hcnt = 0;
    RM_CHECK(rm_highlighter_items(b.bytes, b.count, &hids, &hcnt, nullptr) ==
                     RM_OK &&
                 hcnt == 1 && hids[0].author == 1 && hids[0].counter == 30,
             "mixed 0x03+0x05 returns glyph highlighter ID");
    rm_item_ids_free(hids);
    uint8_t* out_e = nullptr;
    size_t out_esz = 0;
    RM_CHECK(rm_erase_items(b.bytes, b.count, &(rm_item_id){1, 30}, 1, &out_e,
                            &out_esz, nullptr) == RM_ERR_UNVERIFIED,
             "rm_erase_items refuses duplicate 0x03+0x05 ID");
    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_erase(s, &(rm_item_id){1, 30}, 1, nullptr) == RM_ERR_UNVERIFIED,
        "rm_edit_erase refuses duplicate 0x03+0x05 ID");
    rm_edit_close(s);

    /* R214: 0x01-removed target alone and mixed [live, removed]. */
    b.count = RM_HEADER_LENGTH;
    append_two_layers(&b, true);
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    append_stroke(&b, rm_crdt_make(1, 31), HIDDEN, RM_TOOL_FINELINER_2);
    append_removal(&b, HIDDEN);
    RM_CHECK(rm_erase_items(b.bytes, b.count, &(rm_item_id){1, 31}, 1, &out_e,
                            &out_esz, nullptr) == RM_ERR_ITEMS_NOT_FOUND,
             "rm_erase_items refuses target in 0x01-removed subtree");
    rm_item_id mix[2] = {{1, 30}, {1, 31}};
    RM_CHECK(rm_erase_items(b.bytes, b.count, mix, 2, &out_e, &out_esz,
                            nullptr) == RM_ERR_ITEMS_NOT_FOUND,
             "rm_erase_items refuses mixed [live, removed] atomically");
    s = open_page(&b);
    RM_CHECK(rm_edit_erase(s, mix, 2, nullptr) == RM_ERR_ITEMS_NOT_FOUND,
             "rm_edit_erase refuses mixed [live, removed] atomically");
    rm_edit_close(s);
    rm_buf_free(&tbuf);
    rm_buf_free(&gval);
    rm_buf_free(&rects);
    rm_buf_free(&g);
    rm_buf_free(&b);
  }

  /* 5) Live stroke sharing ID with tombstone OR 0x01-removed copy in BOTH
   * block orders + unrepresentable tombstone + hidden-layer sibling + cycle
   * eraser/image (R217, R220, R224, R225, R229, R230, R232, R35-1). */
  for (int removed_copy = 0; removed_copy < 2; ++removed_copy) {
    for (int inactive_first = 0; inactive_first < 2; ++inactive_first) {
      rm_buf b, inact = {0};
      start_page(&b);
      append_two_layers(&b, false);
      if (removed_copy) append_removal(&b, HIDDEN);
      if (removed_copy) {
        append_stroke(&inact, rm_crdt_make(1, 30), HIDDEN, RM_TOOL_FINELINER_2);
      } else {
        rm_buf t = {0};
        rm_write_id(&t, 1, SHOWN);
        rm_write_id(&t, 2, rm_crdt_make(1, 30));
        rm_write_id(&t, 3, RM_CRDT_NONE);
        rm_write_id(&t, 4, RM_CRDT_NONE);
        rm_write_tag(&t, 5, RM_TAG_BYTE4);
        rm_write_u32(&t, 1);
        append_block(&inact, 5, 2, 2, t.bytes, t.count);
        rm_buf_free(&t);
      }
      if (inactive_first) rm_buf_append(&b, inact.bytes, inact.count);
      append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
      if (!inactive_first) rm_buf_append(&b, inact.bytes, inact.count);
      append_stroke(&b, rm_crdt_make(1, 31), SHOWN, RM_TOOL_FINELINER_2);

      rm_edit_session* s = open_page(&b);
      RM_CHECK(rm_edit_transform_stroke(
                   s, (rm_item_id){1, 30}, (rm_item_id){1, 50},
                   (rm_edit_transform){1, 0, 0, 1, 2, 0}, nullptr) == RM_OK,
               "transform with duplicate inactive copy rem=%d ord=%d",
               removed_copy, inactive_first);
      size_t sz = 0;
      const uint8_t* eb = rm_edit_bytes(s, &sz);
      rm_page* p = parse(eb, sz);
      RM_CHECK(p && p->stroke_count == 2 && p->strokes[0].id.counter == 31 &&
                   p->strokes[1].id.counter == 50 &&
                   buf_has_bytes(eb, sz, inact.bytes, inact.count),
               "transformed preserves inactive copy & [31, 50] order");
      rm_page_free(p);
      RM_CHECK(rm_edit_undo(s), "undo transform");
      RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 30}, (rm_item_id){1, 60},
                                  0, 1, nullptr) == RM_OK,
               "cut with duplicate inactive copy succeeds");
      rm_edit_close(s);
      rm_buf_free(&inact);
      rm_buf_free(&b);
    }
  }

  /* Unrepresentable same-parent tombstone after sibling B refuses with
   * RM_ERR_UNVERIFIED (R220); hidden-layer transform/middle-cut + reveal (R230,
   * R35-1); middle cut on last stroke (R224); disconnected 2-group cycle with
   * eraser B / image I (R225, R229, R232). */
  {
    /* Unrepresentable same-parent tombstone: A(1:30, left=0) -> B(1:31,
     * left=1:30) -> T(1:30, deleted=1, left=1:31, right=0); fresh(1:50,
     * left=1:30, right=1:31) follows T after B in positions ->
     * RM_ERR_UNVERIFIED. */
    rm_buf b, unrep = {0};
    start_page(&b);
    append_two_layers(&b, true);
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    rm_buf line_b = {0};
    rm_draft_stroke bs =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
    rm_write_line_item(&line_b, &bs, SHOWN, rm_crdt_make(1, 31),
                       rm_crdt_make(1, 30), RM_CRDT_NONE);
    append_block(&b, 5, 2, 2, line_b.bytes, line_b.count);
    rm_write_id(&unrep, 1, SHOWN);
    rm_write_id(&unrep, 2, rm_crdt_make(1, 30));
    rm_write_id(&unrep, 3, rm_crdt_make(1, 31));
    rm_write_id(&unrep, 4, RM_CRDT_NONE);
    rm_write_tag(&unrep, 5, RM_TAG_BYTE4);
    rm_write_u32(&unrep, 1);
    append_block(&b, 5, 2, 2, unrep.bytes, unrep.count);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_add_layer(s, 0, "RedoSeed", (rm_item_id){1, 500},
                               nullptr) == RM_OK &&
                 rm_edit_undo(s),
             "seed redo branch");
    rm_status st_v = rm_edit_transform_stroke(
        s, (rm_item_id){1, 30}, (rm_item_id){1, 50},
        (rm_edit_transform){1, 0, 0, 1, 1, 0}, nullptr);
    RM_CHECK(st_v == RM_ERR_UNVERIFIED,
             "unrepresentable visible tombstone order refuses UNVERIFIED (got "
             "%s) (R220)",
             rm_status_string(st_v));
    RM_CHECK(rm_edit_redo(s), "redo intact after unrepresentable refusal");
    rm_edit_close(s);

    /* Hidden-layer middle cut + reveal + hidden unrepresentable tombstone
     * (R230, R35-1). */
    b.count = RM_HEADER_LENGTH;
    append_two_layers(&b, true); /* HIDDEN is hidden */
    rm_draft_point hpts[3] = {rm_draft_point_make(0, 0),
                              rm_draft_point_make(10, 0),
                              rm_draft_point_make(20, 0)};
    rm_draft_stroke hs =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, hpts, 3);
    line_b.count = 0;
    rm_write_line_item(&line_b, &hs, HIDDEN, rm_crdt_make(1, 40), RM_CRDT_NONE,
                       RM_CRDT_NONE);
    append_block(&b, 5, 2, 2, line_b.bytes, line_b.count);
    line_b.count = 0;
    rm_write_line_item(&line_b, &bs, HIDDEN, rm_crdt_make(1, 41),
                       rm_crdt_make(1, 40), RM_CRDT_NONE);
    append_block(&b, 5, 2, 2, line_b.bytes, line_b.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 40}, (rm_item_id){1, 60}, 1,
                                1, nullptr) == RM_OK,
             "hidden-layer middle cut with sibling succeeds");
    RM_CHECK(
        rm_edit_set_node_visible(s, rm_item_id_of(HIDDEN), (rm_item_id){1, 100},
                                 true, nullptr) == RM_OK,
        "reveal hidden layer");
    rm_page* rp = parse_session_bytes(s);
    RM_CHECK(rp && rp->stroke_count == 3 && rp->strokes[0].id.counter == 60 &&
                 rp->strokes[1].id.counter == 61 &&
                 rp->strokes[2].id.counter == 41,
             "revealed hidden-layer pieces preserve [60, 61, 41] order (R230)");
    rm_page_free(rp);
    rm_edit_close(s);

    /* Unrepresentable hidden-layer tombstone -> RM_ERR_UNVERIFIED (R230). */
    b.count = RM_HEADER_LENGTH;
    append_two_layers(&b, true);
    append_stroke(&b, rm_crdt_make(1, 30), HIDDEN, RM_TOOL_FINELINER_2);
    line_b.count = 0;
    rm_write_line_item(&line_b, &bs, HIDDEN, rm_crdt_make(1, 31),
                       rm_crdt_make(1, 30), RM_CRDT_NONE);
    append_block(&b, 5, 2, 2, line_b.bytes, line_b.count);
    unrep.count = 0;
    rm_write_id(&unrep, 1, HIDDEN);
    rm_write_id(&unrep, 2, rm_crdt_make(1, 30));
    rm_write_id(&unrep, 3, rm_crdt_make(1, 31));
    rm_write_id(&unrep, 4, RM_CRDT_NONE);
    rm_write_tag(&unrep, 5, RM_TAG_BYTE4);
    rm_write_u32(&unrep, 1);
    append_block(&b, 5, 2, 2, unrep.bytes, unrep.count);
    s = open_page(&b);
    rm_status st_h = rm_edit_transform_stroke(
        s, (rm_item_id){1, 30}, (rm_item_id){1, 50},
        (rm_edit_transform){1, 0, 0, 1, 1, 0}, nullptr);
    RM_CHECK(st_h == RM_ERR_UNVERIFIED,
             "unrepresentable hidden-layer tombstone refuses UNVERIFIED (got "
             "%s) (R230)",
             rm_status_string(st_h));
    rm_edit_close(s);

    /* Last-stroke middle cut succeeds (R224). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    line_b.count = 0;
    rm_write_line_item(&line_b, &hs, SHOWN, rm_crdt_make(1, 40), RM_CRDT_NONE,
                       RM_CRDT_NONE);
    append_block(&b, 5, 2, 2, line_b.bytes, line_b.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_cut_stroke(s, (rm_item_id){1, 40}, (rm_item_id){1, 60}, 1,
                                1, nullptr) == RM_OK,
             "middle cut on sole/last stroke succeeds (R224)");
    rp = parse_session_bytes(s);
    RM_CHECK(rp && rp->stroke_count == 2 && rp->strokes[0].id.counter == 60 &&
                 rp->strokes[1].id.counter == 61,
             "last stroke middle cut produces [60, 61]");
    rm_page_free(rp);
    rm_edit_close(s);

    /* Disconnected 2-group cycle: A(1:300) followed by (i) eraser B(1:301,
     * left=A) -> lower fresh(1:250) flips order -> RM_ERR_UNVERIFIED for BOTH
     * transform(A) and insert(left=A, right=B); (ii) valid attachment image
     * I(1:301, left=A, strokes_before=1) -> identity transform with lower
     * fresh(1:250) flips strokes_before -> RM_ERR_UNVERIFIED (R225, R229,
     * R232). */
    rm_crdt_id g1 = rm_crdt_make(1, 200), g2 = rm_crdt_make(1, 201);
    rm_draft_stroke er =
        rm_draft_stroke_make(RM_TOOL_ERASER, RM_COLOR_BLACK, &pt, 1);
    rm_buf bcyc, eb_er = {0};
    start_page(&bcyc);
    append_layer(&bcyc, SHOWN, false, rm_crdt_make(1, 12));
    append_nested_group(&bcyc, g1, rm_crdt_make(1, 204), g2);
    append_nested_group(&bcyc, g2, rm_crdt_make(1, 205), g1);
    append_stroke(&bcyc, rm_crdt_make(1, 300), g1, RM_TOOL_FINELINER_2);
    rm_write_line_item(&eb_er, &er, g1, rm_crdt_make(1, 301),
                       rm_crdt_make(1, 300), RM_CRDT_NONE);
    append_block(&bcyc, 5, 2, 2, eb_er.bytes, eb_er.count);
    rm_buf_free(&eb_er);
    s = open_page(&bcyc);
    RM_CHECK(rm_edit_add_layer(s, 0, "CycSeed", (rm_item_id){1, 500},
                               nullptr) == RM_OK &&
                 rm_edit_undo(s),
             "cycle seed redo");
    RM_CHECK(
        rm_edit_transform_stroke(s, (rm_item_id){1, 300}, (rm_item_id){1, 250},
                                 (rm_edit_transform){1, 0, 0, 1, 1, 0},
                                 nullptr) == RM_ERR_UNVERIFIED,
        "cycle A+eraserB lower-id transform refuses UNVERIFIED (R225)");
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(g1), (rm_item_id){1, 250},
                                   (rm_item_id){1, 300}, (rm_item_id){1, 301},
                                   &good, nullptr) == RM_ERR_UNVERIFIED,
             "cycle insert_stroke left=A right=B lower-id refuses UNVERIFIED "
             "(R232)");
    RM_CHECK(rm_edit_redo(s), "redo intact after cycle refusals");
    rm_edit_close(s);
    rm_buf_free(&bcyc);

    /* (ii) Disconnected 2-group cycle with overlapping attachment image I. */
    rm_buf bim, amap = {0}, arows = {0}, arow = {0}, aname = {0}, iverts = {0},
                iidx = {0}, ival = {0}, irec = {0};
    uint8_t ikey[16] = {7};
    start_page(&bim);
    append_layer(&bim, SHOWN, false, rm_crdt_make(1, 12));
    rm_buf_append(&arow, ikey, 16);
    rm_write_string(&aname, "cyc.png");
    rm_write_stamped_block(&arow, 1, rm_crdt_make(1, 10), aname.bytes,
                           aname.count);
    rm_write_varuint(&arows, 1);
    rm_write_block(&arows, 0, arow.bytes, arow.count);
    rm_write_block(&amap, 1, arows.bytes, arows.count);
    append_block(&bim, 14, 1, 1, amap.bytes, amap.count);
    append_nested_group(&bim, g1, rm_crdt_make(1, 204), g2);
    append_nested_group(&bim, g2, rm_crdt_make(1, 205), g1);
    append_stroke(&bim, rm_crdt_make(1, 300), g1, RM_TOOL_FINELINER_2);
    rm_write_varuint(&iverts, 16);
    for (int i = 0; i < 4; ++i) {
      rm_write_f32(&iverts, i == 1 || i == 2 ? 10.0f : 0.0f);
      rm_write_f32(&iverts, i >= 2 ? 10.0f : 0.0f);
      rm_write_f32(&iverts, i == 1 || i == 2 ? 1.0f : 0.0f);
      rm_write_f32(&iverts, i >= 2 ? 1.0f : 0.0f);
    }
    rm_write_varuint(&iidx, 6);
    const uint32_t quad[6] = {0, 1, 2, 2, 3, 0};
    for (int q = 0; q < 6; ++q) rm_write_u32(&iidx, quad[q]);
    rm_buf_append_byte(&ival, 0x07);
    rm_write_block(&ival, 1, ikey, 16);
    rm_write_block(&ival, 3, iverts.bytes, iverts.count);
    rm_write_block(&ival, 4, iidx.bytes, iidx.count);
    rm_write_id(&irec, 1, g1);
    rm_write_id(&irec, 2, rm_crdt_make(1, 301));
    rm_write_id(&irec, 3, rm_crdt_make(1, 300));
    rm_write_id(&irec, 4, RM_CRDT_NONE);
    rm_write_tag(&irec, 5, RM_TAG_BYTE4);
    rm_write_u32(&irec, 0);
    rm_write_block(&irec, 6, ival.bytes, ival.count);
    append_block(&bim, 15, 2, 2, irec.bytes, irec.count);
    s = open_page(&bim);
    RM_CHECK(rm_edit_add_layer(s, 0, "ImgSeed", (rm_item_id){1, 500},
                               nullptr) == RM_OK &&
                 rm_edit_undo(s),
             "image cycle redo seed");
    RM_CHECK(
        rm_edit_transform_stroke(s, (rm_item_id){1, 300}, (rm_item_id){1, 250},
                                 (rm_edit_transform){1, 0, 0, 1, 0, 0},
                                 nullptr) == RM_ERR_UNVERIFIED,
        "cycle image strokes_before flip refuses UNVERIFIED (R229)");
    RM_CHECK(rm_edit_redo(s), "redo intact after image cycle refusal");
    rm_edit_close(s);
    rm_buf im_bufs[] = {bim,  amap, arows, arow,   aname, iverts,
                        iidx, ival, irec,  line_b, unrep, b};
    for (size_t i = 0; i < sizeof im_bufs / sizeof *im_bufs; ++i)
      rm_buf_free(&im_bufs[i]);
  }
}

static void a5_node_edit_truncations_and_unstamped_registers(void) {
  /* (a) valid hidden vis stamp 10 + truncated vis stamp 30, plus (b) valid
   * name stamp 10 + truncated name string varint stamp 30, plus (c) short
   * LENGTH4 & BYTE4 trailing fields, (d) tag 3 BYTE1 before tag 3 LENGTH4, and
   * (e) unstamped TAG_NODE_NAME vs explicit {0, 0} stamp (R74, R95, R141,
   * R154, R163, R215). */
  rm_buf b, node = {0}, grp = {0}, gval = {0};
  start_page(&b);
  rm_write_id(&node, 1, SHOWN);
  rm_write_stamped_u8(&node, 3, rm_crdt_make(1, 10), 0); /* hidden @ 10 */
  rm_buf trunc_v = {0};
  rm_write_id(&trunc_v, 1, rm_crdt_make(1, 30));
  rm_write_tag(&trunc_v, 2, RM_TAG_BYTE1); /* missing value byte! */
  rm_write_block(&node, 3, trunc_v.bytes, trunc_v.count);
  rm_buf_free(&trunc_v);
  rm_buf orig_s = {0};
  rm_write_string(&orig_s, "Orig");
  rm_write_stamped_block(&node, 2, rm_crdt_make(1, 10), orig_s.bytes,
                         orig_s.count);
  rm_buf_free(&orig_s);
  rm_buf bad_n = {0};
  rm_write_id(&bad_n, 1, rm_crdt_make(1, 30));
  rm_write_tag(&bad_n, 2, RM_TAG_LENGTH4);
  rm_write_u32(&bad_n, 1);
  rm_buf_append_byte(&bad_n, 0x80); /* unfinished string varint */
  rm_write_block(&node, 2, bad_n.bytes, bad_n.count);
  rm_buf_free(&bad_n);
  append_block(&b, 2, 1, 2, node.bytes, node.count);

  rm_write_id(&grp, 1, rm_crdt_make(0, 1));
  rm_write_id(&grp, 2, rm_crdt_make(1, 12));
  rm_write_id(&grp, 3, RM_CRDT_NONE);
  rm_write_id(&grp, 4, RM_CRDT_NONE);
  rm_write_tag(&grp, 5, RM_TAG_BYTE4);
  rm_write_u32(&grp, 0);
  rm_buf_append_byte(&gval, 2);
  rm_write_id(&gval, 2, SHOWN);
  rm_write_block(&grp, 6, gval.bytes, gval.count);
  append_block(&b, 4, 1, 1, grp.bytes, grp.count);

  rm_edit_session* s = open_page(&b);
  rm_edit_layer* ls = nullptr;
  size_t lc = 0;
  size_t bsz = 0;
  const uint8_t* bb = rm_edit_bytes(s, &bsz);
  bool found_n = false;
  char* nlabel = nullptr;
  RM_CHECK(rmi_scene_node_state(bb, bsz, rm_item_id_of(SHOWN), &found_n,
                                nullptr, &nlabel) == RM_OK &&
               found_n && nlabel != nullptr && strcmp(nlabel, "Orig") == 0,
           "pre-edit reads Orig@10 despite truncated@30");
  free(nlabel);
  nlabel = nullptr;
  RM_CHECK(rm_edit_layers(s, &ls, &lc, nullptr) == RM_OK && lc == 1 &&
               !ls[0].visible,
           "pre-edit reads valid hidden@10 despite truncated@30");
  rm_edit_layers_free(ls);
  RM_CHECK(
      rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 20},
                               true, nullptr) == RM_OK,
      "vis stamp 20 beats 10 while ignoring truncated 30 (R74)");
  RM_CHECK(rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 21},
                                 "Updated", nullptr) == RM_OK,
           "name stamp 21 beats 10 while ignoring truncated 30 (R95)");
  bb = rm_edit_bytes(s, &bsz);
  RM_CHECK(rmi_scene_node_state(bb, bsz, rm_item_id_of(SHOWN), &found_n,
                                nullptr, &nlabel) == RM_OK &&
               found_n && nlabel != nullptr && strcmp(nlabel, "Updated") == 0,
           "reopened node reflects updated name");
  free(nlabel);
  nlabel = nullptr;
  RM_CHECK(
      rm_edit_layers(s, &ls, &lc, nullptr) == RM_OK && lc == 1 && ls[0].visible,
      "reopened node reflects visibility update");
  rm_edit_layers_free(ls);
  rm_edit_close(s);

  /* Trailing short LENGTH4 prefix and short BYTE4 field preserve anchor &
   * opaque tail across rename (R141, R163); tag 3 BYTE1 before tag 3 LENGTH4
   * (R154); unstamped TAG_NODE_NAME vs explicit {0, 0} stamp (R215). */
  for (int trail = 0; trail < 4; ++trail) {
    node.count = 0;
    rm_write_id(&node, 1, SHOWN);
    if (trail == 2) {
      rm_write_tag(&node, 3, RM_TAG_BYTE1);
      rm_buf_append_byte(&node, 1);
      rm_write_stamped_u8(&node, 3, rm_crdt_make(1, 50), 1);
    } else if (trail == 3) {
      rm_buf n2 = {0};
      rm_write_string(&n2, "BareName");
      rm_write_block(&node, 2, n2.bytes, n2.count); /* no tag 1 stamp! */
      rm_buf_free(&n2);
    } else {
      rm_buf astr = {0};
      rm_write_string(&astr, "KeepMe");
      rm_write_stamped_block(&node, 2, rm_crdt_make(1, 5), astr.bytes,
                             astr.count);
      rm_buf_free(&astr);
      if (trail == 0) {
        rm_write_tag(&node, 7, RM_TAG_LENGTH4);
        rm_buf_append(&node, (const uint8_t[]){0x09, 0x00}, 2);
      } else {
        rm_write_tag(&node, 9, RM_TAG_BYTE4);
        rm_buf_append(&node, (const uint8_t[]){0x11, 0x22}, 2);
      }
    }
    b.count = RM_HEADER_LENGTH;
    append_block(&b, 2, 1, 2, node.bytes, node.count);
    append_block(&b, 4, 1, 1, grp.bytes, grp.count);
    s = open_page(&b);
    if (trail == 2) {
      RM_CHECK(
          rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 20},
                                   false, nullptr) == RM_ERR_INVALID_ARGUMENT,
          "tag 3 BYTE1 before stamped tag 3 rejects stale vis (R154)");
      RM_CHECK(
          rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 60},
                                   false, nullptr) == RM_OK,
          "fresh vis past tag 3 BYTE1 succeeds (R154)");
    } else if (trail == 3) {
      RM_CHECK(
          rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 60},
                                "Stamped", nullptr) == RM_OK,
          "unstamped tag 2 name allows new stamped insert (R215)");
      size_t sz = 0;
      const uint8_t* eb = rm_edit_bytes(s, &sz);
      RM_CHECK(rmi_scene_node_state(eb, sz, rm_item_id_of(SHOWN), &found_n,
                                    nullptr, &nlabel) == RM_OK &&
                   found_n && nlabel != nullptr &&
                   strcmp(nlabel, "Stamped") == 0,
               "reopened name is Stamped");
      free(nlabel);
      nlabel = nullptr;
      RM_CHECK(buf_has_bytes(eb, sz, "BareName", 8),
               "original unstamped BareName bytes survive verbatim (R215)");
    } else {
      RM_CHECK(
          rm_edit_set_node_visible(s, rm_item_id_of(SHOWN), (rm_item_id){1, 60},
                                   false, nullptr) == RM_OK,
          "edit succeeds before truncated trailing field=%d (R163)", trail);
      size_t sz = 0;
      const uint8_t* eb = rm_edit_bytes(s, &sz);
      const uint8_t needle0[3] = {0x7C, 0x09, 0x00};
      const uint8_t needle1[4] = {0x94, 0x01, 0x11, 0x22};
      RM_CHECK(buf_has_bytes(eb, sz, trail == 0 ? needle0 : needle1,
                             trail == 0 ? 3u : 4u),
               "truncated trailing bytes preserved verbatim (trail=%d)", trail);
    }
    rm_edit_close(s);
  }
  rm_buf_free(&gval);
  rm_buf_free(&grp);
  rm_buf_free(&node);
  rm_buf_free(&b);
}

static void append_text_row_item(rm_buf* list, rm_crdt_id id, rm_crdt_id left,
                                 rm_crdt_id right, uint32_t deleted,
                                 const char* text, uint8_t encoding) {
  rm_buf row = {0}, val = {0};
  rm_write_id(&row, 2, id);
  rm_write_id(&row, 3, left);
  rm_write_id(&row, 4, right);
  rm_write_tag(&row, 5, RM_TAG_BYTE4);
  rm_write_u32(&row, deleted);
  rm_write_varuint(&val, text ? strlen(text) : 0);
  rm_buf_append_byte(&val, encoding);
  if (text) rm_buf_append(&val, text, strlen(text));
  rm_write_block(&row, 6, val.bytes, val.count);
  rm_write_block(list, 0, row.bytes, row.count);
  rm_buf_free(&val);
  rm_buf_free(&row);
}

static void surplus_row_counts_must_agree(void) {
  size_t surplus = 0;
  RM_CHECK(rmi_text_row_surplus(3, 1, &surplus) == RM_OK && surplus == 2,
           "a partial decode retains the remaining declared rows");
  RM_CHECK(rmi_text_row_surplus(1, 2, &surplus) == RM_ERR_UNVERIFIED,
           "more decoded rows than declared refuse the edit");
}

static void a5_root_text_edit_edge_cases_and_preferred_layout(void) {
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke good =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);

  /* 1) Extended multi-scalar row 'ab' split by peer 'X' appending after 'aXb'
   * keeps left = 'b' & original row bytes verbatim (R93, R103), plus 'AB' with
   * intervening tombstone + marker replacing 'B' (R107). */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, ext_raw = {0};
    start_page(&b);
    rm_buf r_ab = {0}, val_ab = {0};
    rm_write_id(&r_ab, 2, rm_crdt_make(1, 10));
    rm_write_id(&r_ab, 3, RM_CRDT_NONE);
    rm_write_id(&r_ab, 4, RM_CRDT_NONE);
    rm_write_tag(&r_ab, 5, RM_TAG_BYTE4);
    rm_write_u32(&r_ab, 0);
    rm_write_string(&val_ab, "ab");
    rm_write_tag(&val_ab, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&val_ab, 0x55); /* extension marks row extended! */
    rm_write_block(&r_ab, 6, val_ab.bytes, val_ab.count);
    rm_buf_append(&ext_raw, r_ab.bytes, r_ab.count);
    rm_write_varuint(&l, 2);
    rm_write_block(&l, 0, r_ab.bytes, r_ab.count);
    rm_buf_free(&val_ab);
    rm_buf_free(&r_ab);
    append_text_row_item(&l, rm_crdt_make(2, 50), rm_crdt_make(1, 10),
                         rm_crdt_make(1, 11), 0, "X", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 3, 0, "!", (rm_item_id){1, 90}, nullptr) ==
                 RM_OK,
             "append after split extended 'aXb' succeeds");
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
                 p->text.paragraphs[0].text.length == 4 &&
                 memcmp(p->text.paragraphs[0].text.bytes, "aXb!", 4) == 0,
             "appended '!' lands at end -> 'aXb!'");
    rm_page_free(p);
    RM_CHECK(text_links(s, (rm_item_id){1, 90}, (rm_item_id){1, 11},
                        (rm_item_id){0}),
             "append left points to 'b' (1:11), NOT 'X' (R93, R103)");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(buf_has_bytes(eb, sz, ext_raw.bytes, ext_raw.count),
             "original extended 'ab' wire bytes preserved verbatim");
    rm_edit_close(s);

    /* 'AB' with intervening tombstone + format marker: replace 'B' at offset 1.
     */
    b.count = RM_HEADER_LENGTH;
    l.count = 0;
    in.count = 0;
    it.count = 0;
    root.count = 0;
    rm_write_varuint(&l, 4);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "A", 1);
    append_text_row_item(&l, rm_crdt_make(1, 11), rm_crdt_make(1, 10),
                         RM_CRDT_NONE, 1, nullptr, 1);
    rm_buf mrk = {0}, mval = {0};
    rm_write_id(&mrk, 2, rm_crdt_make(1, 12));
    rm_write_id(&mrk, 3, rm_crdt_make(1, 11));
    rm_write_id(&mrk, 4, RM_CRDT_NONE);
    rm_write_tag(&mrk, 5, RM_TAG_BYTE4);
    rm_write_u32(&mrk, 0);
    rm_write_varuint(&mval, 0);
    rm_buf_append_byte(&mval, 1);
    rm_write_tag(&mval, 2, RM_TAG_BYTE4);
    rm_write_u32(&mval, 1);
    rm_write_block(&mrk, 6, mval.bytes, mval.count);
    rm_write_block(&l, 0, mrk.bytes, mrk.count);
    rm_buf_free(&mval);
    rm_buf_free(&mrk);
    append_text_row_item(&l, rm_crdt_make(1, 13), rm_crdt_make(1, 12),
                         RM_CRDT_NONE, 0, "B", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 1, 1, "Z", (rm_item_id){1, 50}, nullptr) ==
                 RM_OK,
             "replacing B at offset 1 with intervening tomb+marker (R107)");
    p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
                 p->text.paragraphs[0].text.length == 2 &&
                 memcmp(p->text.paragraphs[0].text.bytes, "AZ", 2) == 0,
             "text becomes AZ");
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, ext_raw};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* 2) Unequal newlines duplicate key Q vs distinct Q1 != Q2 (R194), pure
   * deletion "\nC" (R199), "A\nBC" deleting 'B' with Q on '\n' and 'B' (R206),
   * 0:0 sentinel newline style preserved (R210), empty tag 2 styles (R140),
   * partial trailing style entry (R160), unknown BYTE4/LENGTH4 sibling (R196,
   * R201). */
  {
    /* "A\nB\nC" -> replace "\nB\nC" with "\nX": duplicate Q on both '\n'
     * returns RM_ERR_UNVERIFIED; distinct Q1!=Q2 inherits B's style on X
     * (R194); pure deletion "\nC" on shared Q returns RM_ERR_UNVERIFIED (R199).
     */
    for (int mode = 0; mode < 3; ++mode) {
      rm_buf b, l = {0}, in = {0}, it = {0}, st = {0}, sl = {0}, root = {0},
                pos = {0};
      start_page(&b);
      rm_write_varuint(&l, 5);
      rm_crdt_id nl1 = rm_crdt_make(1, 20);
      rm_crdt_id nl2 = mode == 1 ? rm_crdt_make(1, 30) : nl1;
      append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE,
                           0, "A", 1);
      append_text_row_item(&l, nl1, rm_crdt_make(1, 10), rm_crdt_make(1, 11), 0,
                           "\n", 1);
      append_text_row_item(&l, rm_crdt_make(1, 11), rm_crdt_make(1, 10),
                           RM_CRDT_NONE, 0, "B", 1);
      append_text_row_item(&l, nl2, rm_crdt_make(1, 11), rm_crdt_make(1, 12), 0,
                           "\n", 1);
      append_text_row_item(&l, rm_crdt_make(1, 12), rm_crdt_make(1, 11),
                           RM_CRDT_NONE, 0, "C", 1);
      rm_write_block(&in, 1, l.bytes, l.count);
      rm_write_block(&it, 1, in.bytes, in.count);
      rm_write_varuint(&st, mode == 1 ? 2 : 1);
      rm_write_bare_id(&st, nl1);
      rm_write_id(&st, 1, rm_crdt_make(1, 40));
      const uint8_t fmt_h[] = {0x11, 0x02};
      rm_write_block(&st, 2, fmt_h, sizeof fmt_h);
      if (mode == 1) {
        rm_write_bare_id(&st, nl2);
        rm_write_id(&st, 1, rm_crdt_make(1, 41));
        const uint8_t fmt_b[] = {0x11, 0x03};
        rm_write_block(&st, 2, fmt_b, sizeof fmt_b);
      }
      rm_write_block(&sl, 1, st.bytes, st.count);
      rm_write_block(&it, 2, sl.bytes, sl.count);
      rm_write_id(&root, 1, RM_CRDT_NONE);
      rm_write_block(&root, 2, it.bytes, it.count);
      rm_write_f64(&pos, -468);
      rm_write_f64(&pos, 234);
      rm_write_block(&root, 3, pos.bytes, pos.count);
      append_block(&b, 0x07, 1, 1, root.bytes, root.count);

      rm_edit_session* s = open_page(&b);
      if (mode == 2) {
        RM_CHECK(rm_edit_replace_text(s, 3, 2, "", (rm_item_id){1, 90},
                                      nullptr) == RM_ERR_UNVERIFIED,
                 "pure deletion of shared-ID '\\nC' refuses UNVERIFIED (R199)");
      } else {
        rm_status st_res =
            rm_edit_replace_text(s, 1, 4, "\nX", (rm_item_id){1, 90}, nullptr);
        RM_CHECK(st_res == (mode == 0 ? RM_ERR_UNVERIFIED : RM_OK),
                 "unequal newlines mode=%d result", mode);
        if (mode == 1) {
          rm_page* p = parse_session_bytes(s);
          RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
                       p->text.paragraphs[1].has_format &&
                       p->text.paragraphs[1].format.raw == 2,
                   "distinct Q1!=Q2 lets X inherit B's style cleanly (R194)");
          rm_page_free(p);
        }
      }
      rm_edit_close(s);
      rm_buf bufs[] = {b, l, in, it, st, sl, root, pos};
      for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
        rm_buf_free(&bufs[i]);
    }

    /* 0:0 single newline styled at 0:0 -> replacing with fresh newline keeps
     * para 0 styled and para 1 unstyled (R210). */
    rm_buf b, l = {0}, in = {0}, it = {0}, st = {0}, sl = {0}, root = {0},
              pos = {0};
    start_page(&b);
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, RM_CRDT_NONE, RM_CRDT_NONE, RM_CRDT_NONE, 0, "\n",
                         1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_varuint(&st, 1);
    rm_write_bare_id(&st, RM_CRDT_NONE);
    rm_write_id(&st, 1, rm_crdt_make(1, 40));
    const uint8_t fmt_h[] = {0x11, 0x02};
    rm_write_block(&st, 2, fmt_h, sizeof fmt_h);
    rm_write_block(&sl, 1, st.bytes, st.count);
    rm_write_block(&it, 2, sl.bytes, sl.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "\n", (rm_item_id){1, 80},
                                  nullptr) == RM_OK,
             "replacing 0:0 newline with fresh newline succeeds (R210)");
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
                 p->text.paragraphs[0].has_format &&
                 !p->text.paragraphs[1].has_format,
             "0:0 sentinel paragraph 0 stays styled, paragraph 1 unstyled");
    rm_page_free(p);
    rm_edit_close(s);

    /* "A\nBC" sharing Q on '\n' and 'B', deleting 'B' preserves Q on 'C'
     * (R206). */
    b.count = RM_HEADER_LENGTH;
    l.count = 0;
    in.count = 0;
    it.count = 0;
    st.count = 0;
    sl.count = 0;
    root.count = 0;
    rm_crdt_id qid = rm_crdt_make(1, 20);
    rm_write_varuint(&l, 4);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "A", 1);
    append_text_row_item(&l, qid, rm_crdt_make(1, 10), RM_CRDT_NONE, 0, "\n",
                         1);
    append_text_row_item(&l, qid, qid, RM_CRDT_NONE, 0, "B", 1);
    append_text_row_item(&l, rm_crdt_make(1, 11), qid, RM_CRDT_NONE, 0, "C", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_varuint(&st, 1);
    rm_write_bare_id(&st, qid);
    rm_write_id(&st, 1, rm_crdt_make(1, 40));
    rm_write_block(&st, 2, fmt_h, sizeof fmt_h);
    rm_write_block(&sl, 1, st.bytes, st.count);
    rm_write_block(&it, 2, sl.bytes, sl.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(
        rm_edit_replace_text(s, 2, 1, "", (rm_item_id){0}, nullptr) == RM_OK,
        "pure deletion of ordinary char 'B' sharing newline ID succeeds "
        "(R206)");
    p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
                 p->text.paragraphs[1].has_format &&
                 p->text.paragraphs[1].format.raw == 2,
             "paragraph 2 keeps style on C (R206)");
    rm_page_free(p);
    rm_edit_close(s);

    /* Bounded empty tag 2 styles preserved (R140); truncated unknown BYTE4
     * sibling after items (R196); overlong unknown LENGTH4 sibling hiding
     * style (R201). */
    b.count = RM_HEADER_LENGTH;
    l.count = 0;
    in.count = 0;
    it.count = 0;
    root.count = 0;
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "A\nB", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    const uint8_t empty_sty[] = {0x9F, 0x01};
    rm_write_block(&it, 2, empty_sty, sizeof empty_sty);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 1, 1, "\n", (rm_item_id){1, 50},
                                  nullptr) == RM_OK,
             "empty tag 2 styles replaces newline cleanly (R140)");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(buf_has_bytes(eb, sz, empty_sty, sizeof empty_sty),
             "empty tag 2 styles bytes preserved verbatim (R140)");
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, st, sl, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* 3) Bounded rows list suffix recovery: count=2 + BYTE1 (R205), overlong row
   * 2 (R209), count=3 + BYTE1 + Q2/Q3 (R29-1), count=0/1 + uncounted Q (R211),
   * no-tag6 live row with BYTE1 (R216), PTRDIFF_MAX list count overflow (R212),
   * encoding=1 vs encoding=2 (R167, R17-2, R179), and Medium vs Auto
   * tag3/4/5 serialization (R142, R158, R248, R251). */
  {
    /* PTRDIFF_MAX - 1 count list: 1 active row + BYTE1 ext + 1 trailing row ->
     * 2-row replace hits exact PTRDIFF_MAX (RM_OK) while 3-row split overflows
     * TOO_MANY_TO_NUMBER preserving redo history (R212). */
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, (uint64_t)PTRDIFF_MAX - 1u);
    append_text_row_item(&l, rm_crdt_make(1, 20), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "AB", 1);
    rm_write_tag(&l, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 200), RM_CRDT_NONE, RM_CRDT_NONE,
                         0, "Tail", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_add_layer(s, 0, "Seed", (rm_item_id){1, 500}, nullptr) ==
                     RM_OK &&
                 rm_edit_undo(s),
             "seed redo");
    RM_CHECK(rm_edit_replace_text(s, 1, 0, "X", (rm_item_id){1, 60}, nullptr) ==
                 RM_ERR_TOO_MANY_TO_NUMBER,
             "splitting PTRDIFF_MAX + surplus row overflows atomically (R212)");
    RM_CHECK(rm_edit_redo(s), "redo intact after PTRDIFF_MAX refusal");
    RM_CHECK(rm_edit_undo(s), "undo back");
    RM_CHECK(rm_edit_replace_text(s, 0, 2, "CD", (rm_item_id){1, 61},
                                  nullptr) == RM_OK,
             "replacing existing 1-row keeps count == PTRDIFF_MAX (R212)");
    rm_edit_close(s);

    /* Count=0 + uncounted multi-scalar row Q (R211); live row with BYTE1 and no
     * tag 6 (R216). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    l.count = 0;
    in.count = 0;
    it.count = 0;
    root.count = 0;
    rm_write_varuint(&l, 1);
    rm_buf notag6 = {0};
    rm_write_id(&notag6, 2, rm_crdt_make(1, 300));
    rm_write_id(&notag6, 3, RM_CRDT_NONE);
    rm_write_id(&notag6, 4, RM_CRDT_NONE);
    rm_write_tag(&notag6, 5, RM_TAG_BYTE4);
    rm_write_u32(&notag6, 0);
    rm_write_tag(&notag6, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&notag6, 1); /* extension, no tag 6! */
    rm_write_block(&l, 0, notag6.bytes, notag6.count);
    /* Uncounted Q spanning (1, 100)..(1, 102) past count=1! */
    append_text_row_item(&l, rm_crdt_make(1, 100), RM_CRDT_NONE, RM_CRDT_NONE,
                         0, "QWE", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_insert_stroke(
                 s, rm_item_id_of(SHOWN), (rm_item_id){1, 300}, (rm_item_id){0},
                 (rm_item_id){0}, &good, nullptr) == RM_ERR_INVALID_ARGUMENT,
             "live no-tag6 BYTE1 row ID collides (R216)");
    RM_CHECK(rm_edit_insert_stroke(
                 s, rm_item_id_of(SHOWN), (rm_item_id){1, 101}, (rm_item_id){0},
                 (rm_item_id){0}, &good, nullptr) == RM_ERR_INVALID_ARGUMENT,
             "uncounted trailing row interior ID collides (R211)");
    RM_CHECK(rm_edit_replace_text(s, 0, 0, "Hi", (rm_item_id){1, 400},
                                  nullptr) == RM_OK,
             "append past no-tag6 + uncounted row succeeds");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(buf_has_bytes(eb, sz, notag6.bytes, notag6.count),
             "live no-tag6 BYTE1 row bytes preserved verbatim (R216)");
    rm_edit_close(s);
    rm_buf_free(&notag6);

    /* encoding == 2 non-marker & complete marker=5 (R167, R17-2, R179). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    l.count = 0;
    in.count = 0;
    it.count = 0;
    root.count = 0;
    rm_write_varuint(&l, 2);
    append_text_row_item(&l, rm_crdt_make(1, 100), RM_CRDT_NONE, RM_CRDT_NONE,
                         0, "abc", 2); /* 3-byte non-marker enc=2 -> span 3! */
    rm_buf m2 = {0}, mv2 = {0};
    rm_write_id(&m2, 2, rm_crdt_make(1, 200));
    rm_write_id(&m2, 3, RM_CRDT_NONE);
    rm_write_id(&m2, 4, RM_CRDT_NONE);
    rm_write_tag(&m2, 5, RM_TAG_BYTE4);
    rm_write_u32(&m2, 0);
    rm_write_varuint(&mv2, 19);
    rm_buf_append_byte(&mv2, 2);
    rm_buf_append(&mv2, "https://example.com", 19);
    rm_write_tag(&mv2, 2, RM_TAG_BYTE4);
    rm_write_u32(&mv2, 5);
    rm_write_block(&m2, 6, mv2.bytes, mv2.count);
    rm_write_block(&l, 0, m2.bytes, m2.count);
    rm_buf_free(&mv2);
    rm_buf_free(&m2);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_insert_stroke(
                 s, rm_item_id_of(SHOWN), (rm_item_id){1, 101}, (rm_item_id){0},
                 (rm_item_id){0}, &good, nullptr) == RM_ERR_INVALID_ARGUMENT,
             "encoding=2 3-byte row reserves interior ID 1:101 (R17-2)");
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN),
                                   (rm_item_id){1, 201}, (rm_item_id){0},
                                   (rm_item_id){0}, &good, nullptr) == RM_OK,
             "encoding=2 marker=5 URL has exact opaque_span == 1 (R179)");
    rm_edit_close(s);

    /* Auto mode=4 first text insertion writes tag 5 = (0:0, 0.0f) so a
     * subsequent tag 4 width update takes effect (R248, R251), plus
     * preferredLayout = 258 resolves Medium 936/-468 (R142). */
    for (int lay = 0; lay < 2; ++lay) {
      b.count = RM_HEADER_LENGTH;
      rm_buf sinfo = {0}, szb = {0};
      rm_write_u32(&szb, 1600);
      rm_write_u32(&szb, 2000);
      rm_write_block(&sinfo, 5, szb.bytes, szb.count);
      rm_buf_free(&szb);
      uint64_t mode_val = lay == 0 ? 4u : 258u;
      rm_buf mb = {0};
      rm_write_varuint(&mb, mode_val);
      rm_write_stamped_block(&sinfo, 9, rm_crdt_make(1, 5), mb.bytes, mb.count);
      rm_buf_free(&mb);
      append_block(&b, 13, 0, 1, sinfo.bytes, sinfo.count);
      rm_buf_free(&sinfo);
      s = open_page(&b);
      RM_CHECK(rm_edit_replace_text(s, 0, 0, "Hi", (rm_item_id){1, 50},
                                    nullptr) == RM_OK,
               "first text insertion on layout=%d succeeds", lay);
      rm_page* lp = parse_session_bytes(s);
      RM_CHECK(lp && lp->has_text &&
                   lp->text.column_width == (lay == 0 ? 1152.0 : 936.0) &&
                   lp->text.origin_x == (lay == 0 ? -576.0 : -468.0),
               "layout=%d resolves expected width/origin (R142, R248)", lay);
      rm_page_free(lp);
      rm_edit_close(s);
    }

    rm_buf bufs[] = {b, l, in, it, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }
}

static void append_glyph_block(rm_buf* b, rm_crdt_id parent, rm_crdt_id id,
                               const char* text, bool has_first,
                               rm_crdt_id first, bool has_last, rm_crdt_id last,
                               bool has_incl, uint8_t incl) {
  rm_buf val = {0}, tbuf = {0}, rects = {0}, item = {0};
  rm_buf_append_byte(&val, RM_VALUE_GLYPH_ITEM);
  rm_write_tag(&val, 4, RM_TAG_BYTE4);
  rm_write_u32(&val, RM_COLOR_YELLOW);
  rm_write_string(&tbuf, text);
  rm_write_block(&val, 5, tbuf.bytes, tbuf.count);
  rm_write_varuint(&rects, 1);
  rm_write_f64(&rects, 0);
  rm_write_f64(&rects, 0);
  rm_write_f64(&rects, 10);
  rm_write_f64(&rects, 10);
  rm_write_block(&val, 6, rects.bytes, rects.count);
  if (has_first) rm_write_id(&val, 7, first);
  if (has_last) rm_write_id(&val, 8, last);
  if (has_incl) {
    rm_write_tag(&val, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&val, incl);
  }
  rm_write_id(&item, 1, parent);
  rm_write_id(&item, 2, id);
  rm_write_id(&item, 3, RM_CRDT_NONE);
  rm_write_id(&item, 4, RM_CRDT_NONE);
  rm_write_tag(&item, 5, RM_TAG_BYTE4);
  rm_write_u32(&item, 0);
  rm_write_block(&item, 6, val.bytes, val.count);
  append_block(b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, item.bytes, item.count);
  rm_buf_free(&item);
  rm_buf_free(&rects);
  rm_buf_free(&tbuf);
  rm_buf_free(&val);
}

static void unreadable_glyph_refuses_text_deletion(void) {
  rm_draft_paragraph paragraph = {.text = "ab"};
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = &paragraph,
                         .paragraph_count = 1};
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_page* parsed = nullptr;
  rmi_text_flat_map original = {0};
  RM_CHECK(rm_write_page(&draft, &bytes, &size, nullptr) == RM_OK &&
               rmi_parse_page_with_edit_state(bytes, size, &parsed, &original,
                                              nullptr, nullptr) == RM_OK &&
               original.count == 2,
           "read both character IDs before deleting the text");

  enum { MISSING_COLOR, MISSING_RECTS, OVERSIZED_RECTS };
  for (int damage = MISSING_COLOR;
       original.count == 2 && damage <= OVERSIZED_RECTS; ++damage) {
    rm_buf page, value = {0}, item = {0};
    start_page(&page);
    rm_buf_append_byte(&value, RM_VALUE_GLYPH_ITEM);
    if (damage != MISSING_COLOR) {
      rm_write_tag(&value, 4, RM_TAG_BYTE4);
      rm_write_u32(&value, RM_COLOR_YELLOW);
    }

    if (damage == OVERSIZED_RECTS) {
      rm_write_tag(&value, 6, RM_TAG_LENGTH4);
      rm_write_u32(&value, UINT32_MAX);
    } else if (damage != MISSING_RECTS) {
      uint8_t rectangles[1 + 32] = {1};
      rm_write_block(&value, 6, rectangles, sizeof rectangles);
    }

    rm_write_id(&value, 7, original.ids[0]);
    rm_write_id(&value, 8, original.ids[1]);
    append_raw_preamble(&item, 80, 0);
    rm_write_block(&item, 6, value.bytes, value.count);
    append_block(&page, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, item.bytes,
                 item.count);

    rm_edit_session* session = nullptr;
    rm_error error = {0};
    RM_CHECK(rm_edit_open(page.bytes, page.count, &session, &error) ==
                     RM_ERR_INCOMPLETE_PAGE &&
                 error.value == 1,
             "the parser counts the unreadable highlight as a loss (%d)",
             damage);
    rm_edit_close(session);

    RM_CHECK(
        rm_test_verify_glyph_deletion(&page, &original) == RM_ERR_UNVERIFIED,
        "the verifier refuses a broken highlight naming removed text (%d)",
        damage);
    rm_buf_free(&item);
    rm_buf_free(&value);
    rm_buf_free(&page);
  }

  rmi_text_flat_map_free(&original);
  rm_page_free(parsed);
  rm_bytes_free(bytes);
}

static void a5_sentinels_removed_anchors_and_glyph_remapping(void) {
  /* 1) Interior text rows carrying BOTH sentinels {0, 0xFFFFFFFFFFFE} and
   * {0, 0xFFFFFFFFFFFF} never hijack anchor layout or get remapped (R218). */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, node = {0},
              areg = {0}, grp = {0}, gval = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "X", 1);
    append_text_row_item(&l, rm_crdt_make(0, 0xFFFFFFFFFFFEu),
                         rm_crdt_make(1, 10), RM_CRDT_NONE, 0, "S", 1);
    append_text_row_item(&l, rm_crdt_make(0, 0xFFFFFFFFFFFFu),
                         rm_crdt_make(0, 0xFFFFFFFFFFFEu), RM_CRDT_NONE, 0, "E",
                         1);
    append_text_row_item(&l, rm_crdt_make(1, 11),
                         rm_crdt_make(0, 0xFFFFFFFFFFFFu), RM_CRDT_NONE, 0, "Z",
                         1);
    rm_buf lcnt = {0};
    rm_write_varuint(&lcnt, 4);
    rm_buf_append(&lcnt, l.bytes, l.count);
    rm_write_block(&in, 1, lcnt.bytes, lcnt.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    rm_buf_free(&lcnt);

    rm_crdt_id anch = rm_crdt_make(1, 50);
    rm_write_id(&node, 1, anch);
    rm_write_id(&areg, 1, rm_crdt_make(1, 51));
    rm_write_id(&areg, 2, rm_crdt_make(0, 0xFFFFFFFFFFFFu));
    rm_write_block(&node, 7, areg.bytes, areg.count);
    append_block(&b, 2, 1, 2, node.bytes, node.count);
    rm_write_id(&grp, 1, SHOWN);
    rm_write_id(&grp, 2, rm_crdt_make(1, 52));
    rm_write_id(&grp, 3, RM_CRDT_NONE);
    rm_write_id(&grp, 4, RM_CRDT_NONE);
    rm_write_tag(&grp, 5, RM_TAG_BYTE4);
    rm_write_u32(&grp, 0);
    rm_buf_append_byte(&gval, 2);
    rm_write_id(&gval, 2, anch);
    rm_write_block(&grp, 6, gval.bytes, gval.count);
    append_block(&b, 4, 1, 1, grp.bytes, grp.count);
    append_stroke(&b, rm_crdt_make(1, 53), anch, RM_TOOL_FINELINER_2);

    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].has_anchor &&
                 p->strokes[0].anchor.paragraph == 0 &&
                 p->strokes[0].anchor.offset == 5,
             "end sentinel skips interior {0, 0xFFFFFFFFFFFF} row (R218)");
    rm_page_free(p);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 1, 2, "SE", (rm_item_id){1, 80},
                                  nullptr) == RM_OK,
             "replacing interior sentinel rows preserves boundary sentinel");
    p = parse_session_bytes(s);
    RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].has_anchor &&
                 p->strokes[0].anchor.offset == 5,
             "end sentinel still resolves to end after interior replace");
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, node, areg, grp, gval};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* 2) Duplicate visible ID anchor refusal (R189) + Live Glyph Highlight
   * Remapping rules (a)-(g) + linear work rm_test_glyph_checks() & exact 2
   * scene decodes rm_test_scene_block_reads() (R165, R166, R169-R180,
   * R182-R203, R207, R213, R24-1). */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_two_layers(&b, true);
    /* Page text: "A\nabc def ghi" (A=1:10, \n=1:11, abc def ghi = 1:12..1:22).
     */
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "A\nabc def ghi", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    /* Highlight over "abc" (1:12 .. 1:14, inclusive) inside HIDDEN child group.
     */
    rm_crdt_id hgrp = rm_crdt_make(1, 60);
    append_nested_group(&b, HIDDEN, rm_crdt_make(1, 61), hgrp);
    append_glyph_block(&b, hgrp, rm_crdt_make(1, 62), "abc", true,
                       rm_crdt_make(1, 12), true, rm_crdt_make(1, 14), true, 1);
    rm_edit_session* s = open_page(&b);

    /* Interior substitution "abc" -> "aXc" refuses atomically (R169). */
    RM_CHECK(rm_edit_replace_text(s, 3, 1, "X", (rm_item_id){1, 200},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "interior highlight substitution refused atomically");
    /* Pure interior insertion "abc" -> "abXc" refuses atomically (R169). */
    RM_CHECK(rm_edit_replace_text(s, 4, 0, "X", (rm_item_id){1, 201},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "pure interior highlight insertion refused atomically");
    /* Word joining "abc def" -> "abcxdef" refuses atomically (R178). */
    RM_CHECK(rm_edit_replace_text(s, 5, 1, "x", (rm_item_id){1, 202},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "word-joining highlight boundary edit refused");
    /* Deleting "ab" (under HIDDEN child group) refuses atomically (R198). */
    RM_CHECK(rm_edit_replace_text(s, 2, 2, "", (rm_item_id){0}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "hidden child group highlight enforces protection (R198)");
    /* Pre-highlight same-char replacement 'A' -> 'A' succeeds preserving 0x03
     * (R184); later-word simple-LTR edit "ghi" -> "XYZ" succeeds (R24-1). */
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "A", (rm_item_id){1, 203},
                                  nullptr) == RM_OK,
             "pre-highlight same-char replace leaves highlight intact (R184)");
    RM_CHECK(rm_edit_replace_text(s, 10, 3, "XYZ", (rm_item_id){1, 204},
                                  nullptr) == RM_OK,
             "later-word simple-LTR edit after SP succeeds (R24-1)");
    /* Simple-LTR SP -> '\n' right after highlighted "abc" succeeds (R170,
     * R193).
     */
    RM_CHECK(
        rm_edit_replace_text(s, 5, 1, "\n", (rm_item_id){1, 220}, nullptr) ==
            RM_OK,
        "simple-LTR space -> newline immediately after highlight succeeds");
    rm_edit_close(s);

    /* Removed ancestor layer excludes glyph from protection while restoring
     * layer restores it (R166, R198). */
    rm_buf brem = {0};
    rm_buf_append(&brem, b.bytes, b.count);
    append_removal(&brem, HIDDEN);
    s = open_page(&brem);
    RM_CHECK(rm_edit_replace_text(s, 2, 3, "zzz", (rm_item_id){1, 206},
                                  nullptr) == RM_OK,
             "0x01-removed child-group glyph does not block text edit (R166)");
    rm_edit_close(s);
    rm_buf_free(&brem);

    /* First-only highlight at EOF appending '\n' -> RM_ERR_UNVERIFIED with no
     * OOB read (R207). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    l.count = 0;
    in.count = 0;
    it.count = 0;
    root.count = 0;
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "abc", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    append_glyph_block(&b, SHOWN, rm_crdt_make(1, 62), "c", true,
                       rm_crdt_make(1, 12), false, RM_CRDT_NONE, false, 0);
    s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 3, 0, "\n", (rm_item_id){1, 200},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "first-only highlight at EOF appending newline refused cleanly "
             "(R207)");
    rm_edit_close(s);

    /* Non-Form-1 tag 9 without tag 7 -> RM_ERR_UNVERIFIED (R195); tag 7 =
     * RM_CRDT_NONE -> RM_ERR_UNVERIFIED (R213); truncated tag 7 ->
     * RM_ERR_UNVERIFIED (R192). */
    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    append_glyph_block(&b, SHOWN, rm_crdt_make(1, 62), "c", false, RM_CRDT_NONE,
                       false, RM_CRDT_NONE, true, 1);
    s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "a", (rm_item_id){1, 200},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "tag 9 without tag 7 refuses UNVERIFIED (R195)");
    rm_edit_close(s);

    b.count = RM_HEADER_LENGTH;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    append_glyph_block(&b, SHOWN, rm_crdt_make(1, 62), "a", true, RM_CRDT_NONE,
                       false, RM_CRDT_NONE, false, 0);
    s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "a", (rm_item_id){1, 200},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "tag 7 = RM_CRDT_NONE refuses UNVERIFIED (R213)");
    rm_edit_close(s);

    /* Linear work & exact 2 scene decodes on zero-highlight stroke-heavy page
     * (R180, R186, R197). */
    rm_buf bperf;
    start_page(&bperf);
    append_layer(&bperf, SHOWN, false, rm_crdt_make(1, 12));
    for (int k = 0; k < 20; ++k) {
      append_stroke(&bperf, rm_crdt_make(1, 5000 + (uint64_t)k), SHOWN,
                    RM_TOOL_FINELINER_2);
    }
    char bigs[1001];
    memset(bigs, 'a', 1000);
    bigs[1000] = '\0';
    rm_buf pl = {0}, pin = {0}, pit = {0}, proot = {0};
    rm_write_varuint(&pl, 1);
    append_text_row_item(&pl, rm_crdt_make(2, 1), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         bigs, 1);
    rm_write_block(&pin, 1, pl.bytes, pl.count);
    rm_write_block(&pit, 1, pin.bytes, pin.count);
    rm_write_id(&proot, 1, RM_CRDT_NONE);
    rm_write_block(&proot, 2, pit.bytes, pit.count);
    rm_write_block(&proot, 3, pos.bytes, pos.count);
    append_block(&bperf, 0x07, 1, 1, proot.bytes, proot.count);
    rm_buf_free(&proot);
    rm_buf_free(&pit);
    rm_buf_free(&pin);
    rm_buf_free(&pl);
    s = open_page(&bperf);
    rm_test_reset_scene_block_reads();
    RM_CHECK(rm_edit_replace_text(s, 1000, 0, "!", (rm_item_id){3, 1},
                                  nullptr) == RM_OK,
             "pure append on stroke-heavy zero-highlight page succeeds");
    RM_CHECK(rm_test_scene_block_reads() == 2,
             "pure append executes exact 2 scene decodes (got %zu)",
             rm_test_scene_block_reads());
    rm_edit_close(s);

    /* H = 200 overlapping full-document highlights on N = 1000 chars undergoing
     * whole-range same-text replacement stays O(N + H) (R186). */
    for (int h = 0; h < 200; ++h) {
      append_glyph_block(&bperf, SHOWN, rm_crdt_make(4, 1 + (uint64_t)h), "",
                         true, rm_crdt_make(2, 1), true, rm_crdt_make(2, 1000),
                         true, 1);
    }
    s = open_page(&bperf);
    rm_test_reset_glyph_checks();
    RM_CHECK(rm_edit_replace_text(s, 0, 1000, bigs, (rm_item_id){5, 1},
                                  nullptr) == RM_OK,
             "200 full-doc highlights whole-range same-text replace succeeds");
    RM_CHECK(rm_test_glyph_checks() <= 4u * (1000u + 200u),
             "200 overlapping highlights run in O(N + H) checks (got %zu)",
             rm_test_glyph_checks());
    rm_edit_close(s);

    /* Trailing append on 200 near-end highlights stays O(N + H) too
     * (RV43-R2-13). */
    char trails[1001];
    memset(trails, 'a', 999);
    trails[999] = '\n';
    trails[1000] = '\0';
    rm_buf btrail = {0};
    start_page(&btrail);
    append_layer(&btrail, SHOWN, false, rm_crdt_make(1, 12));
    pl.count = 0;
    pin.count = 0;
    pit.count = 0;
    proot.count = 0;
    rm_write_varuint(&pl, 1);
    append_text_row_item(&pl, rm_crdt_make(2, 1), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         trails, 1);
    rm_write_block(&pin, 1, pl.bytes, pl.count);
    rm_write_block(&pit, 1, pin.bytes, pin.count);
    rm_write_id(&proot, 1, RM_CRDT_NONE);
    rm_write_block(&proot, 2, pit.bytes, pit.count);
    rm_write_block(&proot, 3, pos.bytes, pos.count);
    append_block(&btrail, 0x07, 1, 1, proot.bytes, proot.count);
    rm_buf_free(&proot);
    rm_buf_free(&pit);
    rm_buf_free(&pin);
    rm_buf_free(&pl);
    for (int h = 0; h < 200; ++h) {
      append_glyph_block(&btrail, SHOWN, rm_crdt_make(4, 500 + (uint64_t)h),
                         "a", true, rm_crdt_make(2, 999), false, RM_CRDT_NONE,
                         false, 0);
    }
    s = open_page(&btrail);
    rm_test_reset_glyph_checks();
    RM_CHECK(rm_edit_replace_text(s, 1000, 0, "!", (rm_item_id){5, 10},
                                  nullptr) == RM_OK,
             "trailing append after 200 end-anchored highlights succeeds");
    RM_CHECK(rm_test_glyph_checks() <= 4u * (1000u + 200u),
             "trailing append checks stay O(N + H) (got %zu)",
             rm_test_glyph_checks());
    rm_edit_close(s);
    rm_buf_free(&btrail);
    rm_buf_free(&bperf);

    rm_buf bufs[] = {b, l, in, it, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }
}

static bool scene_tree_ops_unique(const uint8_t* data, size_t size) {
  rm_idmap seen;
  rm_idmap_init(&seen);
  bool unique = true;
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    if (rm_read_block_header(&r, &b) != RM_OK) {
      unique = false;
      break;
    }
    if (b.type == RM_BLOCK_SCENE_TREE) {
      rm_reader body = r;
      body.size = b.end;
      rm_crdt_id op_id;
      if (rm_read_crdt(&body, 1, &op_id) == RM_OK &&
          !rm_crdt_equal(op_id, RM_CRDT_NONE)) {
        if (rm_idmap_contains(&seen, op_id) || !rm_idmap_set(&seen, op_id, 1)) {
          unique = false;
          break;
        }
      }
    }
    r.offset = b.end;
  }
  rm_idmap_free(&seen);
  return unique;
}

static void claude_rv43_regression_checks(void) {
  /* RV43-1: placement-free page where SHOWN already has a parentless 0x01
   * creation record (tag 1 = SHOWN, tag 2 = 0:0, tag 3 = 1). */
  {
    rm_buf b, rec = {0};
    start_page(&b);
    append_two_layers(&b, false);
    rm_write_id(&rec, 1, SHOWN);
    rm_write_id(&rec, 2, RM_CRDT_NONE);
    rm_write_tag(&rec, 3, RM_TAG_BYTE1);
    rm_buf_append_byte(&rec, 1);
    append_block(&b, RM_BLOCK_SCENE_TREE, 1, 1, rec.bytes, rec.count);
    rm_buf_free(&rec);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_set_node_name(s, rm_item_id_of(SHOWN), (rm_item_id){1, 500},
                              "Seed", nullptr) == RM_OK &&
            rm_edit_undo(s),
        "RV43-1 setup redo");
    RM_CHECK(
        rm_edit_add_layer(s, 1, "Mid", (rm_item_id){1, 600}, nullptr) ==
            RM_ERR_UNVERIFIED,
        "RV43-1 placement-free add_layer refuses colliding 0x01 creation ID");
    RM_CHECK(rm_edit_redo(s) && rm_edit_undo(s),
             "RV43-1 redo intact after unverified refusal");
    RM_CHECK(rm_edit_delete_layer(s, rm_item_id_of(HIDDEN),
                                  (rm_item_id){1, 610}, nullptr) == RM_OK,
             "RV43-1 placement-free delete_layer emits no duplicate 0x01");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(scene_tree_ops_unique(eb, sz),
             "RV43-1 delete preserves unique 0x01 tag 1 op IDs");
    rm_edit_close(s);

    /* Without prior 0x01 creation record, add_layer materializes unique IDs. */
    b.count = RM_HEADER_LENGTH;
    append_two_layers(&b, false);
    s = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(s, 1, "Mid", (rm_item_id){1, 600}, nullptr) == RM_OK,
        "RV43-1 clean placement-free add_layer succeeds");
    eb = rm_edit_bytes(s, &sz);
    RM_CHECK(scene_tree_ops_unique(eb, sz),
             "RV43-1 materialized 0x01 placements keep unique tag 1 IDs");
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* RV43-2: Legacy Form-1 highlight with truncated tag 10 (BYTE4, 2 payload
   * bytes) parses and lets unrelated text edits succeed. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    rm_buf g = {0}, rects = {0}, tbuf = {0}, gval = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "abc", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_write_varuint(&rects, 1);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 10);
    rm_write_f64(&rects, 10);
    rm_write_string(&tbuf, "abc");
    rm_buf_append_byte(&gval, RM_VALUE_GLYPH_ITEM);
    rm_write_tag(&gval, 4, RM_TAG_BYTE4);
    rm_write_u32(&gval, RM_COLOR_YELLOW);
    rm_write_block(&gval, 5, tbuf.bytes, tbuf.count);
    rm_write_block(&gval, 6, rects.bytes, rects.count);
    rm_write_tag(&gval, 10, RM_TAG_BYTE4);
    rm_buf_append_byte(&gval, 0x11);
    rm_buf_append_byte(&gval, 0x22); /* truncated 2-byte optional tail */
    rm_write_id(&g, 1, SHOWN);
    rm_write_id(&g, 2, rm_crdt_make(1, 62));
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 0);
    rm_write_block(&g, 6, gval.bytes, gval.count);
    append_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, g.bytes, g.count);

    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->highlight_count == 1,
             "RV43-2 Form-1 highlight with truncated tag 10 parses cleanly");
    rm_page_free(p);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_replace_text(s, 3, 0, "!", (rm_item_id){1, 200}, nullptr) ==
            RM_OK,
        "RV43-2 truncated tag 10 on Form-1 glyph does not block text edit");
    p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
                 p->text.paragraphs[0].text.length == 4 &&
                 memcmp(p->text.paragraphs[0].text.bytes, "abc!", 4) == 0,
             "RV43-2 appended scalar survives");
    rm_page_free(p);
    rm_edit_close(s);

    rm_buf bufs[] = {b, l, in, it, root, pos, g, rects, tbuf, gval};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-3: Full deletion of peer-split extended 'ab' ('aXb') succeeds while
   * partial fragment deletion refuses RM_ERR_UNVERIFIED. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    rm_buf r_ab = {0}, val_ab = {0};
    rm_write_id(&r_ab, 2, rm_crdt_make(1, 10));
    rm_write_id(&r_ab, 3, RM_CRDT_NONE);
    rm_write_id(&r_ab, 4, RM_CRDT_NONE);
    rm_write_tag(&r_ab, 5, RM_TAG_BYTE4);
    rm_write_u32(&r_ab, 0);
    rm_write_string(&val_ab, "ab");
    rm_write_tag(&val_ab, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&val_ab, 0x55);
    rm_write_block(&r_ab, 6, val_ab.bytes, val_ab.count);
    rm_write_varuint(&l, 2);
    rm_write_block(&l, 0, r_ab.bytes, r_ab.count);
    rm_buf_free(&val_ab);
    rm_buf_free(&r_ab);
    append_text_row_item(&l, rm_crdt_make(2, 50), rm_crdt_make(1, 10),
                         rm_crdt_make(1, 11), 0, "X", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 2, "", (rm_item_id){0}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "RV43-3 partial deletion of split extended row refuses");
    RM_CHECK(
        rm_edit_replace_text(s, 0, 3, "", (rm_item_id){0}, nullptr) == RM_OK,
        "RV43-3 full deletion of split extended 'aXb' succeeds");
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
                 p->text.paragraphs[0].text.length == 0,
             "RV43-3 deleted split extended page is empty");
    rm_page_free(p);
    RM_CHECK(rm_edit_undo(s), "RV43-3 undo back to aXb");
    RM_CHECK(rm_edit_replace_text(s, 0, 3, "Z", (rm_item_id){1, 90}, nullptr) ==
                 RM_OK,
             "RV43-3 replacing entire split extended 'aXb' -> 'Z' succeeds");
    p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 1 &&
                 p->text.paragraphs[0].text.length == 1 &&
                 p->text.paragraphs[0].text.bytes[0] == 'Z',
             "RV43-3 replaced split extended page reads 'Z'");
    rm_page_free(p);
    rm_edit_close(s);

    rm_buf bufs[] = {b, l, in, it, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-4: Non-canonical varuint on untouched style entry preserved verbatim
   * when another newline style is remapped. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, st = {0}, sl = {0}, root = {0},
              pos = {0}, noncanon = {0};
    start_page(&b);
    rm_crdt_id nl1 =
        rm_crdt_make(1, 1); /* encoded non-canonically as 0x81 0x00 */
    rm_crdt_id nl2 = rm_crdt_make(1, 30);
    rm_write_varuint(&l, 5);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "A", 1);
    append_text_row_item(&l, nl1, rm_crdt_make(1, 10), rm_crdt_make(1, 11), 0,
                         "\n", 1);
    append_text_row_item(&l, rm_crdt_make(1, 11), rm_crdt_make(1, 10),
                         RM_CRDT_NONE, 0, "B", 1);
    append_text_row_item(&l, nl2, rm_crdt_make(1, 11), rm_crdt_make(1, 12), 0,
                         "\n", 1);
    append_text_row_item(&l, rm_crdt_make(1, 12), rm_crdt_make(1, 11),
                         RM_CRDT_NONE, 0, "C", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_varuint(&st, 2);
    /* Entry 1: non-canonical bare CRDT (author=1, overlong counter 1 -> 0x81
     * 0x00). */
    rm_buf_append_byte(&noncanon, 1);
    rm_buf_append_byte(&noncanon, 0x81);
    rm_buf_append_byte(&noncanon, 0x00);
    rm_write_id(&noncanon, 1, rm_crdt_make(1, 40));
    const uint8_t fmt_h[] = {0x11, 0x02};
    rm_write_block(&noncanon, 2, fmt_h, sizeof fmt_h);
    rm_buf_append(&st, noncanon.bytes, noncanon.count);
    /* Entry 2: nl2 with heading raw=3. */
    rm_write_bare_id(&st, nl2);
    rm_write_id(&st, 1, rm_crdt_make(1, 41));
    const uint8_t fmt_b[] = {0x11, 0x03};
    rm_write_block(&st, 2, fmt_b, sizeof fmt_b);
    rm_write_block(&sl, 1, st.bytes, st.count);
    rm_write_block(&it, 2, sl.bytes, sl.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 3, 1, "\n", (rm_item_id){1, 90},
                                  nullptr) == RM_OK,
             "RV43-4 replace second newline succeeds");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(buf_has_bytes(eb, sz, noncanon.bytes, noncanon.count),
             "RV43-4 untouched non-canonical style entry preserved verbatim");
    rm_edit_close(s);

    rm_buf bufs[] = {b, l, in, it, st, sl, root, pos, noncanon};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-5: add_layer on settings-only 0x0D (tag 2 without tag 1) rewrites that
   * single 0x0D block in place starting with tag 1 (no extra 0x0D appended). */
  {
    rm_buf b, sset = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_stamped_u8(&sset, 2, rm_crdt_make(1, 50), 0);
    append_block(&b, RM_BLOCK_SCENE_INFO, 0, 1, sset.bytes, sset.count);
    rm_edit_session* s = open_page(&b);
    rm_item_id new_layer = {1, 500};
    RM_CHECK(rm_edit_add_layer(s, 0, "Added", new_layer, nullptr) == RM_OK,
             "RV43-5 add_layer on settings-only 0x0D succeeds");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    size_t count_0d = 0;
    bool first_tag_is_new_layer = false;
    rm_reader r = {.data = eb, .size = sz, .offset = RM_HEADER_LENGTH};
    while (rm_reader_remaining(&r) >= 8) {
      rm_block_header hdr;
      if (rm_read_block_header(&r, &hdr) != RM_OK) break;
      if (hdr.type == RM_BLOCK_SCENE_INFO) {
        ++count_0d;
        rm_reader sub = r;
        sub.size = hdr.end;
        rm_reader val;
        rm_crdt_id stamp, layer;
        if (rm_expect_tag(&sub, 1, RM_TAG_LENGTH4) == RM_OK &&
            rm_read_subblock(&sub, &val) == RM_OK &&
            rm_read_crdt(&val, 1, &stamp) == RM_OK &&
            rm_read_crdt(&val, 2, &layer) == RM_OK &&
            rm_crdt_equal(layer, rm_crdt_of(new_layer))) {
          first_tag_is_new_layer = true;
        }
      }
      r.offset = hdr.end;
    }
    RM_CHECK(count_0d == 1 && first_tag_is_new_layer,
             "RV43-5 single 0x0D block rewritten in place starting with tag 1 "
             "(count=%zu)",
             count_0d);
    rm_page* p = parse(eb, sz);
    RM_CHECK(p && !p->background_visible,
             "RV43-5 background_visible=false preserved across rewrite");
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf_free(&sset);
    rm_buf_free(&b);
  }
}

static void codex_rv43_r2_regression_checks(void) {
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke good =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);

  /* RV43-R2-01 & RV43-R2-07: Authoritative layer A, valid A -> G link, plus
   * stale root -> G (or orphan O -> G) in BOTH file orders still renders,
   * enumerates highlighters, inserts strokes, and transforms strokes in G. */
  for (int use_orphan = 0; use_orphan < 2; ++use_orphan) {
    for (int bad_first = 0; bad_first < 2; ++bad_first) {
      rm_buf b;
      start_page(&b);
      append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
      append_reinsertion(&b, SHOWN, RM_CRDT_ROOT);
      rm_crdt_id g = rm_crdt_make(1, 60);
      rm_crdt_id bad_parent = use_orphan ? rm_crdt_make(1, 999) : RM_CRDT_ROOT;
      if (bad_first) {
        append_nested_group(&b, bad_parent, rm_crdt_make(1, 61), g);
        append_nested_group(&b, SHOWN, rm_crdt_make(1, 62), g);
      } else {
        append_nested_group(&b, SHOWN, rm_crdt_make(1, 62), g);
        append_nested_group(&b, bad_parent, rm_crdt_make(1, 61), g);
      }
      append_stroke(&b, rm_crdt_make(1, 70), g, RM_TOOL_HIGHLIGHTER_2);
      rm_page* p = parse(b.bytes, b.count);
      RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].id.counter == 70,
               "RV43-R2-01 renders reachable group G orphan=%d bad_first=%d",
               use_orphan, bad_first);
      rm_page_free(p);
      rm_item_id* hids = nullptr;
      size_t hcnt = 0;
      RM_CHECK(rm_highlighter_items(b.bytes, b.count, &hids, &hcnt, nullptr) ==
                       RM_OK &&
                   hcnt == 1 && hids[0].counter == 70,
               "RV43-R2-01 highlighter enumerates G");
      rm_item_ids_free(hids);
      rm_edit_session* s = open_page(&b);
      RM_CHECK(
          rm_edit_insert_stroke(s, rm_item_id_of(g), (rm_item_id){1, 71},
                                (rm_item_id){0}, (rm_item_id){0}, &good,
                                nullptr) == RM_OK,
          "RV43-R2-07 insert_stroke into G succeeds orphan=%d bad_first=%d",
          use_orphan, bad_first);
      RM_CHECK(rm_edit_transform_stroke(
                   s, (rm_item_id){1, 70}, (rm_item_id){1, 72},
                   (rm_edit_transform){1, 0, 0, 1, 5, 0}, nullptr) == RM_OK,
               "RV43-R2-01 transform stroke inside G succeeds");
      rm_edit_close(s);
      rm_buf_free(&b);
    }
  }

  /* RV43-R2-02: "AB" starting at Q followed by empty row Q owning an anchor:
   * replacing 'A' -> 'Z' refuses RM_ERR_UNVERIFIED keeping anchor at offset 2.
   */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, node = {0},
              areg = {0}, grp = {0}, gval = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_crdt_id q = rm_crdt_make(1, 10);
    rm_crdt_id end_ab = rm_crdt_make(1, 11);
    rm_write_varuint(&l, 2);
    append_text_row_item(&l, q, RM_CRDT_NONE, RM_CRDT_NONE, 0, "AB", 1);
    append_text_row_item(&l, q, end_ab, RM_CRDT_NONE, 0, nullptr, 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_crdt_id anch = rm_crdt_make(1, 50);
    rm_write_id(&node, 1, anch);
    rm_write_id(&areg, 1, rm_crdt_make(1, 51));
    rm_write_id(&areg, 2, q);
    rm_write_block(&node, 7, areg.bytes, areg.count);
    append_block(&b, 2, 1, 2, node.bytes, node.count);
    rm_write_id(&grp, 1, SHOWN);
    rm_write_id(&grp, 2, rm_crdt_make(1, 52));
    rm_write_id(&grp, 3, RM_CRDT_NONE);
    rm_write_id(&grp, 4, RM_CRDT_NONE);
    rm_write_tag(&grp, 5, RM_TAG_BYTE4);
    rm_write_u32(&grp, 0);
    rm_buf_append_byte(&gval, 2);
    rm_write_id(&gval, 2, anch);
    rm_write_block(&grp, 6, gval.bytes, gval.count);
    append_block(&b, 4, 1, 1, grp.bytes, grp.count);
    append_stroke(&b, rm_crdt_make(1, 53), anch, RM_TOOL_FINELINER_2);

    rm_page* pre = parse(b.bytes, b.count);
    RM_CHECK(pre && pre->stroke_count == 1 && pre->strokes[0].has_anchor &&
                 pre->strokes[0].anchor.offset == 2,
             "RV43-R2-02 empty row owns rendered anchor at offset 2");
    rm_page_free(pre);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "Z", (rm_item_id){1, 80}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "RV43-R2-02 refuses replacing scalar sharing ID with empty row");
    rm_page* post = parse_session_bytes(s);
    RM_CHECK(post && post->stroke_count == 1 && post->strokes[0].has_anchor &&
                 post->strokes[0].anchor.offset == 2 &&
                 post->text.paragraphs[0].text.bytes[0] == 'A',
             "RV43-R2-02 anchor offset 2 and text unchanged after refusal");
    rm_page_free(post);
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, node, areg, grp, gval};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-R2-03: Duplicate tag 7 on 0x03 highlight refuses RM_ERR_UNVERIFIED on
   * same-scalar replacement. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, g = {0},
              rects = {0}, tbuf = {0}, gval = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, 1);
    rm_crdt_id c1 = rm_crdt_make(1, 10);
    append_text_row_item(&l, c1, RM_CRDT_NONE, RM_CRDT_NONE, 0, "a", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_write_varuint(&rects, 1);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 10);
    rm_write_f64(&rects, 10);
    rm_write_string(&tbuf, "a");
    rm_buf_append_byte(&gval, RM_VALUE_GLYPH_ITEM);
    rm_write_tag(&gval, 4, RM_TAG_BYTE4);
    rm_write_u32(&gval, RM_COLOR_YELLOW);
    rm_write_block(&gval, 5, tbuf.bytes, tbuf.count);
    rm_write_block(&gval, 6, rects.bytes, rects.count);
    rm_write_id(&gval, 7, c1);
    rm_write_id(&gval, 7, c1); /* duplicate tag 7 */
    rm_write_id(&g, 1, SHOWN);
    rm_write_id(&g, 2, rm_crdt_make(1, 62));
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 0);
    rm_write_block(&g, 6, gval.bytes, gval.count);
    append_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, g.bytes, g.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "a", (rm_item_id){1, 200},
                                  nullptr) == RM_ERR_UNVERIFIED,
             "RV43-R2-03 duplicate tag 7 refuses UNVERIFIED");
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, g, rects, tbuf, gval};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-R2-04: Placement-free fallback layer with author=0 (0:42) refuses
   * RM_ERR_UNVERIFIED on rm_edit_add_layer. */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, rm_crdt_make(0, 42), false, rm_crdt_make(1, 12));
    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(s, 0, "Top", (rm_item_id){1, 500}, nullptr) ==
            RM_ERR_UNVERIFIED,
        "RV43-R2-04 placement-free add_layer refuses author-0 fallback node");
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* RV43-R2-05 & RV43-R2-06: styles child before overlong inactive items child
   * (R2-05) AND overlong inactive second rows list mimicking fake ID followed
   * by reachable styles stamp (R2-06). */
  {
    rm_buf b, st = {0}, sl = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_crdt_id sty_stamp = rm_crdt_make(1, 44);
    rm_write_varuint(&st, 1);
    rm_write_bare_id(&st, RM_CRDT_NONE);
    rm_write_id(&st, 1, sty_stamp);
    const uint8_t fmt_h[] = {0x11, 0x02};
    rm_write_block(&st, 2, fmt_h, sizeof fmt_h);
    rm_write_block(&sl, 1, st.bytes, st.count);
    rm_write_block(&it, 2, sl.bytes, sl.count); /* tag 2 styles FIRST */
    rm_write_tag(&it, 1, RM_TAG_LENGTH4);
    rm_write_u32(&it, 500u); /* overlong trailing tag 1 items child */
    rm_write_id(&it, 1, rm_crdt_make(1, 77));
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN),
                                   (rm_item_id){1, 200}, (rm_item_id){0},
                                   (rm_item_id){0}, &good, nullptr) == RM_OK,
             "RV43-R2-05 overlong non-initial items child ignored safely");
    RM_CHECK(
        rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), rm_item_id_of(sty_stamp),
                              (rm_item_id){0}, (rm_item_id){0}, &good,
                              nullptr) == RM_ERR_INVALID_ARGUMENT,
        "RV43-R2-05 preceding style stamp reserved");
    rm_edit_close(s);

    /* R2-06: active rows list (tag 1), then overlong second rows list (tag 1)
     * whose payload contains fake_row mimicking tag 1 rows, then reachable
     * tag 2 style stamp real_stamp. */
    b.count = RM_HEADER_LENGTH;
    it.count = 0;
    root.count = 0;
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_buf r1 = {0}, fake_row = {0}, fake_list = {0}, items_c = {0};
    rm_crdt_id fake_id = rm_crdt_make(1, 333);
    rm_crdt_id real_stamp = rm_crdt_make(1, 444);
    rm_write_varuint(&r1, 1);
    append_text_row_item(&r1, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE,
                         0, "hi", 1);
    rm_write_block(&items_c, 1, r1.bytes, r1.count);
    rm_write_varuint(&fake_list, 1);
    append_text_row_item(&fake_list, fake_id, RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "x", 1);
    rm_write_block(&fake_row, 1, fake_list.bytes, fake_list.count);
    rm_write_tag(&items_c, 1, RM_TAG_LENGTH4);
    rm_write_u32(&items_c, (uint32_t)(fake_row.count + 900u));
    rm_buf_append(&items_c, fake_row.bytes, fake_row.count);
    rm_write_block(&it, 1, items_c.bytes, items_c.count);
    st.count = 0;
    sl.count = 0;
    rm_write_varuint(&st, 1);
    rm_write_bare_id(&st, RM_CRDT_NONE);
    rm_write_id(&st, 1, real_stamp);
    rm_write_block(&st, 2, fmt_h, sizeof fmt_h);
    rm_write_block(&sl, 1, st.bytes, st.count);
    rm_write_block(&it, 2, sl.bytes, sl.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);
    s = open_page(&b);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN),
                                   rm_item_id_of(fake_id), (rm_item_id){0},
                                   (rm_item_id){0}, &good, nullptr) == RM_OK,
             "RV43-R2-06 ID inside overlong inactive rows remains free");
    RM_CHECK(
        rm_edit_insert_stroke(
            s, rm_item_id_of(SHOWN), rm_item_id_of(real_stamp), (rm_item_id){0},
            (rm_item_id){0}, &good, nullptr) == RM_ERR_INVALID_ARGUMENT,
        "RV43-R2-06 reachable style stamp after overlong rows is reserved");
    rm_edit_close(s);

    rm_buf bufs[] = {b,   st, sl,       it,        root,
                     pos, r1, fake_row, fake_list, items_c};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-R2-08: Identity transform of RM_TOOL_SHADER with explicit nil tag 9
   * (0:0) preserves {0, 0} source_id verbatim. */
  {
    rm_buf b, val = {0}, pts = {0}, line = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_buf_append_byte(&val, 0x03);
    rm_write_tag(&val, 1, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_TOOL_SHADER);
    rm_write_tag(&val, 2, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_HIGHLIGHT);
    rm_write_tag(&val, 3, RM_TAG_BYTE8);
    rm_write_f64(&val, 1.0);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_f32(&val, 0.0f);
    for (int i = 0; i < 2; ++i) {
      rm_write_f32(&pts, (float)i * 10.0f);
      rm_write_f32(&pts, 0.0f);
      rm_write_u16(&pts, 0);
      rm_write_u16(&pts, 8);
      rm_buf_append_byte(&pts, 0);
      rm_buf_append_byte(&pts, 255);
    }
    rm_write_block(&val, 5, pts.bytes, pts.count);
    rm_write_tag(&val, 8, RM_TAG_BYTE4);
    rm_write_u32(&val, 0x44112233u);
    rm_write_id(&val, 9, RM_CRDT_NONE); /* explicit nil tag 9 */
    rm_write_id(&line, 1, SHOWN);
    rm_write_id(&line, 2, rm_crdt_make(1, 30));
    rm_write_id(&line, 3, RM_CRDT_NONE);
    rm_write_id(&line, 4, RM_CRDT_NONE);
    rm_write_tag(&line, 5, RM_TAG_BYTE4);
    rm_write_u32(&line, 0);
    rm_write_block(&line, 6, val.bytes, val.count);
    append_block(&b, 5, 2, 2, line.bytes, line.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_transform_stroke(
                 s, (rm_item_id){1, 30}, (rm_item_id){1, 40},
                 (rm_edit_transform){1, 0, 0, 1, 0, 0}, nullptr) == RM_OK,
             "RV43-R2-08 identity transform of nil-tag9 shader succeeds");
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(
        p && p->stroke_count == 1 && p->strokes[0].has_source_id &&
            p->strokes[0].source_id.author == 0 &&
            p->strokes[0].source_id.counter == 0,
        "RV43-R2-08 transformed shader keeps explicit nil source_id {0,0}");
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf_free(&line);
    rm_buf_free(&pts);
    rm_buf_free(&val);
    rm_buf_free(&b);
  }

  /* RV43-R2-09: 0x0D SceneInfo with stamp-only tag 1 register (stamp=1:200, no
   * tag 2) + valid winner block (stamp=1:100, SHOWN). */
  {
    rm_buf b, bvalid = {0}, fv = {0}, bso = {0}, fso = {0};
    start_page(&b);
    append_two_layers(&b, false);
    rm_write_id(&fv, 1, rm_crdt_make(1, 100));
    rm_write_id(&fv, 2, SHOWN);
    rm_write_block(&bvalid, 1, fv.bytes, fv.count);
    append_block(&b, RM_BLOCK_SCENE_INFO, 0, 1, bvalid.bytes, bvalid.count);
    rm_write_id(&fso, 1, rm_crdt_make(1, 200));
    rm_write_block(&bso, 1, fso.bytes, fso.count); /* stamp-only, no tag 2 */
    append_block(&b, RM_BLOCK_SCENE_INFO, 0, 1, bso.bytes, bso.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(
        rm_edit_add_layer(s, 0, "Stale", (rm_item_id){1, 150}, nullptr) ==
            RM_ERR_INVALID_ARGUMENT,
        "RV43-R2-09 stamp-only 0x0D register blocks stale stamp (1:150 < 200)");
    RM_CHECK(rm_edit_delete_layer(s, rm_item_id_of(HIDDEN),
                                  (rm_item_id){1, 160}, nullptr) == RM_OK,
             "RV43-R2-09 unrelated delete_layer succeeds on stamp-only 0x0D");
    RM_CHECK(rm_edit_add_layer(s, 0, "Fresh", (rm_item_id){1, 500}, nullptr) ==
                 RM_OK,
             "RV43-R2-09 fresh add_layer succeeds on stamp-only 0x0D");
    size_t sz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &sz);
    RM_CHECK(buf_has_bytes(eb, sz, bso.bytes, bso.count),
             "RV43-R2-09 raw stamp-only 0x0D block bytes preserved verbatim");
    rm_edit_close(s);
    rm_buf bufs[] = {b, bvalid, fv, bso, fso};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-R2-10: Bounded text row truncated after tag 2 + author byte returns
   * RM_ERR_MALFORMED_PAGE (not RM_ERR_UNEXPECTED_EOF) on text replace. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, badrow = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_tag(&badrow, 2, RM_TAG_ID);
    rm_buf_append_byte(&badrow, 1); /* truncated before counter varuint */
    rm_write_varuint(&l, 1);
    rm_write_block(&l, 0, badrow.bytes, badrow.count);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, 0x07, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    rm_error err = {0};
    RM_CHECK(rm_edit_replace_text(s, 0, 0, "Hi", (rm_item_id){1, 50}, &err) ==
                     RM_ERR_MALFORMED_PAGE &&
                 err.status == RM_ERR_MALFORMED_PAGE,
             "RV43-R2-10 truncated row returns MALFORMED_PAGE (not EOF)");
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, badrow};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV43-R2-11: Equal non-nil left == right neighbors in rm_edit_insert_stroke
   * return RM_ERR_UNVERIFIED leaving revision and redo intact. */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_add_layer(s, 0, "RedoSeed", (rm_item_id){1, 500},
                               nullptr) == RM_OK &&
                 rm_edit_undo(s),
             "RV43-R2-11 redo seed");
    size_t pre_sz = 0;
    const uint8_t* pre_ptr = rm_edit_bytes(s, &pre_sz);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 40},
                                   (rm_item_id){1, 30}, (rm_item_id){1, 30},
                                   &good, nullptr) == RM_ERR_UNVERIFIED,
             "RV43-R2-11 equal non-nil neighbors refuse RM_ERR_UNVERIFIED");
    size_t post_sz = 0;
    RM_CHECK(rm_edit_bytes(s, &post_sz) == pre_ptr && post_sz == pre_sz &&
                 rm_edit_redo(s),
             "RV43-R2-11 revision and redo intact after refusal");
    rm_edit_close(s);
    rm_buf_free(&b);
  }
}

static void claude_rv45_regression_checks(void) {
  /* RV45-1 & RV46-09: legacy Form-1 glyphs tolerate truncated tail bytes after
   * tag 6, while Form-2 glyphs pass cleanly without trailing truncation and
   * refuse when trailing truncation in tag<=9 is added. */
  for (int mode = 0; mode < 4; ++mode) {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "abc\n", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

    rm_buf val = {0}, tbuf = {0}, rects = {0}, item = {0};
    rm_buf_append_byte(&val, RM_VALUE_GLYPH_ITEM);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_YELLOW);
    rm_write_string(&tbuf, "a");
    rm_write_block(&val, 5, tbuf.bytes, tbuf.count);
    rm_write_varuint(&rects, 1);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 10);
    rm_write_f64(&rects, 10);
    rm_write_block(&val, 6, rects.bytes, rects.count);
    if (mode == 0) {
      rm_buf_append_byte(&val, 0x80); /* Form-1 + stray unfinished tag */
    } else if (mode == 1) {
      rm_write_tag(&val, 2, RM_TAG_BYTE4);
      rm_write_u16(&val, 0x1234); /* Form-1 + truncated BYTE4 tag<=9 */
    } else if (mode == 2) {
      rm_write_id(&val, 7, rm_crdt_make(1, 10)); /* clean Form-2 control */
    } else {
      rm_write_id(&val, 7, rm_crdt_make(1, 10));
      rm_buf_append_byte(&val, 0x80); /* Form-2 + stray unfinished tag */
    }
    rm_write_id(&item, 1, SHOWN);
    rm_write_id(&item, 2, rm_crdt_make(1, 62));
    rm_write_id(&item, 3, RM_CRDT_NONE);
    rm_write_id(&item, 4, RM_CRDT_NONE);
    rm_write_tag(&item, 5, RM_TAG_BYTE4);
    rm_write_u32(&item, 0);
    rm_write_block(&item, 6, val.bytes, val.count);
    append_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, item.bytes, item.count);

    rm_edit_session* s = open_page(&b);
    rm_status st =
        rm_edit_replace_text(s, 4, 0, "!", (rm_item_id){2, 1}, nullptr);
    if (mode < 3) {
      RM_CHECK(st == RM_OK,
               "RV45-1/RV46-09 Form-1 truncated or clean Form-2 glyph allows "
               "edit (mode %d)",
               mode);
      rm_page* p = parse_session_bytes(s);
      RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
                   strcmp(p->text.paragraphs[0].text.bytes, "abc") == 0 &&
                   strcmp(p->text.paragraphs[1].text.bytes, "!") == 0,
               "RV45-1/RV46-09 reopened paragraphs updated (mode %d)", mode);
      rm_page_free(p);
    } else {
      RM_CHECK(st == RM_ERR_UNVERIFIED,
               "RV45-1/RV46-09 Form-2 glyph with trailing stray byte refuses");
    }
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, val, tbuf, rects, item};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV45-2: pre-existing stale Form-2 stored text allows safe unrelated edit
   * after hard break, while mutating highlighted slice still refuses. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, 1);
    append_text_row_item(&l, rm_crdt_make(1, 10), RM_CRDT_NONE, RM_CRDT_NONE, 0,
                         "abc\nnext", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

    append_glyph_block(&b, SHOWN, rm_crdt_make(1, 62), "stale", true,
                       rm_crdt_make(1, 10), true, rm_crdt_make(1, 12), true, 1);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 1, 1, "X", (rm_item_id){2, 1}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "RV45-2 mutating highlighted text with stale copy still refuses");
    RM_CHECK(
        rm_edit_replace_text(s, 4, 4, "later", (rm_item_id){2, 10}, nullptr) ==
            RM_OK,
        "RV45-2 safe edit after hard break succeeds despite stale glyph text");
    rm_page* p = parse_session_bytes(s);
    RM_CHECK(p && p->has_text && p->text.paragraph_count == 2 &&
                 strcmp(p->text.paragraphs[0].text.bytes, "abc") == 0 &&
                 strcmp(p->text.paragraphs[1].text.bytes, "later") == 0,
             "RV45-2 reopened paragraphs reflect safe edit");
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }
}

static void append_tree_place(rm_buf* b, rm_crdt_id op, rm_crdt_id node,
                              rm_crdt_id parent) {
  rm_buf rec = {0}, pbox = {0};
  rm_write_id(&rec, 1, op);
  rm_write_id(&rec, 2, node);
  rm_write_tag(&rec, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&rec, 1);
  rm_write_id(&pbox, 1, parent);
  rm_write_block(&rec, 4, pbox.bytes, pbox.count);
  append_block(b, RM_BLOCK_SCENE_TREE, 1, 1, rec.bytes, rec.count);
  rm_buf_free(&pbox);
  rm_buf_free(&rec);
}

static void append_group_link_after(rm_buf* b, rm_crdt_id parent,
                                    rm_crdt_id link, rm_crdt_id left,
                                    rm_crdt_id node) {
  rm_buf grp = {0}, val = {0};
  rm_write_id(&grp, 1, parent);
  rm_write_id(&grp, 2, link);
  rm_write_id(&grp, 3, left);
  rm_write_id(&grp, 4, RM_CRDT_NONE);
  rm_write_tag(&grp, 5, RM_TAG_BYTE4);
  rm_write_u32(&grp, 0);
  rm_buf_append_byte(&val, RM_VALUE_GROUP_ITEM);
  rm_write_id(&val, 2, node);
  rm_write_block(&grp, 6, val.bytes, val.count);
  append_block(b, RM_BLOCK_SCENE_GROUP_ITEM, 1, 1, grp.bytes, grp.count);
  rm_buf_free(&val);
  rm_buf_free(&grp);
}

static void codex_rv46_regression_checks(void) {
  /* RV46-01: authoritative RM_CRDT_ROOT walks before orphan links regardless of
   * file order, preserving [S, T] stroke order. */
  for (int orphan_first = 0; orphan_first < 2; ++orphan_first) {
    rm_buf b;
    start_page(&b);
    rm_crdt_id A = rm_crdt_make(1, 10), O = rm_crdt_make(1, 20),
               G = rm_crdt_make(1, 30), S = rm_crdt_make(1, 40),
               T = rm_crdt_make(1, 42);
    if (orphan_first) {
      append_nested_group(&b, O, rm_crdt_make(1, 50), G);
    }
    append_layer(&b, A, false, rm_crdt_make(1, 12));
    append_tree_place(&b, rm_crdt_make(1, 500), A, RM_CRDT_ROOT);
    append_stroke(&b, S, A, RM_TOOL_FINELINER_2);
    append_group_link_after(&b, A, rm_crdt_make(1, 41), S, G);
    append_stroke(&b, T, G, RM_TOOL_FINELINER_2);
    if (!orphan_first) {
      append_nested_group(&b, O, rm_crdt_make(1, 50), G);
    }
    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->stroke_count == 2 &&
                 rm_item_id_equal(p->strokes[0].id, rm_item_id_of(S)) &&
                 rm_item_id_equal(p->strokes[1].id, rm_item_id_of(T)),
             "RV46-01 root walks before orphan links (orphan_first=%d)",
             orphan_first);
    rm_page_free(p);
    rm_buf_free(&b);
  }

  /* RV46-02: shared group G reachable through surviving visible layer B stays
   * drawn and editable when sibling layer A is removed or hidden. */
  for (int hide_mode = 0; hide_mode < 2; ++hide_mode) {
    rm_buf b;
    start_page(&b);
    rm_crdt_id A = rm_crdt_make(1, 10), B = rm_crdt_make(1, 11),
               G = rm_crdt_make(1, 20), X = rm_crdt_make(1, 21),
               T = rm_crdt_make(1, 30), U = rm_crdt_make(1, 31);
    append_layer(&b, A, hide_mode == 1, rm_crdt_make(1, 12));
    append_layer(&b, B, false, rm_crdt_make(1, 13));
    append_nested_group(&b, A, rm_crdt_make(1, 40), G);
    append_nested_group(&b, B, rm_crdt_make(1, 41), G);
    append_nested_group(&b, A, rm_crdt_make(1, 42), X);
    append_stroke(&b, T, G, RM_TOOL_FINELINER_2);
    append_stroke(&b, U, X, RM_TOOL_FINELINER_2);
    if (hide_mode == 0) append_removal(&b, A);

    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->stroke_count == 1 &&
                 rm_item_id_equal(p->strokes[0].id, rm_item_id_of(T)),
             "RV46-02 shared group G stays visible/live (hide_mode=%d)",
             hide_mode);
    rm_page_free(p);

    if (hide_mode == 0) {
      rm_edit_session* s = open_page(&b);
      rm_draft_point pts[2] = {rm_draft_point_make(0, 0),
                               rm_draft_point_make(5, 5)};
      rm_draft_stroke st =
          rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, pts, 2);
      RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(X), (rm_item_id){2, 1},
                                     (rm_item_id){0}, (rm_item_id){0}, &st,
                                     nullptr) == RM_ERR_ITEMS_NOT_FOUND,
               "RV46-02 exclusively removed subgroup X refuses insert");
      RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(G), (rm_item_id){2, 2},
                                     (rm_item_id){0}, (rm_item_id){0}, &st,
                                     nullptr) == RM_OK,
               "RV46-02 shared subgroup G under live B accepts insert");
      rm_edit_close(s);
    }
    rm_buf_free(&b);
  }

  /* Two visible layers can both link the same group after a merge. */
  {
    rm_buf b;
    start_page(&b);
    append_two_layers(&b, false);
    rm_crdt_id group = rm_crdt_make(1, 90), stroke = rm_crdt_make(1, 91);
    append_nested_group(&b, SHOWN, rm_crdt_make(1, 92), group);
    append_nested_group(&b, HIDDEN, rm_crdt_make(1, 93), group);
    append_stroke(&b, stroke, group, RM_TOOL_FINELINER_2);

    rm_test_reset_chain_order_calls();
    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->stroke_count == 1 &&
                 rm_item_id_equal(p->strokes[0].id, rm_item_id_of(stroke)),
             "two visible parents draw a shared group's stroke once");
    RM_CHECK(
        rm_test_chain_order_calls() == 4,
        "the root, its two layers and their shared child are each walked once");
    rm_page_free(p);
    rm_buf_free(&b);
  }

  /* RV46-03: explicit in_tree=1 detachment with nil parent 0:0 suppresses G in
   * rendering, layers, and edit targeting until a later reattach arrives. */
  {
    rm_buf b;
    start_page(&b);
    rm_crdt_id A = rm_crdt_make(1, 10), G = rm_crdt_make(1, 20),
               T = rm_crdt_make(1, 30);
    append_layer(&b, A, false, rm_crdt_make(1, 12));
    append_tree_place(&b, rm_crdt_make(1, 80), A, RM_CRDT_ROOT);
    append_layer(&b, G, false, rm_crdt_make(1, 13));
    append_stroke(&b, T, G, RM_TOOL_FINELINER_2);
    append_tree_place(&b, rm_crdt_make(1, 81), G, RM_CRDT_NONE);

    rm_page* p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->stroke_count == 0,
             "RV46-03 detached 0:0 group is suppressed during render");
    rm_page_free(p);

    rm_edit_session* s = open_page(&b);
    rm_edit_layer* layers = nullptr;
    size_t lcount = 0;
    RM_CHECK(rm_edit_layers(s, &layers, &lcount, nullptr) == RM_OK &&
                 lcount == 1 &&
                 rm_item_id_equal(layers[0].node, rm_item_id_of(A)),
             "RV46-03 detached 0:0 layer excluded from rm_edit_layers");
    rm_edit_layers_free(layers);
    rm_draft_point pts[2] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(5, 5)};
    rm_draft_stroke st =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, pts, 2);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(G), (rm_item_id){2, 1},
                                   (rm_item_id){0}, (rm_item_id){0}, &st,
                                   nullptr) == RM_ERR_ITEMS_NOT_FOUND,
             "RV46-03 detached 0:0 layer refuses insert");
    rm_edit_close(s);

    append_tree_place(&b, rm_crdt_make(1, 82), G, RM_CRDT_ROOT);
    p = parse(b.bytes, b.count);
    RM_CHECK(p && p->skipped_blocks == 0 && p->stroke_count == 1 &&
                 rm_item_id_equal(p->strokes[0].id, rm_item_id_of(T)),
             "RV46-03 reattached layer draws again");
    rm_page_free(p);
    s = open_page(&b);
    RM_CHECK(
        rm_edit_layers(s, &layers, &lcount, nullptr) == RM_OK && lcount == 2,
        "RV46-03 reattached layer visible in rm_edit_layers");
    rm_edit_layers_free(layers);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(G), (rm_item_id){2, 2},
                                   (rm_item_id){0}, (rm_item_id){0}, &st,
                                   nullptr) == RM_OK,
             "RV46-03 reattached layer accepts insert");
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* RV46-04: inserting immediately before a later split fragment of a living
   * extended row refuses RM_ERR_UNVERIFIED instead of splitting source row. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_varuint(&l, 2);
    /* Extended row 1:10..1:11 ("ab" + trailing unknown tag 9 BYTE1=7). */
    rm_buf row = {0}, val = {0};
    rm_write_id(&row, 2, rm_crdt_make(1, 10));
    rm_write_id(&row, 3, RM_CRDT_NONE);
    rm_write_id(&row, 4, RM_CRDT_NONE);
    rm_write_tag(&row, 5, RM_TAG_BYTE4);
    rm_write_u32(&row, 0);
    rm_write_varuint(&val, 2);
    rm_buf_append_byte(&val, 1);
    rm_buf_append(&val, "ab", 2);
    rm_write_tag(&val, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&val, 7);
    rm_write_block(&row, 6, val.bytes, val.count);
    rm_write_block(&l, 0, row.bytes, row.count);
    rm_buf_free(&val);
    rm_buf_free(&row);
    /* Peer insert "X" splitting 1:10 ("a") and 1:11 ("b"). */
    append_text_row_item(&l, rm_crdt_make(2, 1), rm_crdt_make(1, 10),
                         rm_crdt_make(1, 11), 0, "X", 1);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 3, 0, "!", (rm_item_id){3, 1}, nullptr) ==
                     RM_OK &&
                 rm_edit_undo(s),
             "RV46-04 redo seed");
    size_t pre_sz = 0;
    const uint8_t* pre_ptr = rm_edit_bytes(s, &pre_sz);
    RM_CHECK(rm_edit_replace_text(s, 2, 0, "Y", (rm_item_id){3, 2}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "RV46-04 interior insert before 2nd fragment of extended row "
             "refuses");
    size_t post_sz = 0;
    RM_CHECK(rm_edit_bytes(s, &post_sz) == pre_ptr && post_sz == pre_sz &&
                 rm_edit_redo(s),
             "RV46-04 revision and redo intact after refusal");
    RM_CHECK(rm_edit_replace_text(s, 0, 0, "Z", (rm_item_id){3, 5}, nullptr) ==
                 RM_OK,
             "RV46-04 control prepend before 1st fragment succeeds");
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV46-06: empty selection (count == 0) in rm_edit_transform_strokes returns
   * a validation error for an unusable matrix and keeps redo. */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&b, rm_crdt_make(1, 30), SHOWN, RM_TOOL_FINELINER_2);
    rm_edit_session* s = open_page(&b);
    rm_draft_point pts[2] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(5, 5)};
    rm_draft_stroke st =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, pts, 2);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(SHOWN), (rm_item_id){1, 40},
                                   (rm_item_id){0}, (rm_item_id){0}, &st,
                                   nullptr) == RM_OK &&
                 rm_edit_undo(s),
             "RV46-06 redo seed");
    size_t pre_sz = 0;
    const uint8_t* pre_ptr = rm_edit_bytes(s, &pre_sz);
    RM_CHECK(rm_edit_transform_strokes(s, nullptr, 0, (rm_item_id){1, 90},
                                       (rm_edit_transform){0, 0, 0, 0, 0, 0},
                                       nullptr) == RM_ERR_INVALID_ARGUMENT,
             "empty selection refuses a singular matrix");
    RM_CHECK(rm_edit_transform_strokes(s, nullptr, 0, (rm_item_id){1, 90},
                                       (rm_edit_transform){1, 0, 0, 1, NAN, 0},
                                       nullptr) == RM_ERR_INVALID_ARGUMENT,
             "empty selection refuses a nonfinite translation");
    size_t post_sz = 0;
    RM_CHECK(rm_edit_bytes(s, &post_sz) == pre_ptr && post_sz == pre_sz &&
                 rm_edit_redo(s),
             "RV46-06 revision and redo preserved");
    rm_edit_close(s);
    rm_buf_free(&b);
  }

  /* RV46-05: untouched extended text row with non-minimal varuint envelope tag
   * (0x8c 0x00) preserves exact header + body bytes across unrelated append. */
  {
    rm_buf b, l = {0}, in = {0}, it = {0}, root = {0}, pos = {0}, row = {0},
              val = {0}, ext_env = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_write_id(&row, 2, rm_crdt_make(1, 10));
    rm_write_id(&row, 3, RM_CRDT_NONE);
    rm_write_id(&row, 4, RM_CRDT_NONE);
    rm_write_tag(&row, 5, RM_TAG_BYTE4);
    rm_write_u32(&row, 0);
    rm_write_varuint(&val, 2);
    rm_buf_append_byte(&val, 1);
    rm_buf_append(&val, "ab", 2);
    rm_write_tag(&val, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&val, 7);
    rm_write_block(&row, 6, val.bytes, val.count);
    rm_buf_append_byte(&ext_env, 0x8c);
    rm_buf_append_byte(&ext_env, 0x00);
    rm_write_u32(&ext_env, (uint32_t)row.count);
    rm_buf_append(&ext_env, row.bytes, row.count);
    rm_write_varuint(&l, 1);
    rm_buf_append(&l, ext_env.bytes, ext_env.count);
    rm_write_block(&in, 1, l.bytes, l.count);
    rm_write_block(&it, 1, in.bytes, in.count);
    rm_write_id(&root, 1, RM_CRDT_NONE);
    rm_write_block(&root, 2, it.bytes, it.count);
    rm_write_f64(&pos, -468);
    rm_write_f64(&pos, 234);
    rm_write_block(&root, 3, pos.bytes, pos.count);
    append_block(&b, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

    rm_edit_session* s = open_page(&b);
    RM_CHECK(rm_edit_replace_text(s, 2, 0, "!", (rm_item_id){2, 1}, nullptr) ==
                 RM_OK,
             "RV46-05 unrelated append preserves non-minimal extended row");
    size_t out_sz = 0;
    const uint8_t* out_b = rm_edit_bytes(s, &out_sz);
    bool exact_env_found = false;
    if (out_b && out_sz >= ext_env.count) {
      for (size_t off = 0; off <= out_sz - ext_env.count; ++off) {
        if (memcmp(out_b + off, ext_env.bytes, ext_env.count) == 0) {
          exact_env_found = true;
          break;
        }
      }
    }
    RM_CHECK(
        exact_env_found,
        "RV46-05 non-minimal 0x8c 0x00 envelope + body preserved verbatim");
    rm_edit_close(s);
    rm_buf bufs[] = {b, l, in, it, root, pos, row, val, ext_env};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV46-2 (Claude): transform and cut preserve relative wire order of tail
   * fields [6, 10, 7, 9] inside 0x05 value. */
  {
    rm_buf b, val = {0}, pts = {0}, line = {0};
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    rm_buf_append_byte(&val, RM_VALUE_LINE_ITEM);
    rm_write_tag(&val, 1, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_TOOL_SHADER);
    rm_write_tag(&val, 2, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_HIGHLIGHT);
    rm_write_tag(&val, 3, RM_TAG_BYTE8);
    rm_write_f64(&val, 1.0);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_f32(&val, 0.0f);
    for (int i = 0; i < 3; ++i) {
      rm_write_f32(&pts, (float)i * 10.0f);
      rm_write_f32(&pts, 0.0f);
      rm_write_u16(&pts, 0);
      rm_write_u16(&pts, 8);
      rm_buf_append_byte(&pts, 0);
      rm_buf_append_byte(&pts, 255);
    }
    rm_write_block(&val, 5, pts.bytes, pts.count);
    rm_write_id(&val, 6, rm_crdt_make(1, 5));
    rm_write_tag(&val, 10, RM_TAG_BYTE4);
    rm_write_u32(&val, 0x12345678u);
    rm_write_id(&val, 7, rm_crdt_make(1, 6));
    rm_write_id(&val, 9, rm_crdt_make(1, 7));

    rm_write_id(&line, 1, SHOWN);
    rm_write_id(&line, 2, rm_crdt_make(1, 30));
    rm_write_id(&line, 3, RM_CRDT_NONE);
    rm_write_id(&line, 4, RM_CRDT_NONE);
    rm_write_tag(&line, 5, RM_TAG_BYTE4);
    rm_write_u32(&line, 0);
    rm_write_block(&line, 6, val.bytes, val.count);
    append_block(&b, RM_BLOCK_SCENE_LINE_ITEM, 2, 2, line.bytes, line.count);

    rm_edit_session* s = open_page(&b);
    for (int op = 0; op < 2; ++op) {
      rm_item_id fresh = op == 0 ? (rm_item_id){1, 40} : (rm_item_id){1, 50};
      rm_status st = op == 0
                         ? rm_edit_transform_stroke(
                               s, (rm_item_id){1, 30}, fresh,
                               (rm_edit_transform){1, 0, 0, 1, 2, 3}, nullptr)
                         : rm_edit_cut_stroke(s, (rm_item_id){1, 30}, fresh, 1,
                                              1, nullptr);
      RM_CHECK(st == RM_OK, "RV46-2 stroke op=%d succeeds", op);
      size_t bsz = 0;
      const uint8_t* bptr = rm_edit_bytes(s, &bsz);
      uint32_t tags[4] = {0};
      size_t ntags = 0;
      uint32_t tag10_val = 0;
      rm_reader rd = {.data = bptr, .size = bsz, .offset = RM_HEADER_LENGTH};
      rm_block_header hdr;
      while (rm_reader_remaining(&rd) >= 8 &&
             rm_read_block_header(&rd, &hdr) == RM_OK) {
        if (hdr.type == RM_BLOCK_SCENE_LINE_ITEM) {
          rm_reader body = rd;
          body.size = hdr.end;
          rm_item_preamble pre;
          rm_reader v;
          if (rm_parse_item_preamble(&body, &pre) == RM_OK &&
              pre.deleted == 0 && rm_crdt_equal(pre.id, rm_crdt_of(fresh)) &&
              rm_match_tag(&body, 6, RM_TAG_LENGTH4) &&
              rm_read_subblock(&body, &v) == RM_OK) {
            uint8_t k;
            uint32_t t, col;
            double th;
            float sl;
            rm_reader p;
            (void)rm_read_u8(&v, &k);
            (void)rm_expect_tag(&v, 1, RM_TAG_BYTE4);
            (void)rm_read_u32(&v, &t);
            (void)rm_expect_tag(&v, 2, RM_TAG_BYTE4);
            (void)rm_read_u32(&v, &col);
            (void)rm_expect_tag(&v, 3, RM_TAG_BYTE8);
            (void)rm_read_f64(&v, &th);
            (void)rm_expect_tag(&v, 4, RM_TAG_BYTE4);
            (void)rm_read_f32(&v, &sl);
            (void)rm_expect_tag(&v, 5, RM_TAG_LENGTH4);
            (void)rm_read_subblock(&v, &p);
            while (rm_reader_remaining(&v) > 0 && ntags < 4) {
              uint32_t tg;
              uint8_t tp;
              if (rm_read_tag(&v, &tg, &tp) != RM_OK) break;
              tags[ntags++] = tg;
              if (tg == 10 && tp == RM_TAG_BYTE4)
                (void)rm_read_u32(&v, &tag10_val);
              else
                (void)rmi_skip_tagged_value(&v, tp, nullptr);
            }
            break;
          }
        }
        rd.offset = hdr.end;
      }
      RM_CHECK(ntags == 4 && tags[0] == 6 && tags[1] == 10 && tags[2] == 7 &&
                   tags[3] == 9 && tag10_val == 0x12345678u,
               "RV46-2 op=%d keeps tail tag order [6,10,7,9]", op);
      if (op == 0) RM_CHECK(rm_edit_undo(s), "RV46-2 undo before cut");
    }
    rm_edit_close(s);
    rm_buf bufs[] = {b, val, pts, line};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; ++i)
      rm_buf_free(&bufs[i]);
  }

  /* RV46-3 (Claude): ADD_LAYER on placement-free page refuses RM_ERR_UNVERIFIED
   * when a stroke already uses a surviving fallback layer's node ID. */
  {
    rm_buf b;
    start_page(&b);
    append_layer(&b, SHOWN, false, rm_crdt_make(1, 12));
    append_stroke(&b, SHOWN, SHOWN, RM_TOOL_FINELINER_2);
    rm_edit_session* s = open_page(&b);
    size_t pre_sz = 0;
    const uint8_t* pre_ptr = rm_edit_bytes(s, &pre_sz);
    RM_CHECK(rm_edit_add_layer(s, 1, "New", (rm_item_id){2, 1}, nullptr) ==
                 RM_ERR_UNVERIFIED,
             "RV46-3 refuses materializing fallback layer ID colliding with "
             "stroke ID");
    size_t post_sz = 0;
    RM_CHECK(rm_edit_bytes(s, &post_sz) == pre_ptr && post_sz == pre_sz,
             "RV46-3 failed add_layer preserves session revision");
    rm_edit_close(s);
    rm_buf_free(&b);
  }
}

int main(void) {
  inserts_into_layer_without_metadata();
  anchor_survives_peer_rename();
  moving_layer_with_unlinked_descendant();
  unlinked_layer_counts_toward_limit();
  replacing_anchored_character();
  glyphs_follow_layer_visibility();
  stroke_replacement_order();
  removed_stroke_parents();
  repeated_current_layer_stamps();
  historical_active_layers();
  interleaved_page_counts();
  stroke_point_allocation_overflow();
  replacing_styled_paragraph_break();
  text_edit_neighbours();
  written_and_edited_text_counts();
  replace_text_around_carriage_return();
  editing_repaired_text_and_face_markers();
  edit_preserves_nested_error();
  migration_version_and_string_bounds();
  deletion_preserves_live_active_layer();
  layer_links_and_registers();
  stroke_selection_and_reservations();
  edit_revisions();
  a_tombstone_before_a_108_byte_block_is_a_tombstone();
  a_highlight_takes_no_colour_from_the_block_after_it();
  a_deleted_highlight_with_a_value_is_not_shown();
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
  a_layer_taken_out_and_put_back_is_drawn_and_searched_again();
  a_group_moved_under_a_layer_later_removed_goes_with_it();
  a_group_moved_out_of_a_layer_later_removed_stays();
  a_group_moved_and_linked_under_its_new_layer_is_drawn_once();
  a_tree_record_whose_parent_runs_past_its_block_does_not_read();
  a_page_with_a_block_that_will_not_read_is_refused_not_answered_in_part();
  highlights_over_a_text_layer_are_found_with_the_strokes();
  transform_rejects_nonfinite_translation_on_empty_stroke();
  internal_transform_checks_empty_selections();
  id_check_reports_a_block_past_eof();
  id_collision_checks_cover_all_blocks_and_node_stamps();
  replace_text_ignores_anchors_on_removed_layers();
  out_of_order_and_bare_tags_in_node_and_scene_info();
  deleted_text_row_with_nonempty_value_is_ignored();
  node_and_layer_names_repair_ill_formed_utf8();
  replace_text_protects_anchors_on_live_hidden_layers();
  unstamped_style_with_preceding_field_survives_replace();
  lww_extension_singular_transform_and_out_of_order_stroke();
  highlighter_order_survives_multi_item_tombstone();
  noop_edits_preserve_redo_history();
  duplicate_live_stroke_id_and_collision_message();
  unstamped_visibility_register_is_editable();
  scene_info_raw_size_no_false_collision_and_rename_oom();
  a2_deleted_group_item_reserves_only_valid_group_child();
  a4_editor_limits_palette_shader_cut_transforms_and_cycles();
  a5_node_edit_truncations_and_unstamped_registers();
  surplus_row_counts_must_agree();
  a5_root_text_edit_edge_cases_and_preferred_layout();
  a5_sentinels_removed_anchors_and_glyph_remapping();
  unreadable_glyph_refuses_text_deletion();
  claude_rv43_regression_checks();
  codex_rv43_r2_regression_checks();
  claude_rv45_regression_checks();
  codex_rv46_regression_checks();
  return rm_test_report("editor");
}
