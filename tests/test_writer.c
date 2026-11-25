/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The writer against the parser: a draft written and read back must answer
 * with the same text, styles, strokes and anchors. This is one half of the
 * writer's proof; the other half is the device, because only the tablet can
 * say a page is acceptable. */

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

static const uint8_t AUTHOR[16] = {0x0a, 0x41, 0x8e, 0x1a, 0x11, 0x11,
                                   0x42, 0x22, 0x83, 0x33, 0x44, 0x44,
                                   0x55, 0x55, 0x66, 0x66};

static const uint8_t OTHER[16] = {0x0a, 0x41, 0x8e, 0x1a, 0x11, 0x11,
                                  0x42, 0x22, 0x83, 0x33, 0x44, 0x44,
                                  0x55, 0x55, 0x66, 0x77};

static const uint8_t NAMED[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
                                  0x4d, 0xef, 0x80, 0x12, 0x34, 0x56,
                                  0x78, 0x9a, 0xbc, 0xde};

static rm_draft_paragraph base_paragraphs[3];
static rm_draft_point loose_points[3];
static rm_draft_point mark_points[2];
static rm_draft_stroke base_strokes[1];
static rm_draft_mark base_marks[1];
static uint8_t key[16];

static rm_page_draft sample_draft(void) {
  base_paragraphs[0] =
      (rm_draft_paragraph){.text = "Heading", .has_style = true, .style = 2};
  base_paragraphs[1] = (rm_draft_paragraph){.text = "Plain body"};
  base_paragraphs[2] = (rm_draft_paragraph){.text = "Bold words", .bold = true};
  loose_points[0] = rm_draft_point_make(0, 100);
  loose_points[1] = rm_draft_point_make(50, 100);
  loose_points[2] = rm_draft_point_make(100, 100);
  base_strokes[0] = (rm_draft_stroke){.tool = RM_TOOL_FINELINER_2,
                                      .color = RM_COLOR_BLACK,
                                      .thickness = 2,
                                      .points = loose_points,
                                      .point_count = 3};
  mark_points[0] = rm_draft_point_make(0, -10);
  mark_points[1] = rm_draft_point_make(40, -10);
  base_marks[0] = (rm_draft_mark){
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1},
      .pen_origin = -464,
      .stroke = {.tool = RM_TOOL_FINELINER_2,
                 .color = RM_COLOR_RED,
                 .thickness = 2,
                 .points = mark_points,
                 .point_count = 2},
  };
  for (size_t i = 0; i < 16; i += 1) key[i] = (uint8_t)i;

  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .paragraphs = base_paragraphs,
      .paragraph_count = 3,
      .strokes = base_strokes,
      .stroke_count = 1,
      .marks = base_marks,
      .mark_count = 1,
  };
  memcpy(draft.author, AUTHOR, 16);
  return draft;
}

/* Write a draft and read it straight back. Null when either half refused. */
static rm_page* round_trip(const rm_page_draft* draft) {
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;
  if (rm_write_page(draft, &bytes, &size, &error) != RM_OK) return nullptr;
  rm_page* page = nullptr;
  rm_status status = rm_parse_page(bytes, size, &page, &error);
  rm_bytes_free(bytes);
  return status == RM_OK ? page : nullptr;
}

static void a_written_page_round_trips(void) {
  rm_page_draft draft = sample_draft();
  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "the page writes and reads back");
  if (page == nullptr) return;
  RM_CHECK(page->has_text, "it carries text");
  RM_CHECK_EQ_SIZE(page->text.paragraph_count, 3, "three paragraphs");
  size_t formatted = 0;
  for (size_t i = 0; i < page->text.paragraph_count; i += 1) {
    if (page->text.paragraphs[i].has_format) formatted += 1;
  }
  RM_CHECK_EQ_SIZE(formatted, 1, "one of them carries a style record");
  RM_CHECK_EQ_SIZE(page->stroke_count, 2, "the loose stroke and the mark");
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "and nothing was lost on the way");
  rm_page_free(page);
}

static void a_loose_stroke_stays_loose_and_a_mark_stays_anchored(void) {
  rm_page_draft draft = sample_draft();
  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "the page round-trips");
  if (page == nullptr || page->stroke_count != 2) return;

  const rm_stroke* loose = &page->strokes[0];
  RM_CHECK(!loose->has_anchor, "the loose stroke hangs on nothing");
  RM_CHECK(loose->tool == RM_TOOL_FINELINER_2, "its tool survived");
  RM_CHECK(loose->color == RM_COLOR_BLACK, "and its colour");
  RM_CHECK_EQ_SIZE(loose->point_count, 3, "and its points");

  const rm_stroke* marked = &page->strokes[1];
  RM_CHECK(marked->has_anchor, "the mark is fastened");
  RM_CHECK(marked->anchor.paragraph == 1, "to its paragraph (%d)",
           marked->anchor.paragraph);
  RM_CHECK(marked->anchor.has_origin_x && marked->anchor.origin_x == -464,
           "and it carries the pen's horizontal");
  RM_CHECK(marked->color == RM_COLOR_RED, "its colour survived");
  RM_CHECK_EQ_SIZE(marked->point_count, 2, "and its points");
  rm_page_free(page);
}

static void images_survive_the_round_trip(void) {
  rm_page_draft draft = sample_draft();
  rm_draft_image image = {.file = "0a418e1a-1111-4222-8333-444455556666.png",
                          .x = -50,
                          .y = 100,
                          .width = 120,
                          .height = 80};
  memcpy(image.key, key, 16);
  draft.images = &image;
  draft.image_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->image_count == 1, "one image comes back");
  if (page == nullptr || page->image_count != 1) {
    rm_page_free(page);
    return;
  }
  RM_CHECK_EQ_STR(page->images[0].file.bytes, page->images[0].file.length,
                  "0a418e1a-1111-4222-8333-444455556666.png",
                  "with its file name");
  RM_CHECK(page->images[0].vertices[0].x == -50 &&
               page->images[0].vertices[0].y == 100,
           "and its first corner");
  /* Both strokes stand before the image: it joined the chain last. */
  RM_CHECK_EQ_SIZE(page->images[0].strokes_before, 2,
                   "and its place in the drawing order");
  RM_CHECK(!page->images[0].has_anchor, "a loose image hangs on nothing");
  rm_page_free(page);
}

/* A page with no typed text carries no RootText block at all — an image-only
 * page from the tablet does not, and a generated page is worth what it has in
 * common with the tablet's own file. */
static void an_image_only_page_has_no_text(void) {
  rm_page_draft draft = sample_draft();
  draft.paragraphs = nullptr;
  draft.paragraph_count = 0;
  draft.strokes = nullptr;
  draft.stroke_count = 0;
  draft.marks = nullptr;
  draft.mark_count = 0;
  rm_draft_image image = {.file = "0a418e1a-1111-4222-8333-444455556666.png",
                          .x = -60,
                          .y = 200,
                          .width = 120,
                          .height = 160};
  memcpy(image.key, key, 16);
  draft.images = &image;
  draft.image_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "an image-only page round-trips");
  if (page == nullptr) return;
  RM_CHECK(!page->has_text, "and carries no text block");
  RM_CHECK_EQ_SIZE(page->image_count, 1, "one image");
  RM_CHECK_EQ_SIZE(page->stroke_count, 0, "and no strokes");
  rm_page_free(page);
}

static void an_anchored_image_comes_back_fastened(void) {
  rm_page_draft draft = sample_draft();
  rm_draft_image image = {
      .file = "0a418e1a-1111-4222-8333-444455556666.png",
      .x = 40,
      .y = 30,
      .width = 120,
      .height = 80,
      .has_anchor = true,
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1},
      .pen_origin = -464};
  memcpy(image.key, key, 16);
  draft.images = &image;
  draft.image_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->image_count == 1,
           "the anchored image comes back");
  if (page == nullptr || page->image_count != 1) {
    rm_page_free(page);
    return;
  }
  RM_CHECK(page->images[0].has_anchor, "fastened");
  RM_CHECK(page->images[0].anchor.paragraph == 1, "to its paragraph");
  RM_CHECK(page->images[0].anchor.has_origin_x &&
               page->images[0].anchor.origin_x == -464,
           "with the pen's horizontal");
  /* The vertices stay the pen's: where they land is the layout's to say, and
   * the parser does not lay text out. */
  RM_CHECK(page->images[0].vertices[0].x == 40 &&
               page->images[0].vertices[0].y == 30,
           "and its corners counted from the pen");
  rm_page_free(page);
}

/* The file counts characters as Unicode scalars and spells the length of a
 * record in bytes, and outside ASCII the two are different numbers. Every
 * other fixture here is ASCII, where they agree — which is exactly the
 * condition under which a confusion between them would go unseen. */
static void text_outside_ascii_round_trips(void) {
  rm_page_draft draft = sample_draft();
  rm_draft_paragraph paragraphs[] = {
      {.text = "Grüße, Welt", .has_style = true, .style = 2},
      {.text = "Ελληνικά and English"},
      {.text = "絵文字 🖋 emoji"},
  };
  rm_draft_point point = rm_draft_point_make(0, -10);
  rm_draft_mark mark = {
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 2},
      .pen_origin = -464,
      .stroke = {.tool = RM_TOOL_FINELINER_2,
                 .color = RM_COLOR_RED,
                 .thickness = 2,
                 .points = &point,
                 .point_count = 1},
  };
  draft.paragraphs = paragraphs;
  draft.paragraph_count = 3;
  draft.marks = &mark;
  draft.mark_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "the page round-trips");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->text.paragraph_count, 3, "three paragraphs");
  if (page->text.paragraph_count == 3) {
    RM_CHECK_EQ_STR(page->text.paragraphs[0].text.bytes,
                    page->text.paragraphs[0].text.length, "Grüße, Welt",
                    "Latin with diacritics");
    RM_CHECK_EQ_STR(page->text.paragraphs[1].text.bytes,
                    page->text.paragraphs[1].text.length,
                    "Ελληνικά and English", "Greek");
    RM_CHECK_EQ_STR(page->text.paragraphs[2].text.bytes,
                    page->text.paragraphs[2].text.length, "絵文字 🖋 emoji",
                    "CJK and an emoji");
  }
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "nothing was lost");
  RM_CHECK(page->stroke_count == 2 && page->strokes[1].has_anchor &&
               page->strokes[1].anchor.paragraph == 2,
           "and the mark found its paragraph past the non-ASCII lines");
  rm_page_free(page);
}

/* The two sentinels name no paragraph and can never fail to resolve, so a page
 * with no text at all still takes ink hung before and after it. */
static void the_sentinels_always_resolve(void) {
  rm_page_draft draft = sample_draft();
  draft.paragraphs = nullptr;
  draft.paragraph_count = 0;
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = &point,
                            .point_count = 1};
  rm_draft_mark marks[2] = {
      {.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
       .pen_origin = 0,
       .stroke = stroke},
      {.anchor = {.kind = RM_ANCHOR_AFTER_TEXT},
       .pen_origin = 0,
       .stroke = stroke},
  };
  draft.marks = marks;
  draft.mark_count = 2;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr,
           "a page with no paragraphs still takes anchored ink");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->stroke_count, 3, "one loose stroke and the two marks");
  rm_page_free(page);
}

/* "After the text" is just past the last character typed. Taking the sentinel
 * for an unknown character instead puts every stroke anchored to it on line
 * zero — on top of the drawing that sits above the text. */
static void the_end_sentinel_lands_on_the_last_typed_line(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = &point,
                            .point_count = 1};
  rm_draft_mark marks[2] = {
      {.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
       .pen_origin = 0,
       .stroke = stroke},
      {.anchor = {.kind = RM_ANCHOR_AFTER_TEXT},
       .pen_origin = 0,
       .stroke = stroke},
  };

  rm_page_draft draft = sample_draft();
  rm_draft_paragraph four[] = {
      {.text = "one"}, {.text = "two"}, {.text = "three"}, {.text = "four"}};
  draft.paragraphs = four;
  draft.paragraph_count = 4;
  draft.strokes = nullptr;
  draft.stroke_count = 0;
  draft.marks = marks;
  draft.mark_count = 2;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->stroke_count == 2, "both marks come back");
  if (page != nullptr && page->stroke_count == 2) {
    RM_CHECK(page->strokes[0].anchor.paragraph == 0 &&
                 page->strokes[0].anchor.offset == 0,
             "before the text is the very start");
    /* One past the last letter: an anchor names the character the pen
     * stands before, and "after everything typed" is before the character
     * that would come next. */
    RM_CHECK(page->strokes[1].anchor.paragraph == 3 &&
                 page->strokes[1].anchor.offset == 5,
             "after the text is one past the end of the last line (got %d/%d)",
             page->strokes[1].anchor.paragraph, page->strokes[1].anchor.offset);
  }
  rm_page_free(page);

  /* Text ending with newlines leaves empty paragraphs behind it. "After the
   * text" is just past the last character, and the last Return ends the
   * paragraph it closes — the second one here, not the empty third. */
  rm_draft_paragraph trailing[] = {
      {.text = "Test test"}, {.text = ""}, {.text = ""}};
  draft.paragraphs = trailing;
  draft.paragraph_count = 3;
  draft.marks = &marks[1];
  draft.mark_count = 1;

  page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
  if (page != nullptr && page->stroke_count == 1) {
    RM_CHECK(page->strokes[0].anchor.paragraph == 1 &&
                 page->strokes[0].anchor.offset == 1,
             "where the last Return stands (got %d/%d)",
             page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
  }
  rm_page_free(page);
}

/* Nineteen Returns and no letter, with handwriting anchored to paragraphs 0, 4
 * and 17 and one block to the end. On the tablet that block stands one line
 * under the one at 17 — paragraph 18, which the last Return closes. The first
 * paragraph would put it on top of the others; the last, a line too low. */
static void the_end_sentinel_on_empty_paragraphs_goes_to_the_bottom(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = &point,
                            .point_count = 1};
  rm_draft_mark mark = {.anchor = {.kind = RM_ANCHOR_AFTER_TEXT},
                        .pen_origin = 0,
                        .stroke = stroke};
  rm_draft_paragraph empty[20];
  for (size_t i = 0; i < 20; i += 1)
    empty[i] = (rm_draft_paragraph){.text = ""};

  rm_page_draft draft = sample_draft();
  draft.paragraphs = empty;
  draft.paragraph_count = 20;
  draft.strokes = nullptr;
  draft.stroke_count = 0;
  draft.marks = &mark;
  draft.mark_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
  if (page != nullptr && page->stroke_count == 1) {
    RM_CHECK(page->strokes[0].anchor.paragraph == 18 &&
                 page->strokes[0].anchor.offset == 1,
             "under the last Return (got %d/%d)",
             page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
  }
  rm_page_free(page);
}

/* Both shapes of "nothing typed" — no paragraphs, and the single empty one —
 * end where they start. */
static void the_end_sentinel_on_no_text_stays_at_the_start(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = &point,
                            .point_count = 1};
  rm_draft_mark mark = {.anchor = {.kind = RM_ANCHOR_AFTER_TEXT},
                        .pen_origin = 0,
                        .stroke = stroke};
  rm_draft_paragraph one_empty[] = {{.text = ""}};

  for (size_t paragraphs = 0; paragraphs < 2; paragraphs += 1) {
    rm_page_draft draft = sample_draft();
    draft.paragraphs = paragraphs == 0 ? nullptr : one_empty;
    draft.paragraph_count = paragraphs;
    draft.strokes = nullptr;
    draft.stroke_count = 0;
    draft.marks = &mark;
    draft.mark_count = 1;

    rm_page* page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->stroke_count == 1,
             "the mark comes back (%zu)", paragraphs);
    if (page != nullptr && page->stroke_count == 1) {
      RM_CHECK(page->strokes[0].anchor.paragraph == 0 &&
                   page->strokes[0].anchor.offset == 0,
               "at the start with %zu paragraphs (got %d/%d)", paragraphs,
               page->strokes[0].anchor.paragraph,
               page->strokes[0].anchor.offset);
    }
    rm_page_free(page);
  }
}

/* A draft this writer cannot honour is refused, and refused as an error. This
 * library is linked into somebody else's process: aborting there takes the
 * host down, and the host can neither catch it nor fall back to anything. */
static void an_anchor_past_the_text_is_refused(void) {
  rm_page_draft draft = sample_draft();
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_mark mark = {
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 99},
      .pen_origin = 0,
      .stroke = {.tool = RM_TOOL_FINELINER_2,
                 .color = RM_COLOR_BLACK,
                 .thickness = 2,
                 .points = &point,
                 .point_count = 1},
  };
  draft.marks = &mark;
  draft.mark_count = 1;

  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;
  rm_status status = rm_write_page(&draft, &bytes, &size, &error);
  RM_CHECK(status == RM_ERR_ANCHOR_OUT_OF_RANGE,
           "a mark on a paragraph that is not there");
  RM_CHECK(error.value == 99, "and the paragraph is named (%ld)", error.value);
  RM_CHECK(bytes == nullptr, "and nothing at all was written");

  rm_page_draft with_image = sample_draft();
  rm_draft_image image = {
      .file = "a.png",
      .x = 0,
      .y = 0,
      .width = 10,
      .height = 10,
      .has_anchor = true,
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 42}};
  memcpy(image.key, key, 16);
  with_image.images = &image;
  with_image.image_count = 1;
  status = rm_write_page(&with_image, &bytes, &size, &error);
  RM_CHECK(status == RM_ERR_ANCHOR_OUT_OF_RANGE, "and an image likewise");
  RM_CHECK(error.value == 42, "with its own paragraph named (%ld)",
           error.value);
}

/* Past these the identifier ranges run into one another and two items are
 * handed one counter — and the file cannot tell two meanings of one counter
 * apart. */
static void counters_that_would_collide_are_refused(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = &point,
                            .point_count = 1};
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;

  constexpr size_t mark_limit = 12500;
  rm_draft_mark* marks = calloc(mark_limit + 1u, sizeof *marks);
  for (size_t i = 0; i <= mark_limit; i += 1) {
    marks[i] = (rm_draft_mark){.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
                               .stroke = stroke};
  }
  rm_page_draft too_many_marks = sample_draft();
  too_many_marks.marks = marks;
  too_many_marks.mark_count = mark_limit + 1u;
  RM_CHECK(rm_write_page(&too_many_marks, &bytes, &size, &error) ==
               RM_ERR_TOO_MANY_TO_NUMBER,
           "too many marks");
  RM_CHECK(error.value == (long)mark_limit, "the limit travels (%ld)",
           error.value);
  RM_CHECK(strcmp(error.message, "marks") == 0,
           "and so does what there are too many of");
  free(marks);

  constexpr size_t stroke_limit = 100000;
  rm_draft_stroke* strokes = calloc(stroke_limit + 1u, sizeof *strokes);
  for (size_t i = 0; i <= stroke_limit; i += 1) strokes[i] = stroke;
  rm_page_draft too_many_strokes = sample_draft();
  too_many_strokes.strokes = strokes;
  too_many_strokes.stroke_count = stroke_limit + 1u;
  RM_CHECK(rm_write_page(&too_many_strokes, &bytes, &size, &error) ==
               RM_ERR_TOO_MANY_TO_NUMBER,
           "too many strokes");
  RM_CHECK(error.value == (long)stroke_limit, "the limit travels (%ld)",
           error.value);
  free(strokes);

  constexpr size_t character_limit = 99980;
  char* long_text = malloc(character_limit + 1u);
  memset(long_text, 'a', character_limit);
  long_text[character_limit] = '\0';
  rm_draft_paragraph one = {.text = long_text};
  rm_page_draft too_much_text = sample_draft();
  too_much_text.paragraphs = &one;
  too_much_text.paragraph_count = 1;
  too_much_text.marks = nullptr;
  too_much_text.mark_count = 0;
  RM_CHECK(rm_write_page(&too_much_text, &bytes, &size, &error) ==
               RM_ERR_TOO_MANY_TO_NUMBER,
           "too many characters");
  RM_CHECK(error.value == (long)character_limit, "the limit travels (%ld)",
           error.value);

  /* Over two limits at once, the characters are named first — the order the
   * writer checks in. */
  marks = calloc(mark_limit + 1u, sizeof *marks);
  for (size_t i = 0; i <= mark_limit; i += 1) {
    marks[i] = (rm_draft_mark){.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
                               .stroke = stroke};
  }
  too_much_text.marks = marks;
  too_much_text.mark_count = mark_limit + 1u;
  RM_CHECK(rm_write_page(&too_much_text, &bytes, &size, &error) ==
               RM_ERR_TOO_MANY_TO_NUMBER,
           "too many of both");
  RM_CHECK(strcmp(error.message, "characters") == 0,
           "the characters are named first (%s)", error.message);
  free(marks);
  free(long_text);
}

/* Bytes that are not UTF-8 are written the way a UTF-8 decoder repairs them:
 * each ill-formed run one U+FFFD. Counted as written: a stray continuation
 * byte counted as no character would hand the next paragraph's first letter
 * the counter its newline has on read-back, and the mark fastened there would
 * land a line too high. */
static void ill_formed_text_is_written_as_replacement_characters(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  rm_draft_mark mark = {
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1},
      .stroke =
          rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &point, 1),
  };
  rm_draft_paragraph paragraphs[] = {{.text = "a\x80"
                                              "b"},
                                     {.text = "cd"}};
  rm_page_draft draft = sample_draft();
  draft.paragraphs = paragraphs;
  draft.paragraph_count = 2;
  draft.strokes = nullptr;
  draft.stroke_count = 0;
  draft.marks = &mark;
  draft.mark_count = 1;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->has_text && page->text.paragraph_count == 2,
           "the text comes back");
  if (page != nullptr && page->has_text && page->text.paragraph_count == 2) {
    RM_CHECK_EQ_STR(page->text.paragraphs[0].text.bytes,
                    page->text.paragraphs[0].text.length,
                    "a\xEF\xBF\xBD"
                    "b",
                    "the stray byte one U+FFFD");
  }
  RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
  if (page != nullptr && page->stroke_count == 1) {
    RM_CHECK(page->strokes[0].anchor.paragraph == 1 &&
                 page->strokes[0].anchor.offset == 1,
             "on the first letter of the second paragraph (got %d/%d)",
             page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
  }
  rm_page_free(page);
}

/* The stroke a draft makes when only the tool, the colour and the points are
 * named: no argb, thickness 2. */
static void a_stroke_made_with_defaults_has_the_default_ones(void) {
  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(10, 0)};
  rm_draft_stroke stroke =
      rm_draft_stroke_make(RM_TOOL_PENCIL_2, RM_COLOR_BLUE, points, 2);
  RM_CHECK(stroke.tool == RM_TOOL_PENCIL_2 && stroke.color == RM_COLOR_BLUE,
           "tool and colour");
  RM_CHECK(!stroke.has_argb && stroke.thickness == 2, "no argb, thickness 2");
  RM_CHECK(stroke.points == points && stroke.point_count == 2,
           "the points, borrowed");

  rm_page_draft draft = sample_draft();
  draft.strokes = &stroke;
  draft.stroke_count = 1;
  draft.marks = nullptr;
  draft.mark_count = 0;
  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->stroke_count == 1, "the stroke comes back");
  if (page != nullptr && page->stroke_count == 1) {
    RM_CHECK(page->strokes[0].thickness_scale == 2, "with thickness 2 (%g)",
             page->strokes[0].thickness_scale);
  }
  rm_page_free(page);
}

static bool writes_the_same_bytes(const rm_page_draft* a,
                                  const rm_page_draft* b) {
  uint8_t *first = nullptr, *second = nullptr;
  size_t first_size = 0, second_size = 0;
  rm_error error;
  if (rm_write_page(a, &first, &first_size, &error) != RM_OK) return false;
  if (rm_write_page(b, &second, &second_size, &error) != RM_OK) {
    rm_bytes_free(first);
    return false;
  }
  bool same =
      first_size == second_size && memcmp(first, second, first_size) == 0;
  rm_bytes_free(first);
  rm_bytes_free(second);
  return same;
}

/* The same draft has to write the same bytes. A page whose text carries a
 * bold or an italic stretch names a second author; a fresh UUID for it on
 * every call would make two writes of one draft differ, rule out a byte-level
 * regression test, and show content-addressed storage a change where there is
 * none. */
static void the_writer_is_deterministic(void) {
  rm_draft_paragraph plain_text[] = {{.text = "No faces here"}};
  rm_page_draft plain = sample_draft();
  plain.paragraphs = plain_text;
  plain.paragraph_count = 1;
  plain.marks = nullptr;
  plain.mark_count = 0;
  RM_CHECK(writes_the_same_bytes(&plain, &plain),
           "a plain page writes one page");

  /* The case that was not deterministic: faces bring the second author. */
  rm_draft_paragraph faced_text[] = {{.text = "Bold", .bold = true},
                                     {.text = "Italic", .italic = true}};
  rm_page_draft faced = sample_draft();
  faced.paragraphs = faced_text;
  faced.paragraph_count = 2;
  faced.marks = nullptr;
  faced.mark_count = 0;
  RM_CHECK(writes_the_same_bytes(&faced, &faced), "and so does a faced one");

  /* Two drafts alike but for the author write different pages, which is what
   * says the derived author really is derived from that one. */
  rm_page_draft other = faced;
  memcpy(other.author, OTHER, 16);
  RM_CHECK(!writes_the_same_bytes(&faced, &other),
           "a different author writes a different page");
}

static void an_explicit_marks_author_is_used(void) {
  rm_draft_paragraph faced_text[] = {{.text = "Bold", .bold = true}};
  rm_page_draft faced = sample_draft();
  faced.paragraphs = faced_text;
  faced.paragraph_count = 1;
  faced.marks = nullptr;
  faced.mark_count = 0;

  rm_page_draft named = faced;
  named.has_marks_author = true;
  memcpy(named.marks_author, NAMED, 16);
  RM_CHECK(writes_the_same_bytes(&named, &named),
           "a named author is still deterministic");
  RM_CHECK(!writes_the_same_bytes(&named, &faced),
           "and is not the derived one");

  /* A page with no faces names no second author at all, so the field changes
   * nothing there. */
  rm_draft_paragraph plain_text[] = {{.text = "No faces here"}};
  rm_page_draft plain = sample_draft();
  plain.paragraphs = plain_text;
  plain.paragraph_count = 1;
  plain.marks = nullptr;
  plain.mark_count = 0;
  rm_page_draft plain_named = plain;
  plain_named.has_marks_author = true;
  memcpy(plain_named.marks_author, NAMED, 16);
  RM_CHECK(writes_the_same_bytes(&plain, &plain_named),
           "with no faces the field is unread");
}

/* A page's own template name is not in the `.rm` — it comes from the
 * document's index — so the parser never sets it and the layer that holds
 * documents does. */
static void a_template_can_be_stamped_on_a_parsed_page(void) {
  rm_page_draft draft = sample_draft();
  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "the page round-trips");
  if (page == nullptr) return;
  RM_CHECK(!page->has_template_name, "a parsed page names no template");
  RM_CHECK(rm_page_set_template(page, "Grid") == RM_OK, "and takes one");
  RM_CHECK_EQ_STR(page->template_name.bytes, page->template_name.length, "Grid",
                  "the name");
  RM_CHECK(rm_page_set_template(page, nullptr) == RM_OK, "null clears it");
  RM_CHECK(!page->has_template_name, "and it is gone");
  rm_page_free(page);
}

/* Several images on one page, each with its own key. The map is read into a
 * hash rather than scanned, and the failure a hash brings that a scan
 * could not is two distinct keys landing in one slot — which would give two
 * pictures the same file name and draw one of them twice. The keys below
 * differ only in their last byte, which is where a hash that folds the front
 * of the key and stops would collide. */
static void several_images_keep_their_own_names(void) {
  rm_page_draft draft = sample_draft();
  rm_draft_image images[4];
  static const char* const names[4] = {"first.png", "second.png", "third.png",
                                       "fourth.png"};
  for (size_t i = 0; i < 4; i += 1) {
    images[i] = (rm_draft_image){.file = names[i],
                                 .x = (float)(-50 + 10 * (int)i),
                                 .y = 100,
                                 .width = 120,
                                 .height = 80};
    memcpy(images[i].key, key, 16);
    images[i].key[15] = (uint8_t)(0xE0u + i);
  }
  draft.images = images;
  draft.image_count = 4;

  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr, "the page round-trips");
  if (page == nullptr) return;
  RM_CHECK_EQ_SIZE(page->image_count, 4, "all four images come back");
  size_t matched = page->image_count < 4 ? page->image_count : 4;
  for (size_t i = 0; i < matched; i += 1) {
    RM_CHECK_EQ_STR(page->images[i].file.bytes, page->images[i].file.length,
                    names[i], "image %zu keeps its own name", i);
  }
  rm_page_free(page);
}

/* A count without its items is a caller's slip, and the writer answers it at
 * the door: read on trust it was a null dereference inside the text layout,
 * which is a crash in a library whose whole job is to be handed other
 * people's data. A null beside a count of nought stays what it always was —
 * a draft saying it has none of that kind. */
static void a_count_without_its_items_is_refused(void) {
  struct {
    const char* what;
    size_t paragraphs, strokes, marks, images;
  } cases[] = {
      {"paragraphs", 2, 0, 0, 0},
      {"strokes", 0, 2, 0, 0},
      {"marks", 0, 0, 2, 0},
      {"images", 0, 0, 0, 2},
  };
  for (size_t i = 0; i < sizeof cases / sizeof *cases; i += 1) {
    rm_page_draft draft = {
        .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
    draft.paragraph_count = cases[i].paragraphs;
    draft.stroke_count = cases[i].strokes;
    draft.mark_count = cases[i].marks;
    draft.image_count = cases[i].images;
    uint8_t* bytes = nullptr;
    size_t size = 0;
    rm_error error = {0};
    rm_status status = rm_write_page(&draft, &bytes, &size, &error);
    RM_CHECK(status == RM_ERR_INVALID_ARGUMENT,
             "a null %s array is refused (got %s)", cases[i].what,
             rm_status_string(status));
    RM_CHECK(strcmp(error.message, cases[i].what) == 0,
             "and the error names it (got \"%s\")", error.message);
    RM_CHECK(bytes == nullptr, "and nothing is handed back");
    rm_bytes_free(bytes);
  }

  /* The other half of the rule: nought and null together are an empty page,
   * not a refusal. */
  rm_page_draft empty = {
      .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;
  RM_CHECK(rm_write_page(&empty, &bytes, &size, &error) == RM_OK,
           "a draft with nothing on it still writes a page");
  RM_CHECK(size > RM_HEADER_LENGTH, "and the page has a body");
  rm_bytes_free(bytes);
}

/* A mark whose `anchor_origin_x` is not a number keeps its anchor and loses
 * the horizontal: the field is optional, a renderer without it recomputes,
 * and a renderer handed a NaN draws the handwriting nowhere. */
static void a_nan_origin_is_left_unset(void) {
  rm_page_draft draft = sample_draft();
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;
  RM_CHECK(rm_write_page(&draft, &bytes, &size, &error) == RM_OK,
           "the page is written");
  if (bytes == nullptr) return;

  /* Tag 2 as a byte4 (0x24) followed by the float32 of -464, the pen origin
   * the sample's mark carries. */
  static const uint8_t origin[] = {0x24, 0x00, 0x00, 0xE8, 0xC3};
  size_t at = SIZE_MAX;
  for (size_t i = 0; i + sizeof origin <= size; i += 1) {
    if (memcmp(bytes + i, origin, sizeof origin) == 0) {
      at = i;
      break;
    }
  }
  RM_CHECK(at != SIZE_MAX, "the stamped origin is found");
  if (at != SIZE_MAX) {
    static const uint8_t nan32[] = {0x00, 0x00, 0xC0, 0x7F};
    memcpy(bytes + at + 1, nan32, sizeof nan32);
    rm_page* page = nullptr;
    RM_CHECK(rm_parse_page(bytes, size, &page, &error) == RM_OK,
             "the page parses");
    if (page != nullptr) {
      RM_CHECK_EQ_SIZE(page->stroke_count, 2, "both strokes are drawn");
      if (page->stroke_count == 2) {
        RM_CHECK(page->strokes[1].has_anchor, "the mark keeps its anchor");
        RM_CHECK(!page->strokes[1].anchor.has_origin_x,
                 "and loses only the horizontal");
      }
      rm_page_free(page);
    }
  }
  rm_bytes_free(bytes);
}

static void missing_point_arrays_are_refused(void) {
  for (unsigned anchored = 0; anchored < 2; anchored += 1) {
    rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                              .thickness = 1,
                              .points = nullptr,
                              .point_count = 1};
    rm_draft_mark mark = {.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
                          .stroke = stroke};
    rm_page_draft draft = {
        .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
    if (anchored) {
      draft.marks = &mark;
      draft.mark_count = 1;
    } else {
      draft.strokes = &stroke;
      draft.stroke_count = 1;
    }
    uint8_t* bytes = nullptr;
    size_t size = 123;
    rm_error error;
    rm_status status = rm_write_page(&draft, &bytes, &size, &error);
    RM_CHECK(status == RM_ERR_INVALID_ARGUMENT && error.status == status,
             "a missing %s point array is refused",
             anchored ? "anchored" : "loose");
    RM_CHECK(strcmp(error.message,
                    anchored ? "marks.stroke.points" : "strokes.points") == 0,
             "the missing array is named");
    RM_CHECK(bytes == nullptr && size == 123,
             "outputs are untouched on failure");
    rm_bytes_free(bytes);

    stroke.point_count = 0;
    mark.stroke.point_count = 0;
    bytes = nullptr;
    status = rm_write_page(&draft, &bytes, &size, &error);
    RM_CHECK(status == RM_OK, "a null array with zero points remains valid");
    rm_bytes_free(bytes);
  }
}

static void invalid_writer_inputs_are_refused(void) {
  uint8_t* bytes = nullptr;
  size_t size = 0;
  rm_error error;

  /* Embedded newline in paragraph text is prohibited by writer.h. */
  rm_draft_paragraph bad_para[] = {{.text = "line1\nline2"}};
  rm_page_draft d1 = sample_draft();
  d1.paragraphs = bad_para;
  d1.paragraph_count = 1;
  d1.marks = nullptr;
  d1.mark_count = 0;
  RM_CHECK(rm_write_page(&d1, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "embedded newline in paragraph is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_paragraph bad_cr[] = {{.text = "line1\rline2"}};
  d1.paragraphs = bad_cr;
  rm_page* cr_page = round_trip(&d1);
  RM_CHECK(cr_page && cr_page->text.paragraph_count == 1,
           "carriage return stays in one paragraph");
  if (cr_page && cr_page->text.paragraph_count == 1) {
    rm_str text = cr_page->text.paragraphs[0].text;
    RM_CHECK_EQ_STR(text.bytes, text.length, bad_cr[0].text,
                    "carriage return round-trips");
  }
  rm_page_free(cr_page);

  /* Non-finite floats in stroke/point/image/sheet/column/mark are refused. */
  rm_draft_point bad_pt = rm_draft_point_make(NAN, 0);
  rm_draft_stroke bad_stroke =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &bad_pt, 1);
  rm_page_draft d2 = sample_draft();
  d2.strokes = &bad_stroke;
  d2.stroke_count = 1;
  d2.marks = nullptr;
  d2.mark_count = 0;
  RM_CHECK(rm_write_page(&d2, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "NaN point coordinate is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_point good_pt = rm_draft_point_make(0, 0);
  rm_draft_stroke bad_thick =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &good_pt, 1);
  bad_thick.thickness = INFINITY;
  d2.strokes = &bad_thick;
  RM_CHECK(rm_write_page(&d2, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "Inf stroke thickness is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_page_draft d_tool = sample_draft();
  d_tool.marks = nullptr;
  d_tool.mark_count = 0;
  rm_draft_stroke bad_tool =
      rm_draft_stroke_make(RM_TOOL_UNKNOWN, RM_COLOR_BLACK, &good_pt, 1);
  d_tool.strokes = &bad_tool;
  d_tool.stroke_count = 1;
  RM_CHECK(
      rm_write_page(&d_tool, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
      "unknown pen tool is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_stroke bad_color =
      rm_draft_stroke_make(RM_TOOL_FINELINER_2, (rm_pen_color)999, &good_pt, 1);
  d_tool.strokes = &bad_color;
  RM_CHECK(
      rm_write_page(&d_tool, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
      "invalid pen color is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_page_draft d3 = sample_draft();
  d3.marks = nullptr;
  d3.mark_count = 0;
  d3.sheet.canvas_width = NAN;
  RM_CHECK(rm_write_page(&d3, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "NaN sheet canvas_width is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_page_draft d4 = sample_draft();
  d4.marks = nullptr;
  d4.mark_count = 0;
  d4.column.width = -1.0f;
  RM_CHECK(rm_write_page(&d4, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "non-positive column width is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_image bad_img = {
      .file = "a.png", .x = NAN, .width = 10, .height = 10};
  rm_page_draft d5 = sample_draft();
  d5.marks = nullptr;
  d5.mark_count = 0;
  d5.images = &bad_img;
  d5.image_count = 1;
  RM_CHECK(rm_write_page(&d5, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "NaN image coordinate is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_image empty_name = {.file = "", .width = 10, .height = 10};
  d5.images = &empty_name;
  RM_CHECK(rm_write_page(&d5, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "empty image filename is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_image dup_imgs[2] = {
      {.key = {1}, .file = "a.png", .width = 10, .height = 10},
      {.key = {1}, .file = "b.png", .width = 10, .height = 10},
  };
  d5.images = dup_imgs;
  d5.image_count = 2;
  RM_CHECK(rm_write_page(&d5, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "duplicate image keys with different filenames are refused");
  rm_bytes_free(bytes);
  bytes = nullptr;

  rm_draft_mark bad_mark = {
      .anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
      .pen_origin = INFINITY,
      .stroke = rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK,
                                     &good_pt, 1)};
  rm_page_draft d6 = sample_draft();
  d6.marks = &bad_mark;
  d6.mark_count = 1;
  RM_CHECK(rm_write_page(&d6, &bytes, &size, &error) == RM_ERR_INVALID_ARGUMENT,
           "Inf mark pen_origin is refused");
  rm_bytes_free(bytes);
  bytes = nullptr;
}

static bool find_block_payload(const uint8_t* bytes, size_t size, uint8_t type,
                               rm_reader* out_body) {
  rm_reader r;
  rm_reader_init(&r, bytes, size);
  r.offset = RM_HEADER_LENGTH;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header hdr;
    if (rm_read_block_header(&r, &hdr) != RM_OK) return false;
    if (hdr.type == type) {
      rm_reader body = r;
      body.size = hdr.end;
      *out_body = body;
      return true;
    }
    r.offset = hdr.end;
  }
  return false;
}

static void fresh_stroke_tail_matches_its_colour(void) {
  rm_draft_point point = rm_draft_point_make(0, 0);
  for (unsigned explicit_colour = 0; explicit_colour <= 1; ++explicit_colour) {
    rm_draft_stroke stroke = rm_draft_stroke_make(
        RM_TOOL_HIGHLIGHTER_2,
        explicit_colour ? RM_COLOR_HIGHLIGHT : RM_COLOR_YELLOW, &point, 1);
    stroke.thickness = 1.0;

    rm_buf tail = {0};
    if (explicit_colour) {
      stroke.has_argb = true;
      stroke.argb = 0xFFBEEAFEu;
      rm_write_id(&tail, 6, RM_CRDT_NONE);
      rm_write_tag(&tail, 8, RM_TAG_BYTE4);
      rm_write_u32(&tail, stroke.argb);
    }

    rm_page_draft draft = {
        .author = {1}, .strokes = &stroke, .stroke_count = 1};
    uint8_t* bytes = nullptr;
    size_t size = 0;
    RM_CHECK(rm_write_page(&draft, &bytes, &size, nullptr) == RM_OK,
             "write fresh highlighter");
    rm_reader body, value;
    rm_item_preamble pre;
    uint8_t kind;
    bool found =
        bytes &&
        find_block_payload(bytes, size, RM_BLOCK_SCENE_LINE_ITEM, &body) &&
        rm_parse_item_preamble(&body, &pre) == RM_OK &&
        rm_expect_tag(&body, 6, RM_TAG_LENGTH4) == RM_OK &&
        rm_read_subblock(&body, &value) == RM_OK &&
        rm_read_u8(&value, &kind) == RM_OK && kind == RM_VALUE_LINE_ITEM;
    for (uint32_t i = 1; found && i <= 5; ++i) {
      uint32_t tag;
      uint8_t type;
      found = rm_read_tag(&value, &tag, &type) == RM_OK && tag == i &&
              rmi_skip_tagged_value(&value, type, nullptr) == RM_OK;
    }
    RM_CHECK(found && rm_reader_remaining(&value) == tail.count &&
                 (!tail.count || memcmp(value.data + value.offset, tail.bytes,
                                        tail.count) == 0),
             "fresh stroke has the exact tail for palette index %d",
             (int)stroke.color);

    rm_buf_free(&tail);
    rm_bytes_free(bytes);
  }
}

static void a4_writer_columns_authors_limits_and_palette_contracts(void) {
  /* 1) Column width +0.0f / -0.0f vs invalid width and setting (R7-2, R82,
   * R105, R109, R114, R122, R130, R132). */
  const float zero_widths[2] = {0.0f, -0.0f};
  for (size_t z = 0; z < 2; ++z) {
    rm_page_draft d = {.author = {1},
                       .sheet = {1404, 1872, 1404, 1872},
                       .column = {.width = zero_widths[z], .setting = 1}};
    uint8_t* bytes = nullptr;
    size_t size = 0;
    RM_CHECK(rm_write_page(&d, &bytes, &size, nullptr) == RM_OK,
             "zero column width signbit=%zu succeeds", z);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(bytes, size, &s, nullptr) == RM_OK &&
                 rm_edit_replace_text(s, 0, 0, "Hi", (rm_item_id){1, 50},
                                      nullptr) == RM_OK,
             "edit text after zero-width draft");
    rm_page* p = nullptr;
    size_t sz = 0;
    const uint8_t* ebytes = rm_edit_bytes(s, &sz);
    RM_CHECK(rm_parse_page(ebytes, sz, &p, nullptr) == RM_OK && p != nullptr &&
                 p->has_text && p->text.column_width == 936.0 &&
                 p->text.origin_x == -468.0,
             "zero width resolves identically to 936/-468");
    rm_page_free(p);
    rm_edit_close(s);
    rm_bytes_free(bytes);
  }

  const float bad_widths[3] = {-1.0f, NAN, -INFINITY};
  for (size_t i = 0; i < 3; ++i) {
    rm_page_draft d = {.author = {1},
                       .sheet = {1404, 1872, 1404, 1872},
                       .column = {.width = bad_widths[i], .setting = 1}};
    uint8_t* sentinel_out = (uint8_t*)0x1;
    size_t sentinel_sz = 777;
    RM_CHECK(rm_write_page(&d, &sentinel_out, &sentinel_sz, nullptr) ==
                     RM_ERR_INVALID_ARGUMENT &&
                 sentinel_out == (uint8_t*)0x1 && sentinel_sz == 777,
             "invalid column.width[%zu] refuses with untouched outs", i);
  }

  const uint8_t bad_settings[2] = {3, 128};
  for (size_t i = 0; i < 2; ++i) {
    rm_page_draft d = {.author = {1},
                       .sheet = {1404, 1872, 1404, 1872},
                       .column = {.width = 0.0f, .setting = bad_settings[i]}};
    uint8_t* sentinel_out = (uint8_t*)0x1;
    size_t sentinel_sz = 888;
    rm_error err = {0};
    RM_CHECK(rm_write_page(&d, &sentinel_out, &sentinel_sz, &err) ==
                     RM_ERR_INVALID_ARGUMENT &&
                 strcmp(err.message, "column.setting") == 0 &&
                 sentinel_out == (uint8_t*)0x1 && sentinel_sz == 888,
             "unsupported column.setting=%u refuses with untouched outs",
             bad_settings[i]);
  }

  const struct {
    uint8_t setting;
    uint32_t sw, sh;
    double width, origin_x;
  } setting_cases[] = {
      {0, 1404, 1872, 740.0, -370.0},  {1, 1404, 1872, 936.0, -468.0},
      {2, 1404, 1872, 1152.0, -576.0}, {4, 1600, 2000, 1152.0, -576.0},
      {4, 1600, 0, 936.0, -468.0},
  };
  for (size_t i = 0; i < sizeof setting_cases / sizeof *setting_cases; ++i) {
    rm_draft_paragraph para = {.text = "X"};
    rm_page_draft d = {
        .author = {1},
        .sheet = {setting_cases[i].sw, setting_cases[i].sh, 0, 0},
        .column = {.width = 0.0f, .setting = setting_cases[i].setting},
        .paragraphs = &para,
        .paragraph_count = 1};
    uint8_t* wbytes = nullptr;
    size_t wsz = 0;
    RM_CHECK(rm_write_page(&d, &wbytes, &wsz, nullptr) == RM_OK,
             "write column case %zu", i);
    rm_page* wp = nullptr;
    RM_CHECK(rm_parse_page(wbytes, wsz, &wp, nullptr) == RM_OK &&
                 wp != nullptr &&
                 wp->text.column_width == setting_cases[i].width &&
                 wp->text.origin_x == setting_cases[i].origin_x,
             "writer column case %zu -> width=%g origin=%g", i,
             setting_cases[i].width, setting_cases[i].origin_x);
    rm_page_free(wp);
    rm_bytes_free(wbytes);

    /* Now empty text draft opened in editor and edited at 0 -> same resolved
     * width and origin_x. */
    d.paragraphs = nullptr;
    d.paragraph_count = 0;
    RM_CHECK(rm_write_page(&d, &wbytes, &wsz, nullptr) == RM_OK,
             "write empty column case %zu", i);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(wbytes, wsz, &s, nullptr) == RM_OK &&
                 rm_edit_replace_text(s, 0, 0, "X", (rm_item_id){1, 50},
                                      nullptr) == RM_OK,
             "editor text case %zu", i);
    size_t esz = 0;
    const uint8_t* eb = rm_edit_bytes(s, &esz);
    rm_page* ep = nullptr;
    RM_CHECK(rm_parse_page(eb, esz, &ep, nullptr) == RM_OK && ep != nullptr &&
                 ep->text.column_width == setting_cases[i].width &&
                 ep->text.origin_x == setting_cases[i].origin_x,
             "editor column case %zu matches writer", i);
    rm_page_free(ep);
    rm_edit_close(s);
    rm_bytes_free(wbytes);
  }

  /* Explicit width=740 with setting=4 writes tag 4=1152, tag 5=740, tag
   * 3.x=-370 (R114). */
  {
    rm_draft_paragraph para = {.text = "Auto"};
    rm_page_draft d = {.author = {1},
                       .sheet = {1600, 2000, 1600, 2000},
                       .column = {.width = 740.0f, .setting = 4},
                       .paragraphs = &para,
                       .paragraph_count = 1};
    uint8_t* bytes = nullptr;
    size_t size = 0;
    RM_CHECK(rm_write_page(&d, &bytes, &size, nullptr) == RM_OK,
             "write Auto with explicit width");
    rm_reader body;
    bool checked_rt = false;
    if (find_block_payload(bytes, size, RM_BLOCK_ROOT_TEXT, &body)) {
      rm_crdt_id rid;
      rm_reader items, pos, settings, sw;
      double ox = 0.0, oy = 0.0;
      float t4 = 0.0f, t5 = 0.0f;
      rm_crdt_id stamp;
      if (rm_read_crdt(&body, 1, &rid) == RM_OK &&
          rm_expect_tag(&body, 2, RM_TAG_LENGTH4) == RM_OK &&
          rm_read_subblock(&body, &items) == RM_OK &&
          rm_expect_tag(&body, 3, RM_TAG_LENGTH4) == RM_OK &&
          rm_read_subblock(&body, &pos) == RM_OK &&
          rm_read_f64(&pos, &ox) == RM_OK && rm_read_f64(&pos, &oy) == RM_OK &&
          rm_expect_tag(&body, 4, RM_TAG_BYTE4) == RM_OK &&
          rm_read_f32(&body, &t4) == RM_OK &&
          rm_expect_tag(&body, 5, RM_TAG_LENGTH4) == RM_OK &&
          rm_read_subblock(&body, &settings) == RM_OK &&
          rm_expect_tag(&settings, 1, RM_TAG_LENGTH4) == RM_OK &&
          rm_read_subblock(&settings, &sw) == RM_OK &&
          rm_read_crdt(&sw, 1, &stamp) == RM_OK &&
          rm_expect_tag(&sw, 2, RM_TAG_BYTE4) == RM_OK &&
          rm_read_f32(&sw, &t5) == RM_OK) {
        checked_rt = (t4 == 1152.0f && t5 == 740.0f && ox == -370.0);
      }
    }
    RM_CHECK(checked_rt,
             "Auto explicit width writes tag4=1152 tag5=740 origin=-370");
    rm_bytes_free(bytes);
  }

  /* 2) LOOSE_STROKE_LIMIT (100000 accepted, > 20000 accepted) & two-paragraph
   * char limit (R6-1, R65, R17-1, R143). */
  {
    constexpr size_t COUNT = 25000;
    rm_draft_point pt = rm_draft_point_make(0, 0);
    rm_draft_stroke base_s =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
    rm_draft_stroke* many = calloc(COUNT, sizeof *many);
    for (size_t i = 0; i < COUNT; ++i) many[i] = base_s;
    rm_page_draft d = {.author = {1},
                       .sheet = {1404, 1872, 1404, 1872},
                       .column = {936, 1},
                       .strokes = many,
                       .stroke_count = COUNT};
    uint8_t* bytes = nullptr;
    size_t size = 0;
    RM_CHECK(rm_write_page(&d, &bytes, &size, nullptr) == RM_OK,
             "25000 strokes accepted");
    rm_bytes_free(bytes);
    free(many);

    constexpr size_t CHARS = 99977;
    char* big = malloc(CHARS + 2u);
    memset(big, 'a', CHARS);
    big[CHARS] = '\0';
    rm_draft_paragraph two_ok[] = {{.text = big}, {.text = "b"}};
    rm_page_draft dok = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .paragraphs = two_ok,
                         .paragraph_count = 2};
    RM_CHECK(rm_write_page(&dok, &bytes, &size, nullptr) == RM_OK,
             "[99977 'a', 'b'] == 99980 succeeds");
    rm_bytes_free(bytes);

    big[CHARS] = 'a';
    big[CHARS + 1] = '\0';
    bytes = nullptr;
    rm_error cerr = {0};
    RM_CHECK(rm_write_page(&dok, &bytes, &size, &cerr) ==
                     RM_ERR_TOO_MANY_TO_NUMBER &&
                 strcmp(cerr.message, "characters") == 0 && cerr.value == 99980,
             "[99978 'a', 'b'] == 99981 fails at characters limit");
    free(big);

    constexpr size_t EMPTIES = 55000;
    rm_draft_paragraph* empties = calloc(EMPTIES, sizeof *empties);
    for (size_t i = 0; i < EMPTIES; ++i) empties[i].text = "";
    rm_page_draft demp = {.author = {1},
                          .sheet = {1404, 1872, 1404, 1872},
                          .column = {936, 1},
                          .paragraphs = empties,
                          .paragraph_count = EMPTIES};
    RM_CHECK(rm_write_page(&demp, &bytes, &size, nullptr) == RM_OK,
             "55000 empty paragraphs accepted");
    rm_bytes_free(bytes);
    free(empties);
  }

  /* 3) Combined 12,501 marks + missing points fails with "marks" 12500 (R60,
   * R66). */
  {
    constexpr size_t MLIM = 12500;
    rm_draft_mark* marks = calloc(MLIM + 1u, sizeof *marks);
    for (size_t i = 0; i <= MLIM; ++i) {
      marks[i] = (rm_draft_mark){
          .anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
          .stroke = {.tool = RM_TOOL_FINELINER_2, .point_count = 1}};
    }
    rm_page_draft d = {.author = {1},
                       .sheet = {1404, 1872, 1404, 1872},
                       .column = {936, 1},
                       .marks = marks,
                       .mark_count = MLIM + 1u};
    uint8_t* bytes = nullptr;
    size_t sz = 0;
    rm_error err = {0};
    RM_CHECK(
        rm_write_page(&d, &bytes, &sz, &err) == RM_ERR_TOO_MANY_TO_NUMBER &&
            err.value == 12500 && strcmp(err.message, "marks") == 0,
        "mark limit takes precedence over null stroke points");
    free(marks);
  }

  /* 4) Nil author refused (R127); marks_author rules (R117, R137); fresh
   * colour-9 strokes and images omit moveId. Only a page with an image
   * carries the 32-byte zero rectangle in SceneInfo. */
  {
    rm_page_draft nil_auth = {.sheet = {1404, 1872, 1404, 1872},
                              .column = {936, 1}};
    uint8_t* bytes = nullptr;
    size_t size = 0;
    rm_error err = {0};
    RM_CHECK(rm_write_page(&nil_auth, &bytes, &size, &err) ==
                     RM_ERR_INVALID_ARGUMENT &&
                 strcmp(err.message, "author") == 0,
             "all-zero nil draft->author is refused");

    rm_draft_paragraph bold_p = {.text = "B", .bold = true};
    rm_page_draft bd = {.author = {1},
                        .sheet = {1404, 1872, 1404, 1872},
                        .column = {936, 1},
                        .paragraphs = &bold_p,
                        .paragraph_count = 1,
                        .has_marks_author = true};
    memcpy(bd.marks_author, bd.author, 16);
    RM_CHECK(
        rm_write_page(&bd, &bytes, &size, &err) == RM_ERR_INVALID_ARGUMENT &&
            strcmp(err.message, "marks_author") == 0,
        "bold non-empty text with marks_author == author is refused");
    memset(bd.marks_author, 0, 16);
    RM_CHECK(
        rm_write_page(&bd, &bytes, &size, &err) == RM_ERR_INVALID_ARGUMENT &&
            strcmp(err.message, "marks_author") == 0,
        "bold non-empty text with nil marks_author is refused");

    /* Author {0..0, 1} produces distinct non-nil derived marks_author. */
    rm_page_draft d_one = {.sheet = {1404, 1872, 1404, 1872},
                           .column = {936, 1},
                           .paragraphs = &bold_p,
                           .paragraph_count = 1};
    d_one.author[15] = 1;
    RM_CHECK(rm_write_page(&d_one, &bytes, &size, nullptr) == RM_OK,
             "author {0..0,1} derived marks_author succeeds");
    rm_reader abody;
    bool distinct_author = false;
    if (find_block_payload(bytes, size, RM_BLOCK_AUTHOR_IDS, &abody)) {
      size_t n = 0;
      if (rm_read_count(&abody, &n) == RM_OK && n == 2) {
        for (size_t e = 0; e < 2; ++e) {
          rm_reader entry;
          uint64_t uuid_len = 0;
          uint16_t aid = 0;
          const uint8_t* raw_uuid = nullptr;
          if (rm_expect_tag(&abody, 0, RM_TAG_LENGTH4) == RM_OK &&
              rm_read_subblock(&abody, &entry) == RM_OK &&
              rm_read_varuint(&entry, &uuid_len) == RM_OK && uuid_len == 16 &&
              rm_read_bytes(&entry, 16, &raw_uuid) == RM_OK &&
              rm_read_u16(&entry, &aid) == RM_OK && aid == 2) {
            uint8_t zeros[16] = {0};
            distinct_author = memcmp(raw_uuid, zeros, 16) != 0 &&
                              memcmp(raw_uuid, d_one.author, 16) != 0;
          }
        }
      }
    }
    RM_CHECK(distinct_author, "derived marks_author for {0..0,1} is non-nil");
    rm_bytes_free(bytes);

    /* Empty bold paragraph alongside plain text ignores nil marks_author. */
    rm_draft_paragraph empty_bold[] = {{.text = "", .bold = true},
                                       {.text = "plain"}};
    rm_page_draft eb = {.author = {1},
                        .sheet = {1404, 1872, 1404, 1872},
                        .column = {936, 1},
                        .paragraphs = empty_bold,
                        .paragraph_count = 2,
                        .has_marks_author = true};
    RM_CHECK(rm_write_page(&eb, &bytes, &size, nullptr) == RM_OK,
             "empty bold paragraph ignores nil marks_author");
    rm_bytes_free(bytes);

    /* Compare the same typed page before and after adding an image. */
    rm_draft_point pt = rm_draft_point_make(0, 0);
    rm_draft_stroke hl9 = {.tool = RM_TOOL_HIGHLIGHTER_2,
                           .color = RM_COLOR_HIGHLIGHT,
                           .has_argb = true,
                           .argb = 0x40FFFF00u,
                           .thickness = 1.0,
                           .points = &pt,
                           .point_count = 1};
    rm_draft_image im = {
        .key = {1}, .file = "a.png", .width = 10, .height = 10};
    rm_draft_paragraph tp = {.text = "Text"};
    rm_page_draft td = {.author = {1},
                        .sheet = {1404, 1872, 1404, 1872},
                        .column = {936, 1},
                        .paragraphs = &tp,
                        .paragraph_count = 1};
    for (size_t images = 0; images <= 1; ++images) {
      td.images = &im;
      td.image_count = images;
      RM_CHECK(rm_write_page(&td, &bytes, &size, nullptr) == RM_OK,
               "write SceneInfo with %zu images", images);
      rm_reader sinfo;
      uint32_t seen_tags[16] = {0};
      size_t ntags = 0;
      bool tag6_zero = false;
      if (find_block_payload(bytes, size, RM_BLOCK_SCENE_INFO, &sinfo)) {
        while (rm_reader_remaining(&sinfo) > 0 && ntags < 16) {
          uint32_t tag = 0;
          uint8_t typ = 0;
          if (rm_read_tag(&sinfo, &tag, &typ) != RM_OK) break;
          seen_tags[ntags++] = tag;
          if (tag == 6 && typ == RM_TAG_LENGTH4) {
            rm_reader ext, rbytes;
            rm_crdt_id ext_id;
            const uint8_t* ptr = nullptr;
            if (rm_read_subblock(&sinfo, &ext) == RM_OK &&
                rm_read_crdt(&ext, 1, &ext_id) == RM_OK &&
                rm_expect_tag(&ext, 2, RM_TAG_LENGTH4) == RM_OK &&
                rm_read_subblock(&ext, &rbytes) == RM_OK &&
                rm_reader_remaining(&rbytes) == 32 &&
                rm_read_bytes(&rbytes, 32, &ptr) == RM_OK) {
              uint8_t z32[32] = {0};
              tag6_zero = memcmp(ptr, z32, 32) == 0;
            }
          } else {
            if (rmi_skip_tagged_value(&sinfo, typ, nullptr) != RM_OK) break;
          }
        }
      }
      const uint32_t expected_tags[8] = {1, 2, 3, 5, 6, 7, 8, 9};
      const uint32_t text_tags[] = {1, 2, 3, 5, 7, 8, 9};
      if (images == 0) {
        RM_CHECK(
            ntags == 7 && memcmp(seen_tags, text_tags, sizeof text_tags) == 0,
            "text-only SceneInfo omits the extended content rectangle");
      } else {
        RM_CHECK(
            ntags == 8 &&
                memcmp(seen_tags, expected_tags, sizeof expected_tags) == 0 &&
                tag6_zero,
            "image SceneInfo tags in exact order 1,2,3,5,6,7,8,9");
      }
      rm_bytes_free(bytes);
    }

    rm_page_draft sd = {.author = {1},
                        .sheet = {1404, 1872, 1404, 1872},
                        .column = {936, 1},
                        .strokes = &hl9,
                        .stroke_count = 1,
                        .images = &im,
                        .image_count = 1};
    RM_CHECK(rm_write_page(&sd, &bytes, &size, nullptr) == RM_OK,
             "write color-9 + image draft");
    rm_reader lbody, ibody;
    bool stroke_no_tag7 = false, image_no_tag5 = false;
    if (find_block_payload(bytes, size, RM_BLOCK_SCENE_LINE_ITEM, &lbody)) {
      rm_item_preamble pre;
      rm_reader val;
      if (rm_parse_item_preamble(&lbody, &pre) == RM_OK &&
          rm_expect_tag(&lbody, 6, RM_TAG_LENGTH4) == RM_OK &&
          rm_read_subblock(&lbody, &val) == RM_OK) {
        bool saw_tag7 = false;
        uint8_t kind = 0;
        (void)rm_read_u8(&val, &kind);
        while (rm_reader_remaining(&val) > 0) {
          uint32_t t = 0;
          uint8_t tp2 = 0;
          if (rm_read_tag(&val, &t, &tp2) != RM_OK) break;
          if (t == 7) saw_tag7 = true;
          if (rmi_skip_tagged_value(&val, tp2, nullptr) != RM_OK) break;
        }
        stroke_no_tag7 = !saw_tag7;
      }
    }
    if (find_block_payload(bytes, size, RM_BLOCK_IMAGE_ITEM, &ibody)) {
      rm_item_preamble pre;
      if (rm_parse_item_preamble(&ibody, &pre) == RM_OK &&
          rm_expect_tag(&ibody, 6, RM_TAG_LENGTH4) == RM_OK) {
        rm_reader val;
        if (rm_read_subblock(&ibody, &val) == RM_OK) {
          bool saw_tag5 = false;
          uint8_t kind = 0;
          (void)rm_read_u8(&val, &kind);
          while (rm_reader_remaining(&val) > 0) {
            uint32_t t = 0;
            uint8_t tp2 = 0;
            if (rm_read_tag(&val, &t, &tp2) != RM_OK) break;
            if (t == 5) saw_tag5 = true;
            if (rmi_skip_tagged_value(&val, tp2, nullptr) != RM_OK) break;
          }
          image_no_tag5 = !saw_tag5;
        }
      }
    }
    RM_CHECK(stroke_no_tag7 && image_no_tag5,
             "fresh color-9 stroke omits tag 7 and fresh image omits tag 5");
    rm_bytes_free(bytes);
  }

  /* 5) Tool-aware palette + ARGB + thickness matrix (R104, R111, R134, R168,
   * R172). */
  {
    rm_draft_point pt = rm_draft_point_make(0, 0);
    rm_draft_stroke hl_def =
        rm_draft_stroke_make(RM_TOOL_HIGHLIGHTER_2, RM_COLOR_YELLOW, &pt, 1);
    RM_CHECK(hl_def.thickness == 1.0,
             "rm_draft_stroke_make highlighter defaults to thickness 1.0");
    const struct {
      rm_pen_tool tool;
      rm_pen_color color;
      bool has_argb;
      double thickness;
      rm_status expect;
    } cases[] = {
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHTER_BLUE, false, 1.0, RM_OK},
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHTER_BLUE, true, 1.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHT, true, 1.0, RM_OK},
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_HIGHLIGHT, false, 1.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_YELLOW, false, 2.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_HIGHLIGHTER_2, RM_COLOR_YELLOW, false, 3.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_SHADER, RM_COLOR_BLACK, false, 2.0, RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_SHADER, RM_COLOR_HIGHLIGHT, true, 2.0, RM_OK},
        {RM_TOOL_FINELINER_2, RM_COLOR_HIGHLIGHT, true, 2.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_FINELINER_2, RM_COLOR_HIGHLIGHTER_BLUE, false, 2.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_FINELINER_2, RM_COLOR_BLACK, false, 1.5,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_FINELINER_2, RM_COLOR_BLACK, false, 0.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_FINELINER_2, RM_COLOR_BLACK, false, -1.0,
         RM_ERR_INVALID_ARGUMENT},
        {RM_TOOL_FINELINER_2, RM_COLOR_BLACK, false, 1.0, RM_OK},
        {RM_TOOL_FINELINER_2, RM_COLOR_BLACK, false, 3.0, RM_OK},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
      rm_draft_stroke st = {.tool = cases[i].tool,
                            .color = cases[i].color,
                            .has_argb = cases[i].has_argb,
                            .argb = cases[i].has_argb ? 0x80112233u : 0u,
                            .thickness = cases[i].thickness,
                            .points = &pt,
                            .point_count = 1};
      for (int anchored = 0; anchored < 2; ++anchored) {
        rm_draft_mark mk = {.anchor = {.kind = RM_ANCHOR_BEFORE_TEXT},
                            .stroke = st};
        rm_page_draft d = {.author = {1},
                           .sheet = {1404, 1872, 1404, 1872},
                           .column = {936, 1}};
        if (anchored) {
          d.marks = &mk;
          d.mark_count = 1;
        } else {
          d.strokes = &st;
          d.stroke_count = 1;
        }
        uint8_t* bytes = nullptr;
        size_t sz = 0;
        RM_CHECK(rm_write_page(&d, &bytes, &sz, nullptr) == cases[i].expect,
                 "palette/thickness case %zu anchored=%d", i, anchored);
        rm_bytes_free(bytes);
      }
    }
  }

  /* 6) Sheet dimensions, equivalent repaired UTF-8 image keys, far-corner
   * double overflow, image pen_origin (R52, R234, R238). */
  {
    const struct {
      double cw, ch;
    } negs[2] = {{-1.0, 1872.0}, {1404.0, -1.0}};
    for (int i = 0; i < 2; ++i) {
      rm_page_draft d = {.author = {1},
                         .sheet = {1404, 1872, negs[i].cw, negs[i].ch},
                         .column = {936, 1}};
      uint8_t* out_s = (uint8_t*)0x1;
      size_t sz_s = 55;
      rm_error err = {0};
      RM_CHECK(
          rm_write_page(&d, &out_s, &sz_s, &err) == RM_ERR_INVALID_ARGUMENT &&
              strcmp(err.message, "sheet") == 0 && out_s == (uint8_t*)0x1 &&
              sz_s == 55,
          "negative sheet canvas %d refused cleanly", i);
    }

    const struct {
      double cw, ch;
    } zeros[3] = {{0.0, 1872.0}, {1404.0, 0.0}, {0.0, 0.0}};
    for (int i = 0; i < 3; ++i) {
      rm_page_draft d = {.author = {1},
                         .sheet = {1404, 1872, zeros[i].cw, zeros[i].ch},
                         .column = {936, 1}};
      uint8_t* bytes = nullptr;
      size_t sz = 0;
      RM_CHECK(rm_write_page(&d, &bytes, &sz, nullptr) == RM_OK,
               "zero canvas[%d] writes", i);
      rm_page* p = nullptr;
      RM_CHECK(rm_parse_page(bytes, sz, &p, nullptr) == RM_OK && p != nullptr &&
                   p->has_paper_size && p->paper_size.width == 1404.0 &&
                   p->paper_size.height == 1872.0,
               "zero canvas[%d] falls back to (1404, 1872)", i);
      rm_page_free(p);
      rm_bytes_free(bytes);
    }

    rm_page_draft dm = {.author = {1},
                        .sheet = {1600, 0, 0.0, 1872.0},
                        .column = {.width = 0.0f, .setting = 4}};
    uint8_t* mbytes = nullptr;
    size_t msz = 0;
    RM_CHECK(rm_write_page(&dm, &mbytes, &msz, nullptr) == RM_OK,
             "mixed zero screen/canvas writes");
    rm_page* mp = nullptr;
    RM_CHECK(rm_parse_page(mbytes, msz, &mp, nullptr) == RM_OK &&
                 mp != nullptr && !mp->has_paper_size,
             "mixed zero canvas+screen yields has_paper_size == false");
    rm_page_free(mp);
    rm_edit_session* ms = nullptr;
    RM_CHECK(rm_edit_open(mbytes, msz, &ms, nullptr) == RM_OK &&
                 rm_edit_replace_text(ms, 0, 0, "A", (rm_item_id){1, 50},
                                      nullptr) == RM_OK,
             "Auto editor edit with screen_height == 0");
    size_t emsz = 0;
    const uint8_t* embytes = rm_edit_bytes(ms, &emsz);
    mp = nullptr;
    RM_CHECK(rm_parse_page(embytes, emsz, &mp, nullptr) == RM_OK &&
                 mp != nullptr && mp->has_text &&
                 mp->text.column_width == 936.0 && mp->text.origin_x == -468.0,
             "Auto with screen_height == 0 resolves Medium 936/-468");
    rm_page_free(mp);
    rm_edit_close(ms);
    rm_bytes_free(mbytes);

    /* Equivalent repaired UTF-8 duplicate image keys succeed (R52). */
    rm_draft_image eq_imgs[2] = {
        {.key = {2}, .file = "a\x80.png", .width = 10, .height = 10},
        {.key = {2}, .file = "a\xEF\xBF\xBD.png", .width = 10, .height = 10},
    };
    rm_page_draft deq = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .images = eq_imgs,
                         .image_count = 2};
    RM_CHECK(rm_write_page(&deq, &mbytes, &msz, nullptr) == RM_OK,
             "equivalent repaired UTF-8 image filenames succeed");
    rm_bytes_free(mbytes);

    rm_draft_image overflow_img = {.key = {3},
                                   .file = "huge.png",
                                   .x = FLT_MAX,
                                   .width = FLT_MAX,
                                   .height = 10};
    rm_page_draft dov = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = {936, 1},
                         .images = &overflow_img,
                         .image_count = 1};
    RM_CHECK(
        rm_write_page(&dov, &mbytes, &msz, nullptr) == RM_ERR_INVALID_ARGUMENT,
        "FLT_MAX + FLT_MAX image far-corner overflow fails");

    rm_draft_image unanchored_nan = {.key = {4},
                                     .file = "ok.png",
                                     .width = 10,
                                     .height = 10,
                                     .has_anchor = false,
                                     .pen_origin = NAN};
    dov.images = &unanchored_nan;
    RM_CHECK(rm_write_page(&dov, &mbytes, &msz, nullptr) == RM_OK,
             "unanchored image ignores NAN pen_origin");
    rm_bytes_free(mbytes);
    unanchored_nan.has_anchor = true;
    RM_CHECK(
        rm_write_page(&dov, &mbytes, &msz, nullptr) == RM_ERR_INVALID_ARGUMENT,
        "anchored image rejects NAN pen_origin");
  }

  /* 7) Lowered block-limit and wire_fits boundary checks (R67, R71, R108,
   * R116, R120, R128). */
  {
    char big_text[450];
    memset(big_text, 'x', sizeof big_text - 1u);
    big_text[sizeof big_text - 1u] = '\0';
    rm_draft_paragraph p_short = {.text = "ok"};
    rm_draft_paragraph p_long = {.text = big_text};
    rm_page_draft dshort = {.author = {1},
                            .sheet = {1404, 1872, 1404, 1872},
                            .column = {936, 1},
                            .paragraphs = &p_short,
                            .paragraph_count = 1};
    uint8_t* bytes = nullptr;
    size_t sz = 0;
    rm_test_block_limit(512);
    RM_CHECK(rm_write_page(&dshort, &bytes, &sz, nullptr) == RM_OK,
             "short text draft fits within 512-byte limit");
    rm_bytes_free(bytes);
    dshort.paragraphs = &p_long;
    RM_CHECK(rm_write_page(&dshort, &bytes, &sz, nullptr) ==
                 RM_ERR_TOO_MANY_TO_NUMBER,
             "nested text_block crossing 512 limit returns TOO_MANY_TO_NUMBER");

    RM_CHECK(rmi_error_size(UINT32_MAX) ==
                 (UINT32_MAX > LONG_MAX ? LONG_MAX : (long)UINT32_MAX),
             "the full wire limit fits the diagnostic on this platform");
    RM_CHECK(rmi_error_size(SIZE_MAX) ==
                 (SIZE_MAX > LONG_MAX ? LONG_MAX : (long)SIZE_MAX),
             "a size beyond LONG_MAX saturates before conversion");
    rm_test_block_limit(256);
    rm_error block_error = {0};
    RM_CHECK(rm_write_page(&dshort, &bytes, &sz, &block_error) ==
                     RM_ERR_TOO_MANY_TO_NUMBER &&
                 block_error.value == 256,
             "an oversized nested text block reports its wire limit");

    /* SceneInfo-alone limit check (RV46-11). */
    rm_page_draft empty = {
        .author = {1}, .sheet = {1404, 1872, 1404, 1872}, .column = {936, 1}};
    rm_test_block_limit(UINT32_MAX);
    RM_CHECK(rm_write_page(&empty, &bytes, &sz, nullptr) == RM_OK,
             "empty draft serializes under full limit");
    size_t scene_info_len = 0, max_other_len = 0;
    rm_reader sr = {.data = bytes, .size = sz, .offset = RM_HEADER_LENGTH};
    rm_block_header sb;
    while (rm_reader_remaining(&sr) >= 8 &&
           rm_read_block_header(&sr, &sb) == RM_OK) {
      size_t body_len = sb.end - sb.body;
      if (sb.type == RM_BLOCK_SCENE_INFO)
        scene_info_len = body_len;
      else if (body_len > max_other_len)
        max_other_len = body_len;
      sr.offset = sb.end;
    }
    rm_bytes_free(bytes);
    RM_CHECK(scene_info_len > max_other_len,
             "SceneInfo body (%zu) strictly exceeds all other empty blocks "
             "(max %zu)",
             scene_info_len, max_other_len);
    rm_test_block_limit(scene_info_len);
    RM_CHECK(rm_write_page(&empty, &bytes, &sz, nullptr) == RM_OK,
             "SceneInfo succeeds at exact body length %zu", scene_info_len);
    rm_bytes_free(bytes);
    rm_test_block_limit(scene_info_len - 1u);
    RM_CHECK(rm_write_page(&empty, &bytes, &sz, nullptr) ==
                 RM_ERR_TOO_MANY_TO_NUMBER,
             "SceneInfo exceeding lowered block limit returns TOO_MANY");
    rm_test_block_limit(UINT32_MAX);

    /* Direct wire_fits + >50-point color-9 NAN precedence at L and L - 1. */
    rm_draft_point pts[60];
    for (size_t i = 0; i < 60; ++i) pts[i] = rm_draft_point_make((float)i, 0);
    rm_draft_stroke hl9 = {.tool = RM_TOOL_HIGHLIGHTER_2,
                           .color = RM_COLOR_HIGHLIGHT,
                           .has_argb = true,
                           .argb = 0x55AA00AAu,
                           .thickness = 1.0,
                           .points = pts,
                           .point_count = 60};
    rm_buf lb = {0};
    rm_wid parent = rm_crdt_make(1, 11), sid = rm_crdt_make(1, 200000),
           none = RM_CRDT_NONE;
    rm_write_line_item(&lb, &hl9, parent, sid, none, none);
    size_t L = lb.count;
    rm_buf_free(&lb);

    rm_test_block_limit(L);
    RM_CHECK(rmi_line_item_wire_fits(&hl9, parent, sid, none, none),
             "rmi_line_item_wire_fits accepts exact body length L=%zu", L);
    rm_test_block_limit(L - 1u);
    RM_CHECK(!rmi_line_item_wire_fits(&hl9, parent, sid, none, none),
             "rmi_line_item_wire_fits rejects L - 1");

    rm_page_draft wd = {.author = {1},
                        .sheet = {1404, 1872, 1404, 1872},
                        .column = {936, 1},
                        .strokes = &hl9,
                        .stroke_count = 1};
    RM_CHECK(
        rm_write_page(&wd, &bytes, &sz, nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
        "valid 60-point color-9 stroke exceeds L - 1");
    pts[0].x = NAN;
    RM_CHECK(
        rm_write_page(&wd, &bytes, &sz, nullptr) == RM_ERR_TOO_MANY_TO_NUMBER,
        "wire limit precedes NaN coordinate validation at L - 1");
    pts[0].x = 0.0f;
    rm_test_block_limit(L);
    RM_CHECK(rm_write_page(&wd, &bytes, &sz, nullptr) == RM_OK,
             "60-point color-9 stroke succeeds at exact block limit L=%zu", L);
    rm_bytes_free(bytes);
    rm_test_block_limit(UINT32_MAX);

    /* RV43-R2-12: out-of-range anchor is refused in Step 5 before Step 6
     * text_block limit. */
    rm_draft_mark bad_mark = {
        .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 5},
        .pen_origin = -468.0f,
        .stroke =
            rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, pts, 1)};
    rm_page_draft d_bad_anchor = {.author = {1},
                                  .sheet = {1404, 1872, 1404, 1872},
                                  .column = {936, 1},
                                  .paragraphs = &p_long,
                                  .paragraph_count = 1,
                                  .marks = &bad_mark,
                                  .mark_count = 1};
    rm_test_block_limit(512);
    rm_error aerr = {0};
    RM_CHECK(rm_write_page(&d_bad_anchor, &bytes, &sz, &aerr) ==
                     RM_ERR_ANCHOR_OUT_OF_RANGE &&
                 aerr.status == RM_ERR_ANCHOR_OUT_OF_RANGE,
             "out-of-range mark anchor precedes oversized text_block limit "
             "(RV43-R2-12)");
    rm_test_block_limit(UINT32_MAX);
  }
}

static void template_repairs_ill_formed_utf8(void) {
  rm_page_draft draft = {.author = {1},
                         .sheet = {1404, 1872, 1404, 1872},
                         .column = rm_draft_column_of(RM_COLUMN_MEDIUM)};
  uint8_t* data = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &data, &size, nullptr) == RM_OK, "write");
  rm_page* page = nullptr;
  RM_CHECK(rm_parse_page(data, size, &page, nullptr) == RM_OK, "parse");
  rm_bytes_free(data);
  RM_CHECK(rm_page_set_template(page, "bad\x80tmpl") == RM_OK,
           "set_template succeeds with ill-formed UTF-8");
  RM_CHECK_EQ_STR(page->template_name.bytes, page->template_name.length,
                  "bad\xEF\xBF\xBDtmpl",
                  "set_template repairs ill-formed UTF-8");
  rm_page_free(page);
}

static void anchored_mark_on_empty_last_paragraph_survives_round_trip(void) {
  rm_draft_paragraph paras[] = {{.text = "Hello"}, {.text = ""}};
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_mark mark = {
      .anchor = {.kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1},
      .pen_origin = -468.0f,
      .stroke =
          rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1)};
  rm_page_draft draft = {.sheet = {1404, 1872, 1404, 1872},
                         .column = rm_draft_column_of(RM_COLUMN_MEDIUM),
                         .paragraphs = paras,
                         .paragraph_count = 2,
                         .marks = &mark,
                         .mark_count = 1};
  memcpy(draft.author, AUTHOR, 16);
  rm_page* page = round_trip(&draft);
  RM_CHECK(page != nullptr && page->stroke_count == 1, "mark round-trips");
  if (page && page->stroke_count == 1) {
    RM_CHECK(page->strokes[0].has_anchor &&
                 page->strokes[0].anchor.paragraph == 1 &&
                 page->strokes[0].anchor.offset == 0,
             "anchor on empty last paragraph resolves to paragraph 1 offset 0 "
             "(has=%d para=%d off=%d)",
             page->strokes[0].has_anchor, page->strokes[0].anchor.paragraph,
             page->strokes[0].anchor.offset);
  }
  rm_page_free(page);
}

int main(void) {
  missing_point_arrays_are_refused();
  a_written_page_round_trips();
  a_loose_stroke_stays_loose_and_a_mark_stays_anchored();
  images_survive_the_round_trip();
  several_images_keep_their_own_names();
  a_count_without_its_items_is_refused();
  a_nan_origin_is_left_unset();
  an_image_only_page_has_no_text();
  an_anchored_image_comes_back_fastened();
  text_outside_ascii_round_trips();
  the_sentinels_always_resolve();
  the_end_sentinel_lands_on_the_last_typed_line();
  the_end_sentinel_on_empty_paragraphs_goes_to_the_bottom();
  the_end_sentinel_on_no_text_stays_at_the_start();
  ill_formed_text_is_written_as_replacement_characters();
  a_stroke_made_with_defaults_has_the_default_ones();
  an_anchor_past_the_text_is_refused();
  counters_that_would_collide_are_refused();
  the_writer_is_deterministic();
  an_explicit_marks_author_is_used();
  a_template_can_be_stamped_on_a_parsed_page();
  invalid_writer_inputs_are_refused();
  template_repairs_ill_formed_utf8();
  anchored_mark_on_empty_last_paragraph_survives_round_trip();
  fresh_stroke_tail_matches_its_colour();
  a4_writer_columns_authors_limits_and_palette_contracts();
  return rm_test_report("writer");
}
