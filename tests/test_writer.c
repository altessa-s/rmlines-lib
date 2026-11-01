/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The writer against the parser: a draft written and read back must answer
 * with the same text, styles, strokes and anchors. This is one half of the
 * writer's proof; the other half is the device, because only the tablet can
 * say a page is acceptable. */

#include <stdlib.h>

#include "rm_test.h"
#include "rmlines/rmlines.h"

static const uint8_t AUTHOR[16] = { 0x0a, 0x41, 0x8e, 0x1a, 0x11, 0x11, 0x42, 0x22,
                                    0x83, 0x33, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66 };
static const uint8_t OTHER[16] = { 0x0a, 0x41, 0x8e, 0x1a, 0x11, 0x11, 0x42, 0x22,
                                   0x83, 0x33, 0x44, 0x44, 0x55, 0x55, 0x66, 0x77 };
static const uint8_t NAMED[16] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0x4d, 0xef,
                                   0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde };

static rm_draft_paragraph base_paragraphs[3];
static rm_draft_point loose_points[3];
static rm_draft_point mark_points[2];
static rm_draft_stroke base_strokes[1];
static rm_draft_mark base_marks[1];
static uint8_t key[16];

static rm_page_draft sample_draft(void) {
    base_paragraphs[0] = (rm_draft_paragraph){ .text = "Heading", .has_style = true, .style = 2 };
    base_paragraphs[1] = (rm_draft_paragraph){ .text = "Plain body" };
    base_paragraphs[2] = (rm_draft_paragraph){ .text = "Bold words", .bold = true };
    loose_points[0] = rm_draft_point_make(0, 100);
    loose_points[1] = rm_draft_point_make(50, 100);
    loose_points[2] = rm_draft_point_make(100, 100);
    base_strokes[0] = (rm_draft_stroke){ .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                                         .thickness = 2, .points = loose_points,
                                         .point_count = 3 };
    mark_points[0] = rm_draft_point_make(0, -10);
    mark_points[1] = rm_draft_point_make(40, -10);
    base_marks[0] = (rm_draft_mark){
        .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1 },
        .pen_origin = -464,
        .stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_RED, .thickness = 2,
                    .points = mark_points, .point_count = 2 },
    };
    for (size_t i = 0; i < 16; i += 1) key[i] = (uint8_t)i;

    rm_page_draft draft = {
        .sheet = { 1404, 1872, 1404, 1872 },
        .column = { 936, 1 },
        .paragraphs = base_paragraphs, .paragraph_count = 3,
        .strokes = base_strokes, .stroke_count = 1,
        .marks = base_marks, .mark_count = 1,
    };
    memcpy(draft.author, AUTHOR, 16);
    return draft;
}

/* Write a draft and read it straight back. Null when either half refused. */
static rm_page *round_trip(const rm_page_draft *draft) {
    uint8_t *bytes = nullptr;
    size_t size = 0;
    rm_error error;
    if (rm_write_page(draft, &bytes, &size, &error) != RM_OK) return nullptr;
    rm_page *page = nullptr;
    rm_status status = rm_parse_page(bytes, size, &page, &error);
    rm_bytes_free(bytes);
    return status == RM_OK ? page : nullptr;
}

static void a_written_page_round_trips(void) {
    rm_page_draft draft = sample_draft();
    rm_page *page = round_trip(&draft);
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
    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "the page round-trips");
    if (page == nullptr || page->stroke_count != 2) return;

    const rm_stroke *loose = &page->strokes[0];
    RM_CHECK(!loose->has_anchor, "the loose stroke hangs on nothing");
    RM_CHECK(loose->tool == RM_TOOL_FINELINER_2, "its tool survived");
    RM_CHECK(loose->color == RM_COLOR_BLACK, "and its colour");
    RM_CHECK_EQ_SIZE(loose->point_count, 3, "and its points");

    const rm_stroke *marked = &page->strokes[1];
    RM_CHECK(marked->has_anchor, "the mark is fastened");
    RM_CHECK(marked->anchor.paragraph == 1, "to its paragraph (%d)", marked->anchor.paragraph);
    RM_CHECK(marked->anchor.has_origin_x && marked->anchor.origin_x == -464,
             "and it carries the pen's horizontal");
    RM_CHECK(marked->color == RM_COLOR_RED, "its colour survived");
    RM_CHECK_EQ_SIZE(marked->point_count, 2, "and its points");
    rm_page_free(page);
}

static void images_survive_the_round_trip(void) {
    rm_page_draft draft = sample_draft();
    rm_draft_image image = { .file = "0a418e1a-1111-4222-8333-444455556666.png",
                             .x = -50, .y = 100, .width = 120, .height = 80 };
    memcpy(image.key, key, 16);
    draft.images = &image;
    draft.image_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->image_count == 1, "one image comes back");
    if (page == nullptr || page->image_count != 1) { rm_page_free(page); return; }
    RM_CHECK_EQ_STR(page->images[0].file.bytes, page->images[0].file.length,
                    "0a418e1a-1111-4222-8333-444455556666.png", "with its file name");
    RM_CHECK(page->images[0].vertices[0].x == -50 && page->images[0].vertices[0].y == 100,
             "and its first corner");
    /* Both strokes stand before the image: it joined the chain last. */
    RM_CHECK_EQ_SIZE(page->images[0].strokes_before, 2, "and its place in the drawing order");
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
    rm_draft_image image = { .file = "0a418e1a-1111-4222-8333-444455556666.png",
                             .x = -60, .y = 200, .width = 120, .height = 160 };
    memcpy(image.key, key, 16);
    draft.images = &image;
    draft.image_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "an image-only page round-trips");
    if (page == nullptr) return;
    RM_CHECK(!page->has_text, "and carries no text block");
    RM_CHECK_EQ_SIZE(page->image_count, 1, "one image");
    RM_CHECK_EQ_SIZE(page->stroke_count, 0, "and no strokes");
    rm_page_free(page);
}

static void an_anchored_image_comes_back_fastened(void) {
    rm_page_draft draft = sample_draft();
    rm_draft_image image = { .file = "0a418e1a-1111-4222-8333-444455556666.png",
                             .x = 40, .y = 30, .width = 120, .height = 80,
                             .has_anchor = true,
                             .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1 },
                             .pen_origin = -464 };
    memcpy(image.key, key, 16);
    draft.images = &image;
    draft.image_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->image_count == 1, "the anchored image comes back");
    if (page == nullptr || page->image_count != 1) { rm_page_free(page); return; }
    RM_CHECK(page->images[0].has_anchor, "fastened");
    RM_CHECK(page->images[0].anchor.paragraph == 1, "to its paragraph");
    RM_CHECK(page->images[0].anchor.has_origin_x && page->images[0].anchor.origin_x == -464,
             "with the pen's horizontal");
    /* The vertices stay the pen's: where they land is the layout's to say, and
     * the parser does not lay text out. */
    RM_CHECK(page->images[0].vertices[0].x == 40 && page->images[0].vertices[0].y == 30,
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
        { .text = "Grüße, Welt", .has_style = true, .style = 2 },
        { .text = "Ελληνικά and English" },
        { .text = "絵文字 🖋 emoji" },
    };
    rm_draft_point point = rm_draft_point_make(0, -10);
    rm_draft_mark mark = {
        .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 2 },
        .pen_origin = -464,
        .stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_RED, .thickness = 2,
                    .points = &point, .point_count = 1 },
    };
    draft.paragraphs = paragraphs;
    draft.paragraph_count = 3;
    draft.marks = &mark;
    draft.mark_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "the page round-trips");
    if (page == nullptr) return;
    RM_CHECK_EQ_SIZE(page->text.paragraph_count, 3, "three paragraphs");
    if (page->text.paragraph_count == 3) {
        RM_CHECK_EQ_STR(page->text.paragraphs[0].text.bytes, page->text.paragraphs[0].text.length,
                        "Grüße, Welt", "Latin with diacritics");
        RM_CHECK_EQ_STR(page->text.paragraphs[1].text.bytes, page->text.paragraphs[1].text.length,
                        "Ελληνικά and English", "Greek");
        RM_CHECK_EQ_STR(page->text.paragraphs[2].text.bytes, page->text.paragraphs[2].text.length,
                        "絵文字 🖋 emoji", "CJK and an emoji");
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
    rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                               .thickness = 2, .points = &point, .point_count = 1 };
    rm_draft_mark marks[2] = {
        { .anchor = { .kind = RM_ANCHOR_BEFORE_TEXT }, .pen_origin = 0, .stroke = stroke },
        { .anchor = { .kind = RM_ANCHOR_AFTER_TEXT }, .pen_origin = 0, .stroke = stroke },
    };
    draft.marks = marks;
    draft.mark_count = 2;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "a page with no paragraphs still takes anchored ink");
    if (page == nullptr) return;
    RM_CHECK_EQ_SIZE(page->stroke_count, 3, "one loose stroke and the two marks");
    rm_page_free(page);
}

/* "After the text" is just past the last character typed. Taking the sentinel
 * for an unknown character instead puts every stroke anchored to it on line
 * zero — on top of the drawing that sits above the text. */
static void the_end_sentinel_lands_on_the_last_typed_line(void) {
    rm_draft_point point = rm_draft_point_make(0, 0);
    rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                               .thickness = 2, .points = &point, .point_count = 1 };
    rm_draft_mark marks[2] = {
        { .anchor = { .kind = RM_ANCHOR_BEFORE_TEXT }, .pen_origin = 0, .stroke = stroke },
        { .anchor = { .kind = RM_ANCHOR_AFTER_TEXT }, .pen_origin = 0, .stroke = stroke },
    };

    rm_page_draft draft = sample_draft();
    rm_draft_paragraph four[] = { { .text = "one" }, { .text = "two" }, { .text = "three" },
                                  { .text = "four" } };
    draft.paragraphs = four;
    draft.paragraph_count = 4;
    draft.strokes = nullptr;
    draft.stroke_count = 0;
    draft.marks = marks;
    draft.mark_count = 2;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->stroke_count == 2, "both marks come back");
    if (page != nullptr && page->stroke_count == 2) {
        RM_CHECK(page->strokes[0].anchor.paragraph == 0 && page->strokes[0].anchor.offset == 0,
                 "before the text is the very start");
        /* One past the last letter: an anchor names the character the pen
         * stands before, and "after everything typed" is before the character
         * that would come next. */
        RM_CHECK(page->strokes[1].anchor.paragraph == 3 && page->strokes[1].anchor.offset == 5,
                 "after the text is one past the end of the last line (got %d/%d)",
                 page->strokes[1].anchor.paragraph, page->strokes[1].anchor.offset);
    }
    rm_page_free(page);

    /* Text ending with newlines leaves empty paragraphs behind it. "After the
     * text" is just past the last character, and the last Return ends the
     * paragraph it closes — the second one here, not the empty third. */
    rm_draft_paragraph trailing[] = { { .text = "Test test" }, { .text = "" }, { .text = "" } };
    draft.paragraphs = trailing;
    draft.paragraph_count = 3;
    draft.marks = &marks[1];
    draft.mark_count = 1;

    page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
    if (page != nullptr && page->stroke_count == 1) {
        RM_CHECK(page->strokes[0].anchor.paragraph == 1 && page->strokes[0].anchor.offset == 1,
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
    rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                               .thickness = 2, .points = &point, .point_count = 1 };
    rm_draft_mark mark = { .anchor = { .kind = RM_ANCHOR_AFTER_TEXT }, .pen_origin = 0,
                           .stroke = stroke };
    rm_draft_paragraph empty[20];
    for (size_t i = 0; i < 20; i += 1) empty[i] = (rm_draft_paragraph){ .text = "" };

    rm_page_draft draft = sample_draft();
    draft.paragraphs = empty;
    draft.paragraph_count = 20;
    draft.strokes = nullptr;
    draft.stroke_count = 0;
    draft.marks = &mark;
    draft.mark_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
    if (page != nullptr && page->stroke_count == 1) {
        RM_CHECK(page->strokes[0].anchor.paragraph == 18 && page->strokes[0].anchor.offset == 1,
                 "under the last Return (got %d/%d)",
                 page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
    }
    rm_page_free(page);
}

/* Both shapes of "nothing typed" — no paragraphs, and the single empty one —
 * end where they start. */
static void the_end_sentinel_on_no_text_stays_at_the_start(void) {
    rm_draft_point point = rm_draft_point_make(0, 0);
    rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                               .thickness = 2, .points = &point, .point_count = 1 };
    rm_draft_mark mark = { .anchor = { .kind = RM_ANCHOR_AFTER_TEXT }, .pen_origin = 0,
                           .stroke = stroke };
    rm_draft_paragraph one_empty[] = { { .text = "" } };

    for (size_t paragraphs = 0; paragraphs < 2; paragraphs += 1) {
        rm_page_draft draft = sample_draft();
        draft.paragraphs = paragraphs == 0 ? nullptr : one_empty;
        draft.paragraph_count = paragraphs;
        draft.strokes = nullptr;
        draft.stroke_count = 0;
        draft.marks = &mark;
        draft.mark_count = 1;

        rm_page *page = round_trip(&draft);
        RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back (%zu)", paragraphs);
        if (page != nullptr && page->stroke_count == 1) {
            RM_CHECK(page->strokes[0].anchor.paragraph == 0 && page->strokes[0].anchor.offset == 0,
                     "at the start with %zu paragraphs (got %d/%d)", paragraphs,
                     page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
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
        .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 99 },
        .pen_origin = 0,
        .stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK, .thickness = 2,
                    .points = &point, .point_count = 1 },
    };
    draft.marks = &mark;
    draft.mark_count = 1;

    uint8_t *bytes = nullptr;
    size_t size = 0;
    rm_error error;
    rm_status status = rm_write_page(&draft, &bytes, &size, &error);
    RM_CHECK(status == RM_ERR_ANCHOR_OUT_OF_RANGE, "a mark on a paragraph that is not there");
    RM_CHECK(error.value == 99, "and the paragraph is named (%ld)", error.value);
    RM_CHECK(bytes == nullptr, "and nothing at all was written");

    rm_page_draft with_image = sample_draft();
    rm_draft_image image = { .file = "a.png", .x = 0, .y = 0, .width = 10, .height = 10,
                             .has_anchor = true,
                             .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 42 } };
    memcpy(image.key, key, 16);
    with_image.images = &image;
    with_image.image_count = 1;
    status = rm_write_page(&with_image, &bytes, &size, &error);
    RM_CHECK(status == RM_ERR_ANCHOR_OUT_OF_RANGE, "and an image likewise");
    RM_CHECK(error.value == 42, "with its own paragraph named (%ld)", error.value);
}

/* Past these the identifier ranges run into one another and two items are
 * handed one counter — and the file cannot tell two meanings of one counter
 * apart. */
static void counters_that_would_collide_are_refused(void) {
    rm_draft_point point = rm_draft_point_make(0, 0);
    rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .color = RM_COLOR_BLACK,
                               .thickness = 2, .points = &point, .point_count = 1 };
    uint8_t *bytes = nullptr;
    size_t size = 0;
    rm_error error;

    constexpr size_t mark_limit = 12500;
    rm_draft_mark *marks = calloc(mark_limit + 1u, sizeof *marks);
    for (size_t i = 0; i <= mark_limit; i += 1) {
        marks[i] = (rm_draft_mark){ .anchor = { .kind = RM_ANCHOR_BEFORE_TEXT }, .stroke = stroke };
    }
    rm_page_draft too_many_marks = sample_draft();
    too_many_marks.marks = marks;
    too_many_marks.mark_count = mark_limit + 1u;
    RM_CHECK(rm_write_page(&too_many_marks, &bytes, &size, &error) == RM_ERR_TOO_MANY_TO_NUMBER,
             "too many marks");
    RM_CHECK(error.value == (long)mark_limit, "the limit travels (%ld)", error.value);
    RM_CHECK(strcmp(error.message, "marks") == 0, "and so does what there are too many of");
    free(marks);

    constexpr size_t stroke_limit = 100000;
    rm_draft_stroke *strokes = calloc(stroke_limit + 1u, sizeof *strokes);
    for (size_t i = 0; i <= stroke_limit; i += 1) strokes[i] = stroke;
    rm_page_draft too_many_strokes = sample_draft();
    too_many_strokes.strokes = strokes;
    too_many_strokes.stroke_count = stroke_limit + 1u;
    RM_CHECK(rm_write_page(&too_many_strokes, &bytes, &size, &error) == RM_ERR_TOO_MANY_TO_NUMBER,
             "too many strokes");
    RM_CHECK(error.value == (long)stroke_limit, "the limit travels (%ld)", error.value);
    free(strokes);

    constexpr size_t character_limit = 99980;
    char *long_text = malloc(character_limit + 1u);
    memset(long_text, 'a', character_limit);
    long_text[character_limit] = '\0';
    rm_draft_paragraph one = { .text = long_text };
    rm_page_draft too_much_text = sample_draft();
    too_much_text.paragraphs = &one;
    too_much_text.paragraph_count = 1;
    too_much_text.marks = nullptr;
    too_much_text.mark_count = 0;
    RM_CHECK(rm_write_page(&too_much_text, &bytes, &size, &error) == RM_ERR_TOO_MANY_TO_NUMBER,
             "too many characters");
    RM_CHECK(error.value == (long)character_limit, "the limit travels (%ld)", error.value);

    /* Over two limits at once, the characters are named first — the order the
     * writer checks in. */
    marks = calloc(mark_limit + 1u, sizeof *marks);
    for (size_t i = 0; i <= mark_limit; i += 1) {
        marks[i] = (rm_draft_mark){ .anchor = { .kind = RM_ANCHOR_BEFORE_TEXT }, .stroke = stroke };
    }
    too_much_text.marks = marks;
    too_much_text.mark_count = mark_limit + 1u;
    RM_CHECK(rm_write_page(&too_much_text, &bytes, &size, &error) == RM_ERR_TOO_MANY_TO_NUMBER,
             "too many of both");
    RM_CHECK(strcmp(error.message, "characters") == 0, "the characters are named first (%s)",
             error.message);
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
        .anchor = { .kind = RM_ANCHOR_FIRST_CHARACTER, .paragraph = 1 },
        .stroke = rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &point, 1),
    };
    rm_draft_paragraph paragraphs[] = { { .text = "a\x80" "b" }, { .text = "cd" } };
    rm_page_draft draft = sample_draft();
    draft.paragraphs = paragraphs;
    draft.paragraph_count = 2;
    draft.strokes = nullptr;
    draft.stroke_count = 0;
    draft.marks = &mark;
    draft.mark_count = 1;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->has_text && page->text.paragraph_count == 2,
             "the text comes back");
    if (page != nullptr && page->has_text && page->text.paragraph_count == 2) {
        RM_CHECK_EQ_STR(page->text.paragraphs[0].text.bytes, page->text.paragraphs[0].text.length,
                        "a\xEF\xBF\xBD" "b", "the stray byte one U+FFFD");
    }
    RM_CHECK(page != nullptr && page->stroke_count == 1, "the mark comes back");
    if (page != nullptr && page->stroke_count == 1) {
        RM_CHECK(page->strokes[0].anchor.paragraph == 1 && page->strokes[0].anchor.offset == 1,
                 "on the first letter of the second paragraph (got %d/%d)",
                 page->strokes[0].anchor.paragraph, page->strokes[0].anchor.offset);
    }
    rm_page_free(page);
}

/* The stroke a draft makes when only the tool, the colour and the points are
 * named: no argb, thickness 2. */
static void a_stroke_made_with_defaults_has_the_default_ones(void) {
    rm_draft_point points[] = { rm_draft_point_make(0, 0), rm_draft_point_make(10, 0) };
    rm_draft_stroke stroke = rm_draft_stroke_make(RM_TOOL_PENCIL_2, RM_COLOR_BLUE, points, 2);
    RM_CHECK(stroke.tool == RM_TOOL_PENCIL_2 && stroke.color == RM_COLOR_BLUE, "tool and colour");
    RM_CHECK(!stroke.has_argb && stroke.thickness == 2, "no argb, thickness 2");
    RM_CHECK(stroke.points == points && stroke.point_count == 2, "the points, borrowed");

    rm_page_draft draft = sample_draft();
    draft.strokes = &stroke;
    draft.stroke_count = 1;
    draft.marks = nullptr;
    draft.mark_count = 0;
    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr && page->stroke_count == 1, "the stroke comes back");
    if (page != nullptr && page->stroke_count == 1) {
        RM_CHECK(page->strokes[0].thickness_scale == 2, "with thickness 2 (%g)",
                 page->strokes[0].thickness_scale);
    }
    rm_page_free(page);
}

static bool writes_the_same_bytes(const rm_page_draft *a, const rm_page_draft *b) {
    uint8_t *first = nullptr, *second = nullptr;
    size_t first_size = 0, second_size = 0;
    rm_error error;
    if (rm_write_page(a, &first, &first_size, &error) != RM_OK) return false;
    if (rm_write_page(b, &second, &second_size, &error) != RM_OK) { rm_bytes_free(first); return false; }
    bool same = first_size == second_size && memcmp(first, second, first_size) == 0;
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
    rm_draft_paragraph plain_text[] = { { .text = "No faces here" } };
    rm_page_draft plain = sample_draft();
    plain.paragraphs = plain_text;
    plain.paragraph_count = 1;
    plain.marks = nullptr;
    plain.mark_count = 0;
    RM_CHECK(writes_the_same_bytes(&plain, &plain), "a plain page writes one page");

    /* The case that was not deterministic: faces bring the second author. */
    rm_draft_paragraph faced_text[] = { { .text = "Bold", .bold = true },
                                        { .text = "Italic", .italic = true } };
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
    RM_CHECK(!writes_the_same_bytes(&faced, &other), "a different author writes a different page");
}

static void an_explicit_marks_author_is_used(void) {
    rm_draft_paragraph faced_text[] = { { .text = "Bold", .bold = true } };
    rm_page_draft faced = sample_draft();
    faced.paragraphs = faced_text;
    faced.paragraph_count = 1;
    faced.marks = nullptr;
    faced.mark_count = 0;

    rm_page_draft named = faced;
    named.has_marks_author = true;
    memcpy(named.marks_author, NAMED, 16);
    RM_CHECK(writes_the_same_bytes(&named, &named), "a named author is still deterministic");
    RM_CHECK(!writes_the_same_bytes(&named, &faced), "and is not the derived one");

    /* A page with no faces names no second author at all, so the field changes
     * nothing there. */
    rm_draft_paragraph plain_text[] = { { .text = "No faces here" } };
    rm_page_draft plain = sample_draft();
    plain.paragraphs = plain_text;
    plain.paragraph_count = 1;
    plain.marks = nullptr;
    plain.mark_count = 0;
    rm_page_draft plain_named = plain;
    plain_named.has_marks_author = true;
    memcpy(plain_named.marks_author, NAMED, 16);
    RM_CHECK(writes_the_same_bytes(&plain, &plain_named), "with no faces the field is unread");
}

/* A page's own template name is not in the `.rm` — it comes from the
 * document's index — so the parser never sets it and the layer that holds
 * documents does. */
static void a_template_can_be_stamped_on_a_parsed_page(void) {
    rm_page_draft draft = sample_draft();
    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "the page round-trips");
    if (page == nullptr) return;
    RM_CHECK(!page->has_template_name, "a parsed page names no template");
    RM_CHECK(rm_page_set_template(page, "Grid") == RM_OK, "and takes one");
    RM_CHECK_EQ_STR(page->template_name.bytes, page->template_name.length, "Grid", "the name");
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
    static const char *const names[4] = { "first.png", "second.png", "third.png",
                                          "fourth.png" };
    for (size_t i = 0; i < 4; i += 1) {
        images[i] = (rm_draft_image){ .file = names[i], .x = (float)(-50 + 10 * (int)i),
                                      .y = 100, .width = 120, .height = 80 };
        memcpy(images[i].key, key, 16);
        images[i].key[15] = (uint8_t)(0xE0u + i);
    }
    draft.images = images;
    draft.image_count = 4;

    rm_page *page = round_trip(&draft);
    RM_CHECK(page != nullptr, "the page round-trips");
    if (page == nullptr) return;
    RM_CHECK_EQ_SIZE(page->image_count, 4, "all four images come back");
    size_t matched = page->image_count < 4 ? page->image_count : 4;
    for (size_t i = 0; i < matched; i += 1) {
        RM_CHECK_EQ_STR(page->images[i].file.bytes, page->images[i].file.length, names[i],
                        "image %zu keeps its own name", i);
    }
    rm_page_free(page);
}

/* A count without its items is a caller's slip, and the writer answers it at
 * the door: read on trust it was a null dereference inside the text layout,
 * which is a crash in a library whose whole job is to be handed other
 * people's data. A null beside a count of nought stays what it always was —
 * a draft saying it has none of that kind. */
static void a_count_without_its_items_is_refused(void) {
    struct { const char *what; size_t paragraphs, strokes, marks, images; } cases[] = {
        { "paragraphs", 2, 0, 0, 0 },
        { "strokes", 0, 2, 0, 0 },
        { "marks", 0, 0, 2, 0 },
        { "images", 0, 0, 0, 2 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i += 1) {
        rm_page_draft draft = { .sheet = { 1404, 1872, 1404, 1872 }, .column = { 936, 1 } };
        draft.paragraph_count = cases[i].paragraphs;
        draft.stroke_count = cases[i].strokes;
        draft.mark_count = cases[i].marks;
        draft.image_count = cases[i].images;
        uint8_t *bytes = nullptr;
        size_t size = 0;
        rm_error error = { 0 };
        rm_status status = rm_write_page(&draft, &bytes, &size, &error);
        RM_CHECK(status == RM_ERR_INVALID_ARGUMENT, "a null %s array is refused (got %s)",
                 cases[i].what, rm_status_string(status));
        RM_CHECK(strcmp(error.message, cases[i].what) == 0, "and the error names it (got \"%s\")",
                 error.message);
        RM_CHECK(bytes == nullptr, "and nothing is handed back");
        rm_bytes_free(bytes);
    }

    /* The other half of the rule: nought and null together are an empty page,
     * not a refusal. */
    rm_page_draft empty = { .sheet = { 1404, 1872, 1404, 1872 }, .column = { 936, 1 } };
    uint8_t *bytes = nullptr;
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
    uint8_t *bytes = nullptr;
    size_t size = 0;
    rm_error error;
    RM_CHECK(rm_write_page(&draft, &bytes, &size, &error) == RM_OK, "the page is written");
    if (bytes == nullptr) return;

    /* Tag 2 as a byte4 (0x24) followed by the float32 of -464, the pen origin
     * the sample's mark carries. */
    static const uint8_t origin[] = { 0x24, 0x00, 0x00, 0xE8, 0xC3 };
    size_t at = SIZE_MAX;
    for (size_t i = 0; i + sizeof origin <= size; i += 1) {
        if (memcmp(bytes + i, origin, sizeof origin) == 0) { at = i; break; }
    }
    RM_CHECK(at != SIZE_MAX, "the stamped origin is found");
    if (at != SIZE_MAX) {
        static const uint8_t nan32[] = { 0x00, 0x00, 0xC0, 0x7F };
        memcpy(bytes + at + 1, nan32, sizeof nan32);
        rm_page *page = nullptr;
        RM_CHECK(rm_parse_page(bytes, size, &page, &error) == RM_OK, "the page parses");
        if (page != nullptr) {
            RM_CHECK_EQ_SIZE(page->stroke_count, 2, "both strokes are drawn");
            if (page->stroke_count == 2) {
                RM_CHECK(page->strokes[1].has_anchor, "the mark keeps its anchor");
                RM_CHECK(!page->strokes[1].anchor.has_origin_x, "and loses only the horizontal");
            }
            rm_page_free(page);
        }
    }
    rm_bytes_free(bytes);
}

static void missing_point_arrays_are_refused(void) {
    for (unsigned anchored = 0; anchored < 2; anchored += 1) {
        rm_draft_stroke stroke = { .tool = RM_TOOL_FINELINER_2, .thickness = 1,
                                   .points = nullptr, .point_count = 1 };
        rm_draft_mark mark = { .anchor = { .kind = RM_ANCHOR_BEFORE_TEXT }, .stroke = stroke };
        rm_page_draft draft = { .sheet = { 1404, 1872, 1404, 1872 }, .column = { 936, 1 } };
        if (anchored) { draft.marks = &mark; draft.mark_count = 1; }
        else { draft.strokes = &stroke; draft.stroke_count = 1; }
        uint8_t *bytes = nullptr;
        size_t size = 123;
        rm_error error;
        rm_status status = rm_write_page(&draft, &bytes, &size, &error);
        RM_CHECK(status == RM_ERR_INVALID_ARGUMENT && error.status == status,
                 "a missing %s point array is refused", anchored ? "anchored" : "loose");
        RM_CHECK(strcmp(error.message, anchored ? "marks.stroke.points" : "strokes.points") == 0,
                 "the missing array is named");
        RM_CHECK(bytes == nullptr && size == 123, "outputs are untouched on failure");
        rm_bytes_free(bytes);

        stroke.point_count = 0;
        mark.stroke.point_count = 0;
        bytes = nullptr;
        status = rm_write_page(&draft, &bytes, &size, &error);
        RM_CHECK(status == RM_OK, "a null array with zero points remains valid");
        rm_bytes_free(bytes);
    }
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
    return rm_test_report("writer");
}
