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
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
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
}
static void edit_preserves_nested_error(void) {
  rm_buf page;
  start_page(&page);
  append_layer(&page, SHOWN, false, rm_crdt_make(1, 12));
  rm_edit_session* s = open_page(&page);
  /* Inject a version failure after open, at the nested read inside the edit. */
  size_t size;
  uint8_t* data = (uint8_t*)rm_edit_bytes(s, &size);
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
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
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
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
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
static void editing_repaired_text_and_face_markers(void) {
  rm_draft_paragraph paragraphs[] = {{.bold = true}};
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
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
    rm_item_id id = {1, 800 + i}, fresh = {2, 200};
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

static rm_page* parse_session_bytes(rm_edit_session* session) {
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(session, &size);
  return parse(bytes, size);
}

static uint8_t* anchored_page(const char* first, const char* second,
                              size_t* size) {
  rm_draft_paragraph paragraphs[] = {{.text = first}, {.text = second}};
  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(40, 0)};
  rm_draft_mark mark = {.anchor = {RM_ANCHOR_FIRST_CHARACTER, 1},
                        .stroke = rm_draft_stroke_make(
                            RM_TOOL_FINELINER_2, RM_COLOR_BLACK, points, 2)};
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
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

int main(void) {
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
  return rm_test_report("editor");
}
