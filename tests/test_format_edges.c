/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The less common shapes the format can carry: the second form of a highlight,
 * the highlighter's own indexes for blue and orange, the nested ticked
 * checkbox, the markers of typed text whose string is not text, and the stamped
 * column width. The first half builds pages by hand to the layout the format
 * defines; the second reads sample pages written on the tablet. */

#include <float.h>
#include <math.h>

#include "rm_damaged.h"
#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

/* ------------------------------------------------------------------ */
/* Pages built by hand                                                 */
/* ------------------------------------------------------------------ */

static void start_page(rm_buf* page) {
  rm_buf_init(page);
  rm_buf_append(page, RM_HEADER, RM_HEADER_LENGTH);
}

static rm_page* parse(const uint8_t* bytes, size_t size) {
  rm_page* page = nullptr;
  if (rm_parse_page(bytes, size, &page, nullptr) != RM_OK) return nullptr;
  return page;
}

/* One record of the text list: a string, and the marker code when there is
 * one — the tablet writes the code only when it is not nought. */
typedef struct record {
  rm_crdt_id id;
  rm_crdt_id left;
  const char* text;
  bool has_code;
  uint32_t code;
  uint32_t deleted;
  /* Bytes the value claims beyond what it holds, and the bytes that stand
   * after the record in the list — a malformed record, for the bounds. */
  uint32_t overclaim;
  const uint8_t* trailing;
  size_t trailing_count;
  rm_crdt_id right;
} record;

/* The root text block: the records, the box, the legacy width and — when
 * `settings_width` is not NAN — the stamped one of the settings. */
static void append_root_text_declaring(rm_buf* page, const record* records,
                                       size_t count, size_t declared,
                                       float width, float settings_width) {
  rm_buf list = {0}, inner = {0}, items = {0}, text = {0}, position = {0};
  rm_write_varuint(&list, declared);
  for (size_t i = 0; i < count; i += 1) {
    rm_buf value = {0}, item = {0};
    rm_write_string(&value, records[i].text);
    if (records[i].has_code) {
      rm_write_tag(&value, 2, RM_TAG_BYTE4);
      rm_write_u32(&value, records[i].code);
    }
    rm_write_id(&item, 2, records[i].id);
    rm_write_id(&item, 3, records[i].left);
    rm_write_id(&item, 4, records[i].right);
    rm_write_tag(&item, 5, RM_TAG_BYTE4);
    rm_write_u32(&item, records[i].deleted);
    rm_write_tag(&item, 6, RM_TAG_LENGTH4);
    rm_write_u32(&item, (uint32_t)value.count + records[i].overclaim);
    rm_buf_append(&item, value.bytes, value.count);
    rm_write_block(&list, 0, item.bytes, item.count);
    rm_buf_append(&list, records[i].trailing, records[i].trailing_count);
    rm_buf_free(&value);
    rm_buf_free(&item);
  }
  rm_write_block(&inner, 1, list.bytes, list.count);
  rm_write_block(&items, 1, inner.bytes, inner.count);

  rm_write_id(&text, 1, RM_CRDT_NONE);
  rm_write_block(&text, 2, items.bytes, items.count);
  rm_write_f64(&position, (double)(-width / 2));
  rm_write_f64(&position, 234);
  rm_write_block(&text, 3, position.bytes, position.count);
  rm_write_tag(&text, 4, RM_TAG_BYTE4);
  rm_write_f32(&text, width);
  if (!isnan(settings_width)) {
    rm_buf stamped = {0};
    rm_write_stamped_f32(&stamped, 1, rm_crdt_make(1, 2), settings_width);
    rm_write_block(&text, 5, stamped.bytes, stamped.count);
    rm_buf_free(&stamped);
  }
  rm_write_top_block(page, 0x07, 1, 1, text.bytes, text.count);
  rm_buf_free(&list);
  rm_buf_free(&inner);
  rm_buf_free(&items);
  rm_buf_free(&text);
  rm_buf_free(&position);
}

static void append_root_text(rm_buf* page, const record* records, size_t count,
                             float width, float settings_width) {
  append_root_text_declaring(page, records, count, count, width,
                             settings_width);
}

/* The page of these records, parsed. Null when it does not parse. */
static rm_page* text_page(const record* records, size_t count, float width,
                          float settings_width) {
  rm_buf page;
  start_page(&page);
  append_root_text(&page, records, count, width, settings_width);
  rm_page* parsed = parse(page.bytes, page.count);
  rm_buf_free(&page);
  return parsed;
}

/* The paragraphs of a page against the lines expected, one by one. */
static void expect_lines(const rm_page* page, const char* const* lines,
                         size_t count, const char* name) {
  RM_CHECK(page != nullptr && page->has_text, "%s: the page has text", name);
  if (page == nullptr || !page->has_text) return;
  RM_CHECK_EQ_SIZE(page->text.paragraph_count, count, "%s: the paragraphs",
                   name);
  for (size_t i = 0; i < count && i < page->text.paragraph_count; i += 1) {
    RM_CHECK_EQ_STR(page->text.paragraphs[i].text.bytes,
                    page->text.paragraphs[i].text.length, lines[i],
                    "%s: line %zu", name, i);
  }
}

static void link_markers_are_not_text(void) {
  /* `ab`, the link `cd` and `e`. The opening marker's string is the address;
   * read as text it would land on the page, and counted as characters it
   * would move the cursor past `cd`'s `left`. Shuffled, so that only the
   * `left` links can put them back in order. */
  const record records[] = {
      {.id = {13, 1}, .left = {12, 1}, .text = "cd"},
      {.id = {16, 1}, .left = {15, 1}, .text = "e"},
      {.id = {10, 1}, .left = {0, 0}, .text = "ab"},
      {.id = {15, 1}, .left = {14, 1}, .text = "", .has_code = true, .code = 6},
      {.id = {12, 1},
       .left = {11, 1},
       .text = "https://example.com",
       .has_code = true,
       .code = 5},
  };
  rm_page* page = text_page(records, 5, 936, NAN);
  expect_lines(page, (const char* const[]){"abcde"}, 1, "link");
  rm_page_free(page);
}

static void an_inline_image_is_one_object_replacement_character(void) {
  const record records[] = {
      {.id = {20, 1}, .left = {0, 0}, .text = "x\n"},
      {.id = {22, 1},
       .left = {21, 1},
       .text = "{0a418e1a-1111-4222-8333-444455556666}",
       .has_code = true,
       .code = 0xFFFF'FFFFu},
      {.id = {23, 1}, .left = {22, 1}, .text = "\ny"},
  };
  rm_page* page = text_page(records, 3, 936, NAN);
  expect_lines(page, (const char* const[]){"x", "\xEF\xBF\xBC", "y"}, 3,
               "inline image");
  rm_page_free(page);
}

static void a_code_of_nought_is_plain_text(void) {
  const record records[] = {
      {.id = {30, 1},
       .left = {0, 0},
       .text = "plain",
       .has_code = true,
       .code = 0},
  };
  rm_page* page = text_page(records, 1, 936, NAN);
  expect_lines(page, (const char* const[]){"plain"}, 1, "code nought");
  rm_page_free(page);
}

static void a_value_that_claims_more_than_its_record_finds_no_marker_past_it(
    void) {
  /* `24 05 00 00 00` — a link's opening code — lying past the record's end,
   * inside the list. Read as this record's marker it would turn `plain` into
   * nothing. */
  static const uint8_t trailing[] = {0x24, 0x05, 0, 0, 0};
  const record records[] = {
      {.id = {60, 1},
       .left = {0, 0},
       .text = "plain",
       .overclaim = 5,
       .trailing = trailing,
       .trailing_count = sizeof trailing},
  };
  rm_page* page = text_page(records, 1, 936, NAN);
  expect_lines(page, (const char* const[]){"plain"}, 1, "overclaim");
  rm_page_free(page);
}

static void the_stamped_width_of_the_settings_wins_over_the_legacy_one(void) {
  const record records[] = {{.id = {40, 1}, .left = {0, 0}, .text = "w"}};
  struct {
    float width, settings, expected;
  } cases[] = {
      {936, 1152, 1152},
      {936, NAN, 936},
      /* A settings width that is no width leaves the legacy one standing. */
      {740, 0, 740},
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i += 1) {
    rm_page* page = text_page(records, 1, cases[i].width, cases[i].settings);
    RM_CHECK(page != nullptr && page->has_text && page->text.has_column_width &&
                 page->text.column_width == (double)cases[i].expected,
             "case %zu: the width is %g", i, (double)cases[i].expected);
    rm_page_free(page);
  }
}

/* A highlight's block: the preamble, then the value with the colour, one
 * rectangle and whatever tail the case asks for. */
static void append_glyph(rm_buf* page, uint32_t color, bool first_last,
                         bool has_argb, uint32_t argb) {
  rm_buf rectangles = {0}, value = {0}, item = {0};
  rm_write_varuint(&rectangles, 1);
  const double numbers[] = {10, 20, 30, 40};
  for (size_t i = 0; i < 4; i += 1) rm_write_f64(&rectangles, numbers[i]);

  rm_buf_append_byte(&value, 1); /* value type: glyph */
  if (!first_last) {
    rm_write_tag(&value, 2, RM_TAG_BYTE4);
    rm_write_u32(&value, 0);
    rm_write_tag(&value, 3, RM_TAG_BYTE4);
    rm_write_u32(&value, 4);
  }
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_u32(&value, color);
  rm_write_block(&value, 6, rectangles.bytes, rectangles.count);
  if (first_last) {
    rm_write_id(&value, 7, rm_crdt_make(1, 300));
    rm_write_id(&value, 8, rm_crdt_make(1, 304));
    rm_write_tag(&value, 9, RM_TAG_BYTE1);
    rm_buf_append_byte(&value, 1);
  }
  if (has_argb) {
    rm_write_tag(&value, 10, RM_TAG_BYTE4);
    rm_write_u32(&value, argb);
  }

  rm_write_id(&item, 1, rm_crdt_make(0, 11));
  rm_write_id(&item, 2, rm_crdt_make(1, 50));
  rm_write_id(&item, 3, RM_CRDT_NONE);
  rm_write_id(&item, 4, RM_CRDT_NONE);
  rm_write_tag(&item, 5, RM_TAG_BYTE4);
  rm_write_u32(&item, 0);
  rm_write_block(&item, 6, value.bytes, value.count);
  rm_write_top_block(page, 0x03, 1, 1, item.bytes, item.count);
  rm_buf_free(&rectangles);
  rm_buf_free(&value);
  rm_buf_free(&item);
}

static rm_page* glyph_page(uint32_t color, bool first_last, bool has_argb,
                           uint32_t argb) {
  rm_buf page;
  start_page(&page);
  append_glyph(&page, color, first_last, has_argb, argb);
  rm_page* parsed = parse(page.bytes, page.count);
  rm_buf_free(&page);
  return parsed;
}

static void both_forms_of_a_highlight_keep_their_explicit_colour(void) {
  for (int first_last = 0; first_last < 2; first_last += 1) {
    rm_page* page = glyph_page(9, first_last, true, 0xFFBE'EAFEu);
    RM_CHECK(page != nullptr && page->highlight_count == 1,
             "form %d: one highlight", first_last);
    if (page != nullptr && page->highlight_count == 1) {
      const rm_highlight* highlight = &page->highlights[0];
      RM_CHECK(highlight->has_argb && highlight->argb == 0xFFBE'EAFEu,
               "form %d: the colour is kept", first_last);
      RM_CHECK(highlight->rectangle_count == 1 &&
                   highlight->rectangles[0].x == 10 &&
                   highlight->rectangles[0].y == 20 &&
                   highlight->rectangles[0].width == 30 &&
                   highlight->rectangles[0].height == 40,
               "form %d: the rectangle", first_last);
      RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "form %d: nothing lost",
                       first_last);
    }
    rm_page_free(page);
  }
}

static void the_highlighters_blue_and_orange_read_under_their_own_indexes(
    void) {
  rm_page* blue = glyph_page(14, true, false, 0);
  RM_CHECK(blue != nullptr && blue->highlight_count == 1 &&
               blue->highlights[0].color == RM_COLOR_HIGHLIGHTER_BLUE &&
               !blue->highlights[0].has_argb,
           "a highlight of index 14 is the highlighter's blue");
  rm_page_free(blue);

  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(10, 0)};
  rm_draft_stroke stroke = {
      .tool = RM_TOOL_HIGHLIGHTER_2,
      .color = RM_COLOR_HIGHLIGHTER_ORANGE,
      .thickness = 1,
      .points = points,
      .point_count = 2,
  };
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .strokes = &stroke,
      .stroke_count = 1,
  };
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_status status = rm_write_page(&draft, &bytes, &size, nullptr);
  RM_CHECK(status == RM_OK, "the page is written (%s)",
           rm_status_string(status));
  rm_page* written = status == RM_OK ? parse(bytes, size) : nullptr;
  RM_CHECK(written != nullptr && written->stroke_count == 1 &&
               written->strokes[0].color == RM_COLOR_HIGHLIGHTER_ORANGE,
           "the orange reads back as itself");
  rm_page_free(written);
  rm_bytes_free(bytes);
}

static void gray_marker_and_masked_eraser(void) {
  rm_page* page = glyph_page(16, true, false, 0);
  RM_CHECK(page && page->highlight_count == 1 &&
               page->highlights[0].color == RM_COLOR_HIGHLIGHTER_GRAY,
           "index 16 is the gray text highlight");
  rm_page_free(page);
  rm_draft_point point = rm_draft_point_make(3, 4);
  rm_draft_stroke stroke = rm_draft_stroke_make(
      RM_TOOL_MASKED_ERASER, RM_COLOR_HIGHLIGHTER_GRAY, &point, 1);
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1}, .strokes = &stroke, .stroke_count = 1};
  uint8_t* bytes = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &bytes, &size, nullptr) == RM_OK, "write stroke");
  page = bytes ? parse(bytes, size) : nullptr;
  RM_CHECK(page && page->stroke_count == 1 &&
               page->strokes[0].color == RM_COLOR_HIGHLIGHTER_GRAY &&
               page->strokes[0].tool == RM_TOOL_MASKED_ERASER &&
               rm_pen_tool_is_eraser(page->strokes[0].tool),
           "tool 22 round-trips as an eraser");
  rm_page_free(page);
  rm_bytes_free(bytes);
}

static void minimum_block_version(void) {
  rm_buf file;
  start_page(&file);
  rm_write_top_block(&file, 13, 2, 2, nullptr, 0);
  rm_page* page = nullptr;
  rm_error error;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &page, &error) ==
               RM_ERR_UNSUPPORTED_BLOCK_VERSION && error.value == 13 &&
               error.version == 2 && !page, "incompatible SceneInfo");
  file.bytes[RM_HEADER_LENGTH + 5] = 1;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &page, &error) == RM_OK,
           "compatible extension");
  rm_page_free(page);
  rm_buf_free(&file);
}

static void a_nested_ticked_checkbox_is_a_checkbox_item_and_keeps_its_level(
    void) {
  rm_paragraph_format nested = {.raw = 9,
                                .has_second = true,
                                .second = 3,
                                .has_third = true,
                                .third = 0x12};
  RM_CHECK(
      rm_paragraph_format_style(&nested) == RM_PARA_CHECKBOX_CHECKED_NESTED,
      "style 9");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_CHECKBOX_CHECKED_NESTED) ==
               RM_LIST_CHECKBOX,
           "a checkbox");
  RM_CHECK(rm_paragraph_style_is_list_item(RM_PARA_CHECKBOX_CHECKED_NESTED),
           "a list item");
  RM_CHECK(rm_paragraph_format_level(&nested) == 3, "the third level");
  rm_paragraph_format image = {.raw = 12};
  RM_CHECK(rm_paragraph_format_style(&image) == RM_PARA_IMAGE_BLOCK,
           "style 12");
  RM_CHECK(!rm_paragraph_style_is_list_item(RM_PARA_IMAGE_BLOCK),
           "not a list item");
}

static void without_state_bits_a_parent_reads_its_nested_items(void) {
  /* A page from before the state bits: the parent says nothing, its nested
   * item is ticked by style alone. */
  const rm_paragraph paragraphs[] = {
      {.text = {"parent", 6}, .has_format = true, .format = {.raw = 6}},
      {.text = {"child", 5},
       .has_format = true,
       .format = {.raw = 9,
                  .has_second = true,
                  .second = 3,
                  .has_third = true,
                  .third = 0x011}},
      {.text = {"old-style ticked", 16},
       .has_format = true,
       .format = {.raw = 7}},
  };
  rm_text_block text = {.paragraphs = paragraphs, .paragraph_count = 3};
  RM_CHECK(rm_text_block_checkbox_state(&text, 0) == RM_CHECKBOX_PARTIAL,
           "the parent");
  RM_CHECK(rm_text_block_checkbox_state(&text, 1) == RM_CHECKBOX_TICKED,
           "the child");
  RM_CHECK(rm_text_block_checkbox_state(&text, 2) == RM_CHECKBOX_TICKED,
           "the old style");
  RM_CHECK(rm_text_block_checkbox_state(&text, 3) == RM_CHECKBOX_NONE,
           "past the last line");
}

/* ------------------------------------------------------------------ */
/* Fields cut short, and markers among the faces                       */
/* ------------------------------------------------------------------ */

static void a_marker_code_cut_short_leaves_the_record_text(void) {
  /* The value ends two bytes into the code: no code, and `ab` stays. */
  rm_buf page, list = {0}, inner = {0}, items = {0}, text = {0}, position = {0};
  rm_buf value = {0}, item = {0};
  rm_write_string(&value, "ab");
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_buf_append(&value, (const uint8_t[]){5, 0}, 2);
  rm_write_id(&item, 2, rm_crdt_make(1, 80));
  rm_write_id(&item, 3, RM_CRDT_NONE);
  rm_write_id(&item, 4, RM_CRDT_NONE);
  rm_write_tag(&item, 5, RM_TAG_BYTE4);
  rm_write_u32(&item, 0);
  rm_write_block(&item, 6, value.bytes, value.count);
  rm_write_varuint(&list, 1);
  rm_write_block(&list, 0, item.bytes, item.count);
  rm_write_block(&inner, 1, list.bytes, list.count);
  rm_write_block(&items, 1, inner.bytes, inner.count);
  rm_write_id(&text, 1, RM_CRDT_NONE);
  rm_write_block(&text, 2, items.bytes, items.count);
  rm_write_f64(&position, -468);
  rm_write_f64(&position, 234);
  rm_write_block(&text, 3, position.bytes, position.count);
  start_page(&page);
  rm_write_top_block(&page, 0x07, 1, 1, text.bytes, text.count);
  rm_page* parsed = parse(page.bytes, page.count);
  expect_lines(parsed, (const char* const[]){"ab"}, 1, "cut code");
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 0, "nothing lost");
  rm_page_free(parsed);
  rm_buf bufs[] = {page, list, inner, items, text, position, value, item};
  for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i += 1)
    rm_buf_free(&bufs[i]);
}

static void a_settings_width_cut_short_leaves_the_legacy_one(void) {
  /* Tag 5 whose float stops two bytes in. */
  const record records[] = {{.id = {90, 1}, .left = {0, 0}, .text = "w"}};
  rm_buf page;
  start_page(&page);
  append_root_text(&page, records, 1, 740, NAN);
  /* The root text is the last block; its tag 5 is appended by hand. */
  rm_buf width = {0}, settings = {0};
  rm_write_id(&width, 1, rm_crdt_make(1, 2));
  rm_write_tag(&width, 2, RM_TAG_BYTE4);
  rm_buf_append(&width, (const uint8_t[]){0x90, 0x44}, 2);
  rm_write_block(&settings, 1, width.bytes, width.count);
  size_t before = page.count;
  rm_write_block(&page, 5, settings.bytes, settings.count);
  size_t added = page.count - before;
  /* Grow the block's declared length by what was appended. */
  size_t at = RM_HEADER_LENGTH;
  uint32_t length =
      (uint32_t)page.bytes[at] | (uint32_t)page.bytes[at + 1] << 8 |
      (uint32_t)page.bytes[at + 2] << 16 | (uint32_t)page.bytes[at + 3] << 24;
  length += (uint32_t)added;
  for (size_t i = 0; i < 4; i += 1)
    page.bytes[at + i] = (uint8_t)(length >> (8 * i));
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->has_text &&
               parsed->text.has_column_width &&
               parsed->text.column_width == 740,
           "the legacy width stands");
  rm_page_free(parsed);
  rm_buf_free(&width);
  rm_buf_free(&settings);
  rm_buf_free(&page);
}

static void a_highlight_tail_cut_short_keeps_the_highlight(void) {
  /* The value ends inside tag 7: its author is there, its counter is not. */
  rm_buf rectangles = {0}, value = {0}, item = {0}, page;
  rm_write_varuint(&rectangles, 1);
  for (size_t i = 0; i < 4; i += 1) rm_write_f64(&rectangles, 10);
  rm_buf_append_byte(&value, 1);
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_u32(&value, 14);
  rm_write_block(&value, 6, rectangles.bytes, rectangles.count);
  rm_write_tag(&value, 7, RM_TAG_ID);
  rm_buf_append_byte(&value, 1);
  rm_write_id(&item, 1, rm_crdt_make(0, 11));
  rm_write_id(&item, 2, rm_crdt_make(1, 51));
  rm_write_id(&item, 3, RM_CRDT_NONE);
  rm_write_id(&item, 4, RM_CRDT_NONE);
  rm_write_tag(&item, 5, RM_TAG_BYTE4);
  rm_write_u32(&item, 0);
  rm_write_block(&item, 6, value.bytes, value.count);
  start_page(&page);
  rm_write_top_block(&page, 0x03, 1, 1, item.bytes, item.count);
  /* A block after it whose length spells a colour tag must not be read. */
  uint8_t zeros[420] = {0};
  rm_write_top_block(&page, 0x0A, 0, 1, zeros, sizeof zeros);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->highlight_count == 1 &&
               parsed->skipped_blocks == 0,
           "the highlight is read and nothing lost");
  if (parsed != nullptr && parsed->highlight_count == 1) {
    RM_CHECK(parsed->highlights[0].color == RM_COLOR_HIGHLIGHTER_BLUE &&
                 !parsed->highlights[0].has_argb,
             "its own colour, and none from past its value");
  }
  rm_page_free(parsed);
  rm_buf_free(&rectangles);
  rm_buf_free(&value);
  rm_buf_free(&item);
  rm_buf_free(&page);
}

/* A layer node under the root and its link, so that strokes on it are drawn. */
static const rm_crdt_id LAYER = {.counter = 11, .author = 1};

static void append_layer(rm_buf* page) {
  rm_buf state = {0}, group = {0}, value = {0};
  rm_write_id(&state, 1, LAYER);
  rm_write_id(&group, 1, rm_crdt_make(0, 1));
  rm_write_id(&group, 2, rm_crdt_make(1, 12));
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, LAYER);
  rm_write_block(&group, 6, value.bytes, value.count);
  rm_write_top_block(page, 2, 1, 1, state.bytes, state.count);
  rm_write_top_block(page, 4, 1, 1, group.bytes, group.count);
  rm_buf_free(&state);
  rm_buf_free(&group);
  rm_buf_free(&value);
}

/* A stroke of one point under `parent`, in point format `version`, with `tail`
 * appended to its value. */
static void append_one_point_stroke(rm_buf* page, uint8_t version,
                                    rm_crdt_id parent, rm_crdt_id id,
                                    const uint8_t* point, size_t point_size,
                                    const uint8_t* tail, size_t tail_size) {
  rm_buf value = {0}, line = {0};
  rm_buf_append_byte(&value, 0x03);
  rm_write_tag(&value, 1, RM_TAG_BYTE4);
  rm_write_u32(&value, (uint32_t)RM_TOOL_SHADER);
  rm_write_tag(&value, 2, RM_TAG_BYTE4);
  rm_write_u32(&value, 9);
  rm_write_tag(&value, 3, RM_TAG_BYTE8);
  rm_write_f64(&value, 1);
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_f32(&value, 0);
  rm_write_block(&value, 5, point, point_size);
  rm_buf_append(&value, tail, tail_size);
  rm_write_id(&line, 1, parent);
  rm_write_id(&line, 2, id);
  rm_write_id(&line, 3, RM_CRDT_NONE);
  rm_write_id(&line, 4, RM_CRDT_NONE);
  rm_write_tag(&line, 5, RM_TAG_BYTE4);
  rm_write_u32(&line, 0);
  rm_write_block(&line, 6, value.bytes, value.count);
  rm_write_top_block(page, 5, version, version, line.bytes, line.count);
  rm_buf_free(&value);
  rm_buf_free(&line);
}

static rm_page* one_point_stroke(uint8_t version, const uint8_t* point,
                                 size_t point_size, const uint8_t* tail,
                                 size_t tail_size) {
  rm_buf page;
  start_page(&page);
  append_layer(&page);
  append_one_point_stroke(&page, version, LAYER, rm_crdt_make(1, 30), point,
                          point_size, tail, tail_size);
  rm_page* parsed = parse(page.bytes, page.count);
  rm_buf_free(&page);
  return parsed;
}

/* A group node hung on the character `anchor` of the text, linked into the
 * layer by `link`. */
static void append_anchored_node(rm_buf* page, rm_crdt_id node, rm_crdt_id link,
                                 rm_crdt_id anchor) {
  rm_buf state = {0}, field = {0}, group = {0}, value = {0};
  rm_write_id(&state, 1, node);
  rm_write_id(&field, 1, rm_crdt_make(1, link.counter + 100));
  rm_write_id(&field, 2, anchor);
  rm_write_block(&state, 7, field.bytes, field.count);
  rm_write_id(&group, 1, LAYER);
  rm_write_id(&group, 2, link);
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, node);
  rm_write_block(&group, 6, value.bytes, value.count);
  rm_write_top_block(page, 2, 1, 1, state.bytes, state.count);
  rm_write_top_block(page, 4, 1, 1, group.bytes, group.count);
  rm_buf_free(&state);
  rm_buf_free(&field);
  rm_buf_free(&group);
  rm_buf_free(&value);
}

static void
handwriting_anchored_at_and_past_an_inline_image_counts_it_as_one_character(
    void) {
  /* `a`, the image, `b`: the pen past the image stands at 2, past `b` at 3.
   * Were the image no character, or its key read as text, both would
   * shift. */
  const record records[] = {
      {.id = {70, 1}, .left = {0, 0}, .text = "a"},
      {.id = {72, 1},
       .left = {70, 1},
       .text = "{0a418e1a-1111-4222-8333-444455556666}",
       .has_code = true,
       .code = 0xFFFF'FFFFu},
      {.id = {74, 1}, .left = {72, 1}, .text = "b"},
  };
  rm_buf page, point = {0};
  rm_write_f32(&point, 1);
  rm_write_f32(&point, 2);
  rm_write_u16(&point, 0);
  rm_write_u16(&point, 12);
  rm_buf_append_byte(&point, 0);
  rm_buf_append_byte(&point, 100);
  const rm_crdt_id at_image = rm_crdt_make(1, 41), at_b = rm_crdt_make(1, 51);
  start_page(&page);
  append_root_text(&page, records, 3, 936, NAN);
  append_layer(&page);
  append_anchored_node(&page, at_image, rm_crdt_make(1, 42),
                       rm_crdt_make(1, 72));
  append_anchored_node(&page, at_b, rm_crdt_make(1, 52), rm_crdt_make(1, 74));
  append_one_point_stroke(&page, 2, at_image, rm_crdt_make(1, 43), point.bytes,
                          point.count, nullptr, 0);
  append_one_point_stroke(&page, 2, at_b, rm_crdt_make(1, 53), point.bytes,
                          point.count, nullptr, 0);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->skipped_blocks == 0 &&
               parsed->stroke_count == 2 && parsed->anchored_strokes == 2,
           "two strokes, both anchored, nothing lost");
  if (parsed != nullptr && parsed->stroke_count == 2) {
    for (size_t i = 0; i < 2; i += 1) {
      const rm_stroke* stroke = &parsed->strokes[i];
      int32_t expected =
          rm_item_id_equal(stroke->id, (rm_item_id){1, 43}) ? 2 : 3;
      RM_CHECK(stroke->has_anchor && stroke->anchor.paragraph == 0 &&
                   stroke->anchor.offset == expected,
               "stroke %u:%llu stands at offset %d (got %d)",
               (unsigned)stroke->id.author,
               (unsigned long long)stroke->id.counter, (int)expected,
               stroke->has_anchor ? (int)stroke->anchor.offset : -1);
    }
  }
  rm_page_free(parsed);
  rm_buf_free(&point);
  rm_buf_free(&page);
}

static void a_version_one_speed_that_is_no_number_reads_as_nought(void) {
  const float speeds[] = {2.5f, FLT_MAX, INFINITY, NAN};
  const float expected[] = {10.0f, 0.0f, 0.0f, 0.0f};
  for (size_t i = 0; i < 4; i += 1) {
    rm_buf point = {0};
    rm_write_f32(&point, 1); /* x */
    rm_write_f32(&point, 2); /* y */
    rm_write_f32(&point, speeds[i]);
    rm_write_f32(&point, 0);    /* direction */
    rm_write_f32(&point, 3);    /* width */
    rm_write_f32(&point, 0.5f); /* pressure */
    rm_page* parsed = one_point_stroke(1, point.bytes, point.count, nullptr, 0);
    RM_CHECK(parsed != nullptr && parsed->stroke_count == 1 &&
                 parsed->strokes[0].point_count == 1,
             "case %zu: the stroke is drawn", i);
    if (parsed != nullptr && parsed->stroke_count == 1 &&
        parsed->strokes[0].point_count == 1) {
      float speed = parsed->strokes[0].points[0].speed;
      RM_CHECK(isfinite(speed) && speed == expected[i], "case %zu: speed %g", i,
               (double)speed);
    }
    rm_page_free(parsed);
    rm_buf_free(&point);
  }
}

/* A version-one pen that is not a number, or that overflows when scaled,
 * costs its stroke: a width or pressure of infinity draws nothing. */
static void a_version_one_pen_that_is_no_number_costs_its_stroke(void) {
  const float widths[] = {NAN, 3, FLT_MAX};
  const float pressures[] = {0.5f, INFINITY, 0.5f};
  for (size_t i = 0; i < 3; i += 1) {
    rm_buf point = {0};
    rm_write_f32(&point, 1);    /* x */
    rm_write_f32(&point, 2);    /* y */
    rm_write_f32(&point, 2.5f); /* speed */
    rm_write_f32(&point, 0);    /* direction */
    rm_write_f32(&point, widths[i]);
    rm_write_f32(&point, pressures[i]);
    rm_page* parsed = one_point_stroke(1, point.bytes, point.count, nullptr, 0);
    RM_CHECK(parsed != nullptr && parsed->stroke_count == 0 &&
                 parsed->skipped_blocks == 1,
             "case %zu: the stroke is a counted loss", i);
    rm_page_free(parsed);
    rm_buf_free(&point);
  }
}

static void a_source_id_cut_short_is_no_source(void) {
  rm_buf point = {0};
  rm_write_f32(&point, 1);
  rm_write_f32(&point, 2);
  rm_write_u16(&point, 7);
  rm_write_u16(&point, 12);
  rm_buf_append_byte(&point, 0);
  rm_buf_append_byte(&point, 100);
  /* Tag 9 and its author, and the value ends before the counter. */
  const uint8_t cut[] = {0x9F, 1};
  rm_page* parsed =
      one_point_stroke(2, point.bytes, point.count, cut, sizeof cut);
  RM_CHECK(parsed != nullptr && parsed->stroke_count == 1 &&
               parsed->skipped_blocks == 0,
           "the stroke is kept");
  if (parsed != nullptr && parsed->stroke_count == 1) {
    RM_CHECK(!parsed->strokes[0].has_source_id, "and names no source");
    RM_CHECK(parsed->strokes[0].points[0].speed == 7,
             "its speed is the raw uint16");
  }
  rm_page_free(parsed);

  /* Whole, the same tail names its source. */
  rm_buf whole = {0};
  rm_write_id(&whole, 9, rm_crdt_make(1, 17));
  parsed =
      one_point_stroke(2, point.bytes, point.count, whole.bytes, whole.count);
  RM_CHECK(
      parsed != nullptr && parsed->stroke_count == 1 &&
          parsed->strokes[0].has_source_id &&
          rm_item_id_equal(parsed->strokes[0].source_id, (rm_item_id){1, 17}),
      "a whole tag 9 is the source");
  rm_page_free(parsed);
  rm_buf_free(&whole);
  rm_buf_free(&point);
}

static void a_deleted_inline_image_is_no_character(void) {
  const record records[] = {
      {.id = {20, 1}, .left = {0, 0}, .text = "x\n"},
      {.id = {22, 1},
       .left = {21, 1},
       .text = "{0a418e1a-1111-4222-8333-444455556666}",
       .has_code = true,
       .code = 0xFFFF'FFFFu,
       .deleted = 1},
      {.id = {23, 1}, .left = {22, 1}, .text = "\ny"},
  };
  rm_page* page = text_page(records, 3, 936, NAN);
  expect_lines(page, (const char* const[]){"x", "", "y"}, 3, "deleted image");
  rm_page_free(page);
}

static void an_inline_image_between_bold_marks_is_set_in_bold(void) {
  const record records[] = {
      {.id = {70, 1}, .left = {0, 0}, .text = "a"},
      {.id = {71, 1}, .left = {70, 1}, .text = "", .has_code = true, .code = 1},
      {.id = {72, 1},
       .left = {71, 1},
       .text = "{0a418e1a-1111-4222-8333-444455556666}",
       .has_code = true,
       .code = 0xFFFF'FFFFu},
      {.id = {73, 1}, .left = {72, 1}, .text = "", .has_code = true, .code = 2},
      {.id = {74, 1}, .left = {73, 1}, .text = "b"},
  };
  rm_page* page = text_page(records, 5, 936, NAN);
  expect_lines(page,
               (const char* const[]){"a\xEF\xBF\xBC"
                                     "b"},
               1, "bold image");
  if (page != nullptr && page->has_text && page->text.paragraph_count == 1) {
    const rm_paragraph* line = &page->text.paragraphs[0];
    RM_CHECK(line->face_count == 1 && line->faces[0].start == 1 &&
                 line->faces[0].end == 2 && line->faces[0].bold &&
                 !line->faces[0].italic,
             "the image, and only it, is bold");
  }
  rm_page_free(page);
}

/* ------------------------------------------------------------------ */
/* Recovery from damaged text and nodes                                */
/* ------------------------------------------------------------------ */

/* A second text block that will not read is a counted loss, and the text that
 * did read stays: it is replaced only by a text that parsed. */
static void a_later_text_block_that_fails_leaves_the_earlier_one(void) {
  const record records[] = {
      {.id = {.counter = 20, .author = 1}, .text = "kept"}};
  rm_buf page;
  start_page(&page);
  append_root_text(&page, records, 1, 936, NAN);
  const uint8_t broken[] = {0x00};
  rm_write_top_block(&page, 0x07, 1, 1, broken, sizeof broken);
  rm_page* parsed = parse(page.bytes, page.count);
  const char* const lines[] = {"kept"};
  expect_lines(parsed, lines, 1, "a later broken text");
  if (parsed != nullptr)
    RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 1, "the broken one is counted");
  rm_page_free(parsed);
  rm_buf_free(&page);
}

/* A list that declares more records than it holds keeps the ones it holds: the
 * count is the file's word, and running out of records is where it ends. */
static void a_text_list_that_runs_out_keeps_what_it_holds(void) {
  const record records[] = {
      {.id = {.counter = 20, .author = 1}, .text = "ab"},
      {.id = {.counter = 22, .author = 1},
       .left = {.counter = 21, .author = 1},
       .text = "cd"},
  };
  rm_buf page;
  start_page(&page);
  append_root_text_declaring(&page, records, 2, 5, 936, NAN);
  rm_page* parsed = parse(page.bytes, page.count);
  const char* const lines[] = {"abcd"};
  expect_lines(parsed, lines, 1, "a count past the records");
  if (parsed != nullptr)
    RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "and nothing is lost");
  rm_page_free(parsed);
  rm_buf_free(&page);
}

/* A record that fails inside — here one cut short after its identifier — is
 * not half a text, and costs the list: the block reads as text with no
 * characters. */
static void a_text_record_that_fails_inside_costs_the_list(void) {
  const uint8_t no_deleted_field[] = {0x0C, 4, 0, 0, 0, 0x2F, 1, 30, 0x00};
  const record records[] = {
      {.id = {.counter = 20, .author = 1},
       .text = "ab",
       .trailing = no_deleted_field,
       .trailing_count = sizeof no_deleted_field},
  };
  rm_buf page;
  start_page(&page);
  append_root_text_declaring(&page, records, 1, 2, 936, NAN);
  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->has_text,
           "the block still reads as text");
  if (parsed != nullptr && parsed->has_text) {
    RM_CHECK(parsed->text.paragraph_count == 1 &&
                 parsed->text.paragraphs[0].text.length == 0,
             "with nothing typed in it");
  }
  rm_page_free(parsed);
  rm_buf_free(&page);
}

/* A field of a tree node that runs past its block ends the node where it
 * stands: the anchor read before it still fastens the handwriting, and the
 * node is no loss. */
static void a_tree_node_field_past_its_block_keeps_what_came_before(void) {
  const record records[] = {
      {.id = {.counter = 70, .author = 1}, .text = "abc"}};
  rm_buf page;
  start_page(&page);
  append_root_text(&page, records, 1, 936, NAN);
  append_layer(&page);

  rm_crdt_id node = rm_crdt_make(1, 40);
  rm_buf state = {0}, field = {0}, group = {0}, value = {0};
  rm_write_id(&state, 1, node);
  rm_write_id(&field, 1, rm_crdt_make(1, 140));
  rm_write_id(&field, 2, rm_crdt_make(1, 71));
  rm_write_block(&state, 7, field.bytes, field.count);
  rm_write_tag(&state, 4, RM_TAG_LENGTH4);
  rm_write_u32(&state, 1000); /* past the block */
  rm_write_id(&group, 1, LAYER);
  rm_write_id(&group, 2, rm_crdt_make(1, 41));
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, node);
  rm_write_block(&group, 6, value.bytes, value.count);
  rm_write_top_block(&page, 2, 1, 1, state.bytes, state.count);
  rm_write_top_block(&page, 4, 1, 1, group.bytes, group.count);

  rm_buf point = {0};
  rm_write_f32(&point, 1);
  rm_write_f32(&point, 2);
  rm_write_u16(&point, 0);
  rm_write_u16(&point, 12);
  rm_buf_append_byte(&point, 0);
  rm_buf_append_byte(&point, 100);
  append_one_point_stroke(&page, 2, node, rm_crdt_make(1, 50), point.bytes,
                          point.count, nullptr, 0);

  rm_page* parsed = parse(page.bytes, page.count);
  RM_CHECK(parsed != nullptr && parsed->stroke_count == 1,
           "the stroke is drawn");
  if (parsed != nullptr && parsed->stroke_count == 1) {
    RM_CHECK(parsed->strokes[0].has_anchor &&
                 parsed->strokes[0].anchor.paragraph == 0 &&
                 parsed->strokes[0].anchor.offset == 2,
             "and fastened to the character read before the broken field");
    RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 0, "the node is no loss");
  }
  rm_page_free(parsed);
  rm_buf_free(&point);
  rm_buf_free(&state);
  rm_buf_free(&field);
  rm_buf_free(&group);
  rm_buf_free(&value);
  rm_buf_free(&page);
}

/* ------------------------------------------------------------------ */
/* Sample pages                                                        */
/* ------------------------------------------------------------------ */

/* Pages written on the tablet. The highlighter's blue and orange are written
 * as their own indexes with no `argb`, and a highlight over typed text comes in
 * the second form — `first`/`last`/`includeLastId` instead of
 * `start`/`length`. */
static rm_page* sample_page(const char* name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", RM_FIXTURE_DIR, name);
  size_t size = 0;
  uint8_t* bytes = rm_test_read_file(path, &size);
  RM_CHECK(bytes != nullptr, "%s reads", name);
  rm_page* page = bytes == nullptr ? nullptr : parse(bytes, size);
  RM_CHECK(page != nullptr, "%s parses", name);
  free(bytes);
  return page;
}

static void
a_sample_pages_blue_and_orange_highlighter_strokes_keep_their_colours(void) {
  rm_page* page = sample_page("highlighter-blue-orange.rm");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "nothing lost");
  size_t blue = 0, orange = 0, highlighter = 0, with_argb = 0, shader = 0,
         shader_ok = 0;
  for (size_t i = 0; i < page->stroke_count; i += 1) {
    const rm_stroke* stroke = &page->strokes[i];
    if (stroke->tool == RM_TOOL_HIGHLIGHTER_2) {
      highlighter += 1;
      if (stroke->color == RM_COLOR_HIGHLIGHTER_BLUE) blue += 1;
      if (stroke->color == RM_COLOR_HIGHLIGHTER_ORANGE) orange += 1;
      if (stroke->has_argb) with_argb += 1;
    } else if (stroke->tool == RM_TOOL_SHADER) {
      shader += 1;
      if (stroke->color == RM_COLOR_HIGHLIGHT && stroke->has_argb)
        shader_ok += 1;
    }
  }
  RM_CHECK_EQ_SIZE(blue, 2, "two blue");
  RM_CHECK_EQ_SIZE(orange, 2, "two orange");
  RM_CHECK_EQ_SIZE(with_argb, 0,
                   "the tablet writes the index alone (%zu marks)",
                   highlighter);
  /* The shader keeps index 9 and the whole colour in `argb`. */
  RM_CHECK_EQ_SIZE(shader, 3, "three shader strokes");
  RM_CHECK_EQ_SIZE(shader_ok, shader, "each with index 9 and an argb");
  rm_page_free(page);
}

static void
a_sample_pages_highlights_in_the_second_form_keep_their_explicit_colour(void) {
  rm_page* page = sample_page("glyph-first-last-text.rm");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "nothing lost");
  RM_CHECK_EQ_SIZE(page->highlight_count, 2, "two highlights");
  for (size_t i = 0; i < page->highlight_count; i += 1) {
    RM_CHECK(page->highlights[i].color == RM_COLOR_GREEN &&
                 page->highlights[i].has_argb &&
                 page->highlights[i].argb == 0xFF00'0000u,
             "highlight %zu: green, and black in argb", i);
  }
  /* The text's width, from the stamped settings, equal to the legacy copy. */
  RM_CHECK(page->has_text && page->text.has_column_width &&
               page->text.column_width == 740,
           "the column is 740 wide");
  RM_CHECK(page->has_text && page->text.paragraph_count > 2 &&
               page->text.paragraphs[2].has_format &&
               rm_paragraph_format_style(&page->text.paragraphs[2].format) ==
                   RM_PARA_CHECKBOX,
           "line 2 is a checkbox");
  rm_page_free(page);
}

static void a_sample_pages_checkboxes_are_shown_as_the_tablet_shows_them(void) {
  /* Two first-level items, each with two nested ones: under the first both
   * are ticked, under the second one of two. The tablet draws the first
   * parent ticked and struck through, the second with a dash — while both
   * keep style 6. */
  rm_page* page = sample_page("checkbox-states.rm");
  if (page == nullptr) return;
  RM_CHECK(page->has_text && page->text.paragraph_count >= 10,
           "ten lines at least");
  if (!page->has_text || page->text.paragraph_count < 10) {
    rm_page_free(page);
    return;
  }
  const rm_text_block* text = &page->text;
  const rm_paragraph* lines = text->paragraphs;

  RM_CHECK(
      lines[1].has_format &&
          rm_paragraph_format_style(&lines[1].format) == RM_PARA_CHECKBOX &&
          rm_paragraph_format_has_ticked_nested_items(&lines[1].format),
      "line 1: style 6, something nested ticked");
  RM_CHECK(rm_text_block_checkbox_state(text, 1) == RM_CHECKBOX_TICKED,
           "line 1 ticked");
  RM_CHECK(lines[2].has_format && rm_paragraph_format_style(&lines[2].format) ==
                                      RM_PARA_CHECKBOX_CHECKED_NESTED,
           "line 2: style 9");
  RM_CHECK(rm_text_block_checkbox_state(text, 2) == RM_CHECKBOX_TICKED,
           "line 2 ticked");
  RM_CHECK(rm_text_block_checkbox_state(text, 3) == RM_CHECKBOX_PARTIAL,
           "line 3 partial");
  RM_CHECK(rm_text_block_checkbox_state(text, 4) == RM_CHECKBOX_UNTICKED,
           "line 4 unticked");
  RM_CHECK(rm_text_block_checkbox_state(text, 5) == RM_CHECKBOX_TICKED,
           "line 5 ticked");
  RM_CHECK(rm_text_block_checkbox_state(text, 0) == RM_CHECKBOX_NONE,
           "plain text has none");
  /* A parent with nothing ticked under it carries no `tag 3` at all. */
  RM_CHECK(lines[6].has_format && !lines[6].format.has_third &&
               !rm_paragraph_format_has_ticked_nested_items(&lines[6].format),
           "line 6 carries no state");
  for (size_t line = 6; line <= 9; line += 1) {
    RM_CHECK(rm_text_block_checkbox_state(text, line) == RM_CHECKBOX_UNTICKED,
             "line %zu unticked", line);
  }
  for (size_t line = 1; line <= 5; line += 1) {
    int expected = line == 1 || line == 3 ? 1 : 2;
    RM_CHECK(lines[line].has_format &&
                 rm_paragraph_format_level(&lines[line].format) == expected,
             "line %zu at level %d", line, expected);
  }
  rm_page_free(page);
}

static void
the_pieces_of_an_erased_shader_stroke_name_the_stroke_they_were_cut_from(void) {
  /* A shader stroke `1:17` cut by the eraser, one of its pieces cut again; a
   * separate shader stroke `1:16` beside it; later shader strokes in another
   * colour; ballpoint lines over them. */
  rm_page* page = sample_page("shader-erased.rm");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "nothing lost");
  const rm_item_id source = {.author = 1, .counter = 17};
  size_t pieces = 0, pieces_shader = 0, without = 0;
  for (size_t i = 0; i < page->stroke_count; i += 1) {
    const rm_stroke* stroke = &page->strokes[i];
    if (!stroke->has_source_id) {
      without += 1;
    } else if (rm_item_id_equal(stroke->source_id, source)) {
      pieces += 1;
      if (stroke->tool == RM_TOOL_SHADER) pieces_shader += 1;
    }
    if (rm_item_id_equal(stroke->id,
                         (rm_item_id){.author = 1, .counter = 16})) {
      RM_CHECK(!stroke->has_source_id,
               "the separate shader stroke names no source");
    }
  }
  RM_CHECK_EQ_SIZE(pieces, 4, "four pieces of 1:17");
  RM_CHECK_EQ_SIZE(pieces_shader, pieces, "every piece a shader");
  /* Every other stroke carries none. */
  RM_CHECK_EQ_SIZE(without, page->stroke_count - 4, "the rest carry no source");
  rm_page_free(page);
}

static void a_ballpoint_stroke_keeps_the_pens_speed(void) {
  /* The tablet thins the second ballpoint's ink by the speed field, so the
   * reader must keep it — whole numbers, as the uint16 was written. */
  rm_page* page = sample_page("shader-erased.rm");
  if (page == nullptr) return;
  size_t points = 0, moving = 0, whole = 0;
  for (size_t i = 0; i < page->stroke_count; i += 1) {
    const rm_stroke* stroke = &page->strokes[i];
    if (stroke->tool != RM_TOOL_BALLPOINT_2) continue;
    for (size_t p = 0; p < stroke->point_count; p += 1) {
      float speed = stroke->points[p].speed;
      points += 1;
      if (speed > 0) moving += 1;
      if (speed >= 0 && speed <= 65535 && roundf(speed) == speed) whole += 1;
    }
  }
  RM_CHECK(points > 0, "the page has ballpoint points");
  RM_CHECK(moving > 0, "some of them with a speed");
  RM_CHECK_EQ_SIZE(whole, points, "every speed a whole uint16");
  rm_page_free(page);
}

static void text_ties_resume_sibling_branches(void) {
  const record rows[] = {
      {.id = {20, 1}, .text = "Hello "},
      {.id = {26, 1}, .left = {25, 1}, .text = "world"},
      {.id = {100, 1}, .left = {25, 1}, .right = {26, 1}, .text = "brave "},
      {.id = {200, 1}, .right = {20, 1}, .text = "Oh, "},
  };
  rm_page* page = text_page(rows, 4, 936, NAN);
  expect_lines(page, (const char* const[]){"Oh, Hello brave world"}, 1,
               "nested text ties");
  rm_page_free(page);
}

static void transitive_text_neighbours(void) {
  const record rows[] = {{.id={100,1}, .text="A"},
                         {.id={200,1}, .right={100,1}, .text="B"},
                         {.id={300,1}, .right={200,1}, .text="C"}};
  rm_buf file;
  start_page(&file);
  append_root_text(&file, rows, 3, 936, NAN);
  rm_page* pg = parse(file.bytes, file.count);
  expect_lines(pg, (const char* const[]){"CBA"}, 1, "right chain");
  rm_page_free(pg);
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(file.bytes, file.count, &s, nullptr) == RM_OK &&
           rm_edit_replace_text(s, 0, 1, "X", (rm_item_id){1,400}, nullptr) == RM_OK,
           "replace first scalar of chain");
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(s, &size);
  pg = parse(bytes, size);
  expect_lines(pg, (const char* const[]){"XBA"}, 1, "edited right chain");
  rm_page_free(pg);
  rm_edit_close(s);
  rm_buf_free(&file);
}

static void text_neighbours_across_left_groups(void) {
  const record rows[] = {
      {.id = {100, 1}, .text = "A"},
      {.id = {200, 1}, .text = "B"},
      {.id = {300, 1}, .left = {200, 1}, .right = {100, 1}, .text = "C"}};
  rm_buf file;
  start_page(&file);
  append_root_text(&file, rows, 3, 936, NAN);
  rm_page* pg = parse(file.bytes, file.count);
  expect_lines(pg, (const char* const[]){"BCA"}, 1, "cross-group neighbours");
  rm_page_free(pg);
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(file.bytes, file.count, &s, nullptr) == RM_OK &&
               rm_edit_replace_text(s, 0, 1, "X", (rm_item_id){1, 400},
                                    nullptr) == RM_OK,
           "replace first scalar of chain");
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(s, &size);
  pg = parse(bytes, size);
  expect_lines(pg, (const char* const[]){"XCA"}, 1,
               "edited cross-group neighbours");
  rm_page_free(pg);
  rm_edit_close(s);
  rm_buf_free(&file);
}

static rm_page* session_page(rm_edit_session* session) {
  size_t size;
  const uint8_t* data = rm_edit_bytes(session, &size);
  return parse(data, size);
}

static void interior_neighbours_order_scalar_edits(void) {
  record rows[] = {
      {.id = {100, 1}, .text = "ABCD"},
      {.id = {200, 1}, .left = {101, 1}, .right = {102, 1}, .text = "X"}};
  rm_buf b;
  start_page(&b);
  append_root_text(&b, rows, 2, 936, NAN);
  rm_edit_session* session = nullptr;
  RM_CHECK(rm_edit_open(b.bytes, b.count, &session, nullptr) == RM_OK, "open");
  rm_page* p = session_page(session);
  expect_lines(p, (const char*[]){"ABXCD"}, 1, "interior insertion");
  rm_page_free(p);
  RM_CHECK(rm_edit_replace_text(session, 4, 1, "", (rm_item_id){1, 901},
                                nullptr) == RM_OK,
           "remove D");
  p = session_page(session);
  expect_lines(p, (const char*[]){"ABXC"}, 1, "remove last scalar");
  rm_page_free(p);
  rm_edit_close(session);
  rm_buf_free(&b);
}

static void anchor_registers_follow_independent_stamps(void) {
  for (int reverse = 0; reverse < 2; reverse++) {
    rm_buf b;
    start_page(&b);
    record row = {.id = {100, 1}, .text = "ABC"};
    append_root_text(&b, &row, 1, 936, NAN);
    append_layer(&b);
    rm_crdt_id node = {41, 1};
    for (int j = 0; j < 2; j++) {
      bool newer = (j == reverse);
      rm_buf state = {0}, target = {0};
      rm_write_id(&state, 1, node);
      rm_write_id(&target, 1, rm_crdt_make(1, newer ? 142 : 120));
      rm_write_id(&target, 2, rm_crdt_make(1, newer ? 102 : 100));
      rm_write_block(&state, 7, target.bytes, target.count);
      rm_write_stamped_f32(&state, 10, rm_crdt_make(1, newer ? 121 : 143),
                           newer ? 5 : 9);
      rm_write_top_block(&b, 2, 1, 1, state.bytes, state.count);
      rm_buf_free(&state);
      rm_buf_free(&target);
    }
    uint8_t point[14] = {0};
    append_one_point_stroke(&b, 2, node, (rm_crdt_id){43, 1}, point, 14,
                            nullptr, 0);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK, "open");
    for (int step = 0; step < 2; step++) {
      rm_page* p = session_page(s);
      RM_CHECK(p && !p->skipped_blocks && p->anchored_strokes == 1 &&
                   p->strokes[0].anchor.offset == 3,
               "winning character");
      RM_CHECK(p && p->stroke_count == 1 && p->strokes[0].anchor.has_origin_x &&
                   p->strokes[0].anchor.origin_x == 9,
               "winning horizontal");
      rm_page_free(p);
      if (!step)
        RM_CHECK(rm_edit_replace_text(s, 2, 1, "Q", (rm_item_id){1, 901},
                                      nullptr) == RM_OK,
                 "replace C");
    }
    RM_CHECK(rm_edit_replace_text(s, 0, 1, "", (rm_item_id){1, 902}, nullptr) ==
                 RM_OK,
             "stale anchor permits deletion");
    rm_edit_close(s);
    rm_buf_free(&b);
  }
}

static void unlinked_children_inherit_layer_visibility(void) {
  for (int image = 0; image < 2; image++) {
    char path[1024];
    rm_test_fixture_path(path, sizeof path, "Wikipedia_highlighted_p1.rm");
    size_t size;
    uint8_t* data =
        image ? rm_damaged_image_page(&size) : rm_test_read_file(path, &size);
    RM_CHECK(data, "load fixture");
    if (!data) continue;
    rm_buf b, event = {0}, parent_field = {0};
    start_page(&b);
    rm_reader in = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
    rm_crdt_id child = {110, 1}, parent = {0};
    while (rm_reader_remaining(&in) >= 8) {
      rm_block_header h;
      if (rm_read_block_header(&in, &h) != RM_OK) break;
      if (h.type == (image ? 15 : 3)) {
        rm_reader body = in;
        body.size = h.end;
        RM_CHECK(rm_read_crdt(&body, 1, &parent) == RM_OK, "content parent");
        rm_buf moved = {0};
        rm_write_id(&moved, 1, child);
        rm_buf_append(&moved, data + body.offset, h.end - body.offset);
        rm_write_u32(&b, (uint32_t)moved.count);
        rm_buf_append(&b, data + h.start + 4, 4);
        rm_buf_append(&b, moved.bytes, moved.count);
        rm_buf_free(&moved);
      } else
        rm_buf_append(&b, data + h.start, h.end - h.start);
      in.offset = h.end;
    }
    rm_write_id(&event, 1, child);
    rm_write_id(&event, 2, RM_CRDT_NONE);
    rm_write_tag(&event, 3, RM_TAG_BYTE1);
    rm_buf_append_byte(&event, 1);
    rm_write_id(&parent_field, 1, parent);
    rm_write_block(&event, 4, parent_field.bytes, parent_field.count);
    rm_write_top_block(&b, 1, 1, 1, event.bytes, event.count);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK, "open");
    for (int stage = 0; stage < 3; stage++) {
      rm_page* p = session_page(s);
      RM_CHECK(
          p && !p->skipped_blocks &&
              p->highlight_count == (size_t)(!image && stage != 1 ? 4 : 0) &&
              p->image_count == (size_t)(image && stage != 1),
          "visibility at stage %d", stage);
      rm_page_free(p);
      if (stage == 0)
        RM_CHECK(rm_edit_set_node_visible(s, rm_item_id_of(parent),
                                          (rm_item_id){1, 900100}, false,
                                          nullptr) == RM_OK,
                 "hide layer");
      if (stage == 1) RM_CHECK(rm_edit_undo(s), "undo hide");
    }
    rm_edit_close(s);
    rm_buf_free(&event);
    rm_buf_free(&parent_field);
    rm_buf_free(&b);
    free(data);
  }
}

static void scene_preferences_follow_stamps(void) {
  rm_buf b;
  start_page(&b);
  for (int i = 0; i < 2; i++) {
    rm_crdt_id stamp = rm_crdt_make(1, 50 - i);
    uint8_t setting = i ? 0 : 2;
    rm_buf scene = {0};
    rm_write_stamped_u8(&scene, 2, stamp, i);
    rm_write_stamped_u8(&scene, 3, stamp, i);
    rm_write_stamped_block(&scene, 9, stamp, &setting, 1);
    rm_write_top_block(&b, 13, 1, 1, scene.bytes, scene.count);
    rm_buf_free(&scene);
  }
  rm_edit_session* s = nullptr;
  RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK, "open scene");
  RM_CHECK(rm_edit_replace_text(s, 0, 0, "X", (rm_item_id){1, 901}, nullptr) ==
               RM_OK,
           "create root");
  rm_page* p = session_page(s);
  RM_CHECK(p && !p->background_visible, "template register");
  RM_CHECK(p && !p->root_document_visible, "document register");
  RM_CHECK(p && p->text.column_width == rm_text_column_width(RM_COLUMN_WIDE),
           "column register");
  rm_page_free(p);
  rm_edit_close(s);
  rm_buf_free(&b);
}

static void preferred_layout_defaults_follow_the_winning_mode(void) {
  for (int mode = 0; mode < 3; mode++) {
    rm_buf b, info = {0}, screen = {0};
    start_page(&b);
    rm_write_u32(&screen, 1600);
    rm_write_u32(&screen, 2000);
    rm_write_block(&info, 5, screen.bytes, screen.count);
    uint8_t setting = mode == 0 ? 0 : mode == 1 ? 3 : 4;
    rm_write_stamped_block(&info, 9, mode ? rm_crdt_make(1, 100) : RM_CRDT_NONE,
                           &setting, 1);
    rm_write_top_block(&b, 13, 1, 1, info.bytes, info.count);
    if (mode) {
      info.count = 0;
      setting = 0;
      rm_write_stamped_block(&info, 9, rm_crdt_make(1, 50), &setting, 1);
      rm_write_top_block(&b, 13, 1, 1, info.bytes, info.count);
    }
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK,
             "open scene");
    RM_CHECK(rm_edit_replace_text(s, 0, 0, "X", (rm_item_id){1, 901},
                                  nullptr) == RM_OK,
             "create root");
    rm_page* p = session_page(s);
    RM_CHECK(p && p->text.column_width == (mode == 2 ? 1152 : 936),
             "preferred layout mode %d", mode);
    rm_page_free(p);
    rm_edit_close(s);
    rm_buf_free(&screen);
    rm_buf_free(&info);
    rm_buf_free(&b);
  }
}

static void attachment_name_follows_stamp(void) {
  size_t n;
  uint8_t* data = rm_damaged_image_page(&n);
  rm_buf b = {0}, name = {0}, row = {0}, rows = {0}, map = {0};
  rm_buf_append(&b, data, n);
  for (uint8_t i = 0; i < 16; i++) rm_buf_append_byte(&row, i);
  rm_write_string(&name, "stale.png");
  rm_write_stamped_block(&row, 1, rm_crdt_make(1, 1), name.bytes, name.count);
  rm_write_varuint(&rows, 1);
  rm_write_block(&rows, 0, row.bytes, row.count);
  rm_write_block(&map, 1, rows.bytes, rows.count);
  rm_write_top_block(&b, 14, 1, 1, map.bytes, map.count);
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(p && p->image_count == 1, "image");
  if (p && p->image_count)
    RM_CHECK_EQ_STR(p->images[0].file.bytes, p->images[0].file.length,
                    "0a418e1a-1111-4222-8333-444455556666.png",
                    "attachment name");
  rm_page_free(p);
  free(data);
  rm_buf_free(&b);
  rm_buf_free(&name);
  rm_buf_free(&row);
  rm_buf_free(&rows);
  rm_buf_free(&map);
}

static void paragraph_style_and_width_follow_stamps(void) {
  rm_buf b, root = {0}, lists = {0}, entries = {0}, styles = {0},
            position = {0};
  start_page(&b);
  rm_write_varuint(&entries, 2);
  rm_write_id(&root, 1, RM_CRDT_NONE);
  for (int i = 0; i < 2; i++) {
    rm_crdt_id stamp = rm_crdt_make(1, 20 - i);
    uint8_t format[] = {0x11, (uint8_t)(i ? 1 : 3)};
    rm_write_bare_id(&entries, RM_CRDT_NONE);
    rm_write_id(&entries, 1, stamp);
    rm_write_block(&entries, 2, format, sizeof format);
    rm_buf settings = {0};
    rm_write_stamped_f32(&settings, 1, stamp, i ? 740 : 1152);
    rm_write_block(&root, 5, settings.bytes, settings.count);
    rm_buf_free(&settings);
  }
  rm_write_block(&styles, 1, entries.bytes, entries.count);
  rm_write_block(&lists, 2, styles.bytes, styles.count);
  rm_write_block(&root, 2, lists.bytes, lists.count);
  rm_write_f64(&position, 0);
  rm_write_f64(&position, 0);
  rm_write_block(&root, 3, position.bytes, position.count);
  rm_write_top_block(&b, 7, 1, 1, root.bytes, root.count);
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(p && p->has_text && p->text.paragraphs[0].format.raw == 3,
           "style stamp");
  RM_CHECK(p && p->has_text && p->text.column_width == 1152, "width stamp");
  rm_page_free(p);
  rm_buf_free(&b);
  rm_buf_free(&root);
  rm_buf_free(&lists);
  rm_buf_free(&entries);
  rm_buf_free(&styles);
  rm_buf_free(&position);
}

static void append_ordered_stroke(rm_buf* page, uint64_t id, uint64_t left,
                                  uint64_t right) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {
      .points = &point, .point_count = 1, .tool = RM_TOOL_FINELINER_2};
  rm_buf body = {0};
  rm_write_line_item(&body, &stroke, LAYER, rm_crdt_make(1, id),
                     left ? rm_crdt_make(1, left) : RM_CRDT_NONE,
                     right ? rm_crdt_make(1, right) : RM_CRDT_NONE);
  rm_write_top_block(page, 5, 2, 2, body.bytes, body.count);
  rm_buf_free(&body);
}

static void scene_neighbours_inside_deleted_ranges(void) {
  rm_buf b, tomb = {0};
  start_page(&b);
  append_ordered_stroke(&b, 200, 102, 0);
  append_ordered_stroke(&b, 201, 101, 102);
  rm_write_id(&tomb, 1, LAYER);
  rm_write_id(&tomb, 2, rm_crdt_make(1, 100));
  rm_write_id(&tomb, 3, RM_CRDT_NONE);
  rm_write_id(&tomb, 4, RM_CRDT_NONE);
  rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
  rm_write_u32(&tomb, 4);
  rm_write_top_block(&b, 8, 1, 1, tomb.bytes, tomb.count);
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(p && p->stroke_count == 2 && p->strokes[0].id.counter == 201,
           "stroke between deleted positions precedes the later stroke");
  rm_page_free(p);
  /* Two siblings tied at the last ID of an unsplit tombstone. */
  b.count = RM_HEADER_LENGTH;
  rm_write_top_block(&b, 8, 1, 1, tomb.bytes, tomb.count);
  append_ordered_stroke(&b, 200, 103, 0);
  append_ordered_stroke(&b, 201, 103, 0);
  p = parse(b.bytes, b.count);
  RM_CHECK(p && p->stroke_count == 2 && p->strokes[0].id.counter == 201,
           "siblings after a compact tombstone retain the scene tie-break");
  rm_page_free(p);
  rm_buf_free(&b);
  rm_buf_free(&tomb);
}

static void disconnected_scene_chains_honor_known_neighbours(void) {
  rm_buf b;
  start_page(&b);
  append_ordered_stroke(&b, 200, 201, 0);
  append_ordered_stroke(&b, 201, 99, 0);
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(p && p->stroke_count == 2 && p->strokes[0].id.counter == 201,
           "known left dependency in a disconnected chain");
  rm_page_free(p);
  rm_buf_free(&b);
}

static void stamped_sheet_size_overrides_legacy_size(void) {
  rm_buf b;
  start_page(&b);
  for (int i = 0; i < 2; i++) {
    rm_buf scene = {0}, size = {0};
    rm_write_f64(&size, 100);
    rm_write_f64(&size, 200);
    rm_write_block(&scene, 7, size.bytes, size.count);
    size.count = 0;
    rm_write_f64(&size, i ? 400 : 800);
    rm_write_f64(&size, i ? 600 : 1200);
    rm_write_stamped_block(&scene, 8, rm_crdt_make(1, 20 - i), size.bytes,
                           size.count);
    rm_write_top_block(&b, 13, 1, 1, scene.bytes, scene.count);
    rm_buf_free(&scene);
    rm_buf_free(&size);
  }
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(p && p->has_paper_size && p->paper_size.width == 800 &&
               p->paper_size.height == 1200,
           "authoritative sheet size");
  rm_page_free(p);
  rm_buf_free(&b);
}

static void stale_group_links_do_not_discard_sibling_order(void) {
  rm_buf b, event = {0}, placement = {0};
  start_page(&b);
  append_ordered_stroke(&b, 200, 201, 0);
  append_ordered_stroke(&b, 201, 0, 0);
  append_anchored_node(&b, LAYER, rm_crdt_make(1, 52), RM_CRDT_NONE);
  rm_write_id(&event, 1, LAYER);
  rm_write_id(&event, 2, RM_CRDT_NONE);
  rm_write_tag(&event, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&event, 1);
  rm_write_id(&placement, 1, rm_crdt_make(0, 1));
  rm_write_block(&event, 4, placement.bytes, placement.count);
  rm_write_top_block(&b, 1, 1, 1, event.bytes, event.count);
  rm_page* p = parse(b.bytes, b.count);
  RM_CHECK(
      p && p->stroke_count == 2 && p->strokes[0].id.counter == 201,
      "stale group link cannot make ordered strokes fall back to file order");
  rm_page_free(p);
  rm_edit_session* session = nullptr;
  RM_CHECK(rm_edit_open(b.bytes, b.count, &session, nullptr) == RM_OK,
           "open stale link");
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {
      .tool = RM_TOOL_FINELINER_2, .points = &point, .point_count = 1};
  RM_CHECK(
      rm_edit_insert_stroke(session, rm_item_id_of(LAYER), (rm_item_id){1, 300},
                            (rm_item_id){1, 52}, (rm_item_id){0}, &stroke,
                            nullptr) == RM_ERR_ITEMS_NOT_FOUND,
      "a moved group's stale link is not a live insertion neighbour");
  rm_edit_close(session);
  rm_buf_free(&b);
  rm_buf_free(&event);
  rm_buf_free(&placement);
}

static void repeated_node_fields_use_the_winning_stamp(void) {
  for (int order = 0; order < 2; order++) {
    rm_buf b, state = {0};
    start_page(&b);
    append_layer(&b);
    rm_write_id(&state, 1, LAYER);
    for (int i = 0; i < 2; i++) {
      bool latest = i == order;
      rm_write_stamped_u8(&state, 3, rm_crdt_make(1, latest ? 30 : 20),
                          !latest);
    }
    rm_write_top_block(&b, 2, 1, 1, state.bytes, state.count);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK, "open");
    rm_edit_layer* layers = nullptr;
    size_t count = 0;
    RM_CHECK(rm_edit_layers(s, &layers, &count, nullptr) == RM_OK &&
                 count == 1 && !layers[0].visible,
             "newest visibility hides layer");
    rm_edit_layers_free(layers);
    RM_CHECK(
        rm_edit_set_node_visible(s, rm_item_id_of(LAYER), (rm_item_id){1, 25},
                                 true, nullptr) == RM_ERR_INVALID_ARGUMENT,
        "stale edit refused in both field orders");
    rm_edit_close(s);
    rm_buf_free(&b);
    rm_buf_free(&state);
  }
}

int main(void) {
  repeated_node_fields_use_the_winning_stamp();
  stale_group_links_do_not_discard_sibling_order();
  scene_neighbours_inside_deleted_ranges();
  disconnected_scene_chains_honor_known_neighbours();
  stamped_sheet_size_overrides_legacy_size();

  interior_neighbours_order_scalar_edits();
  anchor_registers_follow_independent_stamps();
  unlinked_children_inherit_layer_visibility();
  scene_preferences_follow_stamps();
  preferred_layout_defaults_follow_the_winning_mode();
  attachment_name_follows_stamp();
  paragraph_style_and_width_follow_stamps();

  text_neighbours_across_left_groups();
  transitive_text_neighbours();
  text_ties_resume_sibling_branches();
  gray_marker_and_masked_eraser();
  minimum_block_version();
  link_markers_are_not_text();
  an_inline_image_is_one_object_replacement_character();
  a_code_of_nought_is_plain_text();
  a_value_that_claims_more_than_its_record_finds_no_marker_past_it();
  the_stamped_width_of_the_settings_wins_over_the_legacy_one();
  both_forms_of_a_highlight_keep_their_explicit_colour();
  the_highlighters_blue_and_orange_read_under_their_own_indexes();
  a_nested_ticked_checkbox_is_a_checkbox_item_and_keeps_its_level();
  without_state_bits_a_parent_reads_its_nested_items();
  a_marker_code_cut_short_leaves_the_record_text();
  a_settings_width_cut_short_leaves_the_legacy_one();
  a_highlight_tail_cut_short_keeps_the_highlight();
  a_version_one_speed_that_is_no_number_reads_as_nought();
  a_version_one_pen_that_is_no_number_costs_its_stroke();
  a_source_id_cut_short_is_no_source();
  a_deleted_inline_image_is_no_character();
  an_inline_image_between_bold_marks_is_set_in_bold();
  handwriting_anchored_at_and_past_an_inline_image_counts_it_as_one_character();
  a_sample_pages_blue_and_orange_highlighter_strokes_keep_their_colours();
  a_sample_pages_highlights_in_the_second_form_keep_their_explicit_colour();
  a_sample_pages_checkboxes_are_shown_as_the_tablet_shows_them();
  the_pieces_of_an_erased_shader_stroke_name_the_stroke_they_were_cut_from();
  a_ballpoint_stroke_keeps_the_pens_speed();
  a_later_text_block_that_fails_leaves_the_earlier_one();
  a_text_list_that_runs_out_keeps_what_it_holds();
  a_text_record_that_fails_inside_costs_the_list();
  a_tree_node_field_past_its_block_keeps_what_came_before();
  return rm_test_report("format_edges");
}
