/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Third-party pages from remarkable_rust's sample corpus (see the LICENSE
 * beside them): the one set of `.rm` bytes in this repository that no writer
 * of ours produced, spanning the whole v6 format. A change in the format must
 * not break the parser silently, and pages we assemble ourselves can only ever
 * prove the parser reads what the writer writes.
 *
 * The expected numbers are fixed, not a wish: what each page holds, which is
 * what the parser has to go on reading. `Bold_Heading_Bullet_Normal` really
 * decodes to those four paragraph styles, `test-crdt-ordering` really
 * assembles to `A12_Z`. */

#include "rm_test.h"
#include "rmlines/rmlines.h"

typedef struct sample {
  const char* name;
  size_t strokes;
  size_t highlights;
  /* The typed lines, when the page has text; null for ink-only pages. */
  const char* const* lines;
  size_t line_count;
} sample;

#define LINES(...)                    \
  (const char* const[]){__VA_ARGS__}, \
      sizeof((const char* const[]){__VA_ARGS__}) / sizeof(char*)

static const sample corpus[] = {
    {"Bold_Heading_Bullet_Normal.rm", 0, 0,
     LINES("A", "new line", "B is a letter of the alphabet", "C")},
    {"Color_and_tool_v3.14.4.rm", 25, 6, nullptr, 0},
    {"Lines_v2.rm", 10, 0, nullptr, 0},
    {"Lines_v2_updated.rm", 10, 0, LINES("")},
    {"More_color_highlight_shader_v3.15.4.2.rm", 23, 0, nullptr, 0},
    {"Normal_AB.rm", 0, 0, LINES("AB")},
    {"Normal_A_stroke_2_layers.rm", 2, 0, LINES("A")},
    {"Normal_A_stroke_2_layers_v3.2.2.rm", 3, 0, LINES("A", "v3.2.2")},
    {"Normal_A_stroke_2_layers_v3.3.2.rm", 9, 0,
     LINES("A", "v3.2.2", "Normal bold italic", "Bold italic normal",
           "Bold line", "Normal line", "Heading line")},
    {"Wikipedia_highlighted_p1.rm", 0, 4, nullptr, 0},
    {"Wikipedia_highlighted_p2.rm", 0, 2, nullptr, 0},
    {"With_SceneInfo_Block.rm", 13, 0,
     LINES("TEST TEXT", "Test text from keyboard")},
    {"test-crdt-ordering.rm", 0, 0, LINES("A12_Z")},
};

static rm_page* parse_sample(const char* name) {
  char path[1024];
  rm_test_fixture_path(path, sizeof path, name);
  size_t size = 0;
  unsigned char* bytes = rm_test_read_file(path, &size);
  if (bytes == nullptr) {
    fprintf(stderr, "cannot read %s\n", path);
    return nullptr;
  }
  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(bytes, size, &page, &error);
  free(bytes);
  if (status != RM_OK) {
    fprintf(stderr, "%s: %s\n", name, rm_status_string(status));
    return nullptr;
  }
  return page;
}

static void sample_parses_to_what_it_held_when_taken_in(void) {
  for (size_t i = 0; i < sizeof corpus / sizeof *corpus; i += 1) {
    const sample* expected = &corpus[i];
    rm_page* page = parse_sample(expected->name);
    RM_CHECK(page != nullptr, "%s does not parse at all", expected->name);
    if (page == nullptr) continue;

    RM_CHECK_EQ_SIZE(page->stroke_count, expected->strokes, "%s",
                     expected->name);
    RM_CHECK_EQ_SIZE(page->highlight_count, expected->highlights, "%s",
                     expected->name);
    RM_CHECK_EQ_SIZE(page->image_count, 0, "%s", expected->name);
    /* Every block in every sample is one the parser understands — a
     * page that introduces a new block shows up here first. */
    RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "%s", expected->name);

    if (expected->lines == nullptr) {
      RM_CHECK(!page->has_text, "%s should carry no typed text",
               expected->name);
    } else {
      RM_CHECK(page->has_text, "%s should carry typed text", expected->name);
      if (page->has_text) {
        RM_CHECK_EQ_SIZE(page->text.paragraph_count, expected->line_count, "%s",
                         expected->name);
        size_t shared = page->text.paragraph_count < expected->line_count
                            ? page->text.paragraph_count
                            : expected->line_count;
        for (size_t line = 0; line < shared; line += 1) {
          RM_CHECK_EQ_STR(page->text.paragraphs[line].text.bytes,
                          page->text.paragraphs[line].text.length,
                          expected->lines[line], "%s line %zu", expected->name,
                          line);
        }
      }
    }
    rm_page_free(page);
  }
}

/* The sample's name promises the four styles in order; our decoding of the
 * style codes has to land on the same four. This crosses two independent
 * sources: theirs of what the tablet wrote, ours of what the codes mean. */
static void paragraph_styles_match_the_sample_name(void) {
  rm_page* page = parse_sample("Bold_Heading_Bullet_Normal.rm");
  RM_CHECK(page != nullptr, "the sample parses");
  if (page == nullptr) return;
  RM_CHECK(page->has_text && page->text.paragraph_count == 4,
           "four paragraphs");
  if (page->has_text && page->text.paragraph_count == 4) {
    const uint8_t wanted[4] = {3, 2, 4, 1}; /* bold, heading, bullet, plain */
    for (size_t i = 0; i < 4; i += 1) {
      RM_CHECK(page->text.paragraphs[i].has_format,
               "paragraph %zu carries a style", i);
      RM_CHECK_EQ_SIZE(page->text.paragraphs[i].format.raw, wanted[i],
                       "paragraph %zu", i);
    }
  }
  rm_page_free(page);
}

/* Bold and italic fences survive a page whose text moved around: the sample
 * carries two lines with face runs. */
static void face_runs_survive_moved_text(void) {
  rm_page* page = parse_sample("Normal_A_stroke_2_layers_v3.3.2.rm");
  RM_CHECK(page != nullptr, "the sample parses");
  if (page == nullptr) return;
  size_t lines_with_faces = 0;
  for (size_t i = 0; i < page->text.paragraph_count; i += 1) {
    if (page->text.paragraphs[i].face_count != 0) lines_with_faces += 1;
  }
  RM_CHECK_EQ_SIZE(lines_with_faces, 2, "lines carrying a face run");
  rm_page_free(page);
}

int main(void) {
  sample_parses_to_what_it_held_when_taken_in();
  paragraph_styles_match_the_sample_name();
  face_runs_survive_moved_text();
  return rm_test_report("corpus");
}
