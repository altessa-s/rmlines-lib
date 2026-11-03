/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The header, the column and the style record decoded.
 *
 * Three of the tablet's presentations cannot be told apart by `tag 1` alone,
 * and the arithmetic that tells them apart is the kind that is right until
 * somebody tidies it: a heading level read off the presence of two further
 * fields, a nesting depth packed into half a byte. Written down here so that
 * tidying it breaks a test rather than a page. */

#include "rm_test.h"
#include "rmlines/rmlines.h"

static int version_of(const char* header) {
  int version = -1;
  return rm_header_version(header, strlen(header), &version) ? version : -1;
}

/* The difference the application above this library rests on: a page of an
 * older generation is refused *by its version*, and bytes of no known shape
 * are refused as a format nobody can name. */
static void the_version_is_read_out_of_the_header(void) {
  RM_CHECK(version_of("reMarkable .lines file, version=3          ") == 3,
           "version 3");
  RM_CHECK(version_of("reMarkable .lines file, version=5          ") == 5,
           "version 5");
  RM_CHECK(version_of(RM_HEADER) == 6, "the header this library writes");
  /* A generation nobody has written down yet has to arrive as a number too:
   * that is the difference between "an older format" and "we cannot tell". */
  RM_CHECK(version_of("reMarkable .lines file, version=7          ") == 7,
           "a future 7");

  RM_CHECK(version_of("reMarkable lines with selections and layers") == -1,
           "not a header");
  RM_CHECK(version_of("reMarkable .lines file, version=           ") == -1,
           "no digits");
  RM_CHECK(version_of("%PDF-1.4") == -1, "a PDF");
}

/* The widest column that stands on a sheet with a margin. The rule is not "no
 * wider than the sheet", and the difference showed on exactly one device: a
 * Move's sheet is 954 units and the medium column is 936, which fits with nine
 * units to spare on each side — a page of text running into both edges. The
 * share of the sheet is the whole of the rule, and a test is what keeps
 * somebody from simplifying it back to a comparison. */
static void the_column_leaves_a_share_of_the_sheet(void) {
  /* reMarkable Paper Pro Move: only the narrow column has a margin here. */
  RM_CHECK(rm_text_column_widest_fitting(954) == RM_COLUMN_NARROW, "a Move");
  /* reMarkable 2 — the tablet's own pairing: wide is 1152 of 1404. */
  RM_CHECK(rm_text_column_widest_fitting(1404) == RM_COLUMN_WIDE,
           "a reMarkable 2");
  RM_CHECK(rm_text_column_widest_fitting(1620) == RM_COLUMN_WIDE,
           "a Paper Pro");
  /* A sheet between the two: medium has a margin, wide does not. */
  RM_CHECK(rm_text_column_widest_fitting(1200) == RM_COLUMN_MEDIUM,
           "between the two");
  /* Narrower than anything, and still an answer rather than a crash. */
  RM_CHECK(rm_text_column_widest_fitting(1) == RM_COLUMN_NARROW, "a sliver");
  RM_CHECK(rm_text_column_widest_fitting(0) == RM_COLUMN_NARROW,
           "nothing at all");
}

static void widths_and_settings_agree(void) {
  RM_CHECK(rm_text_column_width(RM_COLUMN_NARROW) == 740.0f, "narrow");
  RM_CHECK(rm_text_column_width(RM_COLUMN_MEDIUM) == 936.0f, "medium");
  RM_CHECK(rm_text_column_width(RM_COLUMN_WIDE) == 1152.0f, "wide");
  RM_CHECK(rm_text_column_setting(RM_COLUMN_NARROW) == 0, "narrow setting");
  RM_CHECK(rm_text_column_setting(RM_COLUMN_MEDIUM) == 1, "medium setting");
  RM_CHECK(rm_text_column_setting(RM_COLUMN_WIDE) == 2, "wide setting");

  /* A draft column built from one of the three cannot pair a width with the
   * wrong setting. */
  const rm_text_column all[] = {RM_COLUMN_NARROW, RM_COLUMN_MEDIUM,
                                RM_COLUMN_WIDE};
  for (size_t i = 0; i < 3; i += 1) {
    rm_draft_column drafted = rm_draft_column_of(all[i]);
    RM_CHECK(drafted.width == rm_text_column_width(all[i]), "width %zu", i);
    RM_CHECK(drafted.setting == rm_text_column_setting(all[i]), "setting %zu",
             i);
  }
}

static rm_paragraph_format format(uint8_t raw, int second, long third) {
  rm_paragraph_format out = {.raw = raw};
  if (second >= 0) {
    out.has_second = true;
    out.second = (uint8_t)second;
  }
  if (third >= 0) {
    out.has_third = true;
    out.third = (uint32_t)third;
  }
  return out;
}

/* Heading 2 and heading 3 both carry tag 1 = 3 and are told apart by what
 * follows. */
static void heading_levels_come_from_the_whole_record(void) {
  rm_paragraph_format heading = format(2, -1, -1);
  rm_paragraph_format second = format(3, 2, 3);
  rm_paragraph_format third = format(3, -1, -1);
  rm_paragraph_format plain = format(1, -1, -1);
  RM_CHECK(rm_paragraph_format_heading_level(&heading) == 1, "heading 1");
  RM_CHECK(rm_paragraph_format_heading_level(&second) == 2, "heading 2");
  RM_CHECK(rm_paragraph_format_heading_level(&third) == 3, "heading 3");
  RM_CHECK(rm_paragraph_format_heading_level(&plain) == 0,
           "a body paragraph is no heading");
}

/* The nesting depth is the low half of tag 3, counted from one: bullet
 * 0x01/0x02, checkbox 0x11/0x12, numbered 0x21/0x22 — the family is the high
 * half and must not reach the answer. */
static void list_levels_come_from_the_low_nibble(void) {
  rm_paragraph_format first_bullet = format(4, -1, -1);
  rm_paragraph_format first_checkbox = format(6, -1, -1);
  RM_CHECK(rm_paragraph_format_level(&first_bullet) == 1,
           "the first level carries no tag 3");
  RM_CHECK(rm_paragraph_format_level(&first_checkbox) == 1,
           "nor does a first checkbox");

  const struct {
    uint8_t raw;
    uint32_t third;
    int level;
  } nested[] = {
      {5, 0x01, 2}, {5, 0x02, 3},  {8, 0x11, 2},
      {8, 0x12, 3}, {11, 0x21, 2}, {11, 0x22, 3},
  };
  for (size_t i = 0; i < sizeof nested / sizeof *nested; i += 1) {
    rm_paragraph_format item = format(nested[i].raw, 3, (long)nested[i].third);
    RM_CHECK(rm_paragraph_format_level(&item) == nested[i].level, "nested %zu",
             i);
  }
  /* A paragraph that is not a list item has no depth, whatever tag 3
   * holds. */
  rm_paragraph_format not_a_list = format(1, 3, 0x12);
  RM_CHECK(rm_paragraph_format_level(&not_a_list) == 1,
           "a body paragraph has no depth");
}

/* A style this library does not know is kept rather than dropped. Dropping
 * the record would lose the paragraph's indent along with the marker it could
 * not draw. */
static void an_unknown_style_survives_as_its_raw_value(void) {
  rm_paragraph_format unseen = format(99, 7, 7);
  RM_CHECK(rm_paragraph_format_style(&unseen) == RM_PARA_UNKNOWN,
           "no name for it");
  RM_CHECK(unseen.raw == 99, "the number is kept");
  RM_CHECK(rm_paragraph_format_has_further_fields(&unseen),
           "and so is the rest of the record");
  RM_CHECK(rm_paragraph_format_heading_level(&unseen) == 0, "it is no heading");
  RM_CHECK(rm_paragraph_format_level(&unseen) == 1, "and no list item");
}

static void list_families_group_the_styles(void) {
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_BULLET) == RM_LIST_BULLET,
           "bullet");
  RM_CHECK(
      rm_paragraph_style_list_family(RM_PARA_BULLET_NESTED) == RM_LIST_BULLET,
      "nested");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_CHECKBOX) == RM_LIST_CHECKBOX,
           "checkbox");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_CHECKBOX_CHECKED) ==
               RM_LIST_CHECKBOX,
           "checked");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_CHECKBOX_NESTED) ==
               RM_LIST_CHECKBOX,
           "nested");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_NUMBERED) == RM_LIST_NUMBERED,
           "numbered");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_NUMBERED_NESTED) ==
               RM_LIST_NUMBERED,
           "nested");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_PLAIN) == RM_LIST_NONE,
           "a body paragraph");
  RM_CHECK(rm_paragraph_style_list_family(RM_PARA_HEADING) == RM_LIST_NONE,
           "a heading");

  const rm_paragraph_style items[] = {
      RM_PARA_BULLET,           RM_PARA_BULLET_NESTED,   RM_PARA_CHECKBOX,
      RM_PARA_CHECKBOX_CHECKED, RM_PARA_CHECKBOX_NESTED, RM_PARA_NUMBERED,
      RM_PARA_NUMBERED_NESTED,
  };
  for (size_t i = 0; i < sizeof items / sizeof *items; i += 1) {
    RM_CHECK(rm_paragraph_style_is_list_item(items[i]),
             "style %d is one of a list", (int)items[i]);
  }
  RM_CHECK(!rm_paragraph_style_is_list_item(RM_PARA_PLAIN),
           "a body paragraph is not");
  RM_CHECK(!rm_paragraph_style_is_list_item(RM_PARA_HEADING),
           "nor is a heading");
}

/* The eraser must not be drawn, and the marker says something about what lies
 * under it — the two questions a renderer asks of a tool before anything
 * else. */
static void the_tools_answer_the_two_questions_about_them(void) {
  RM_CHECK(rm_pen_tool_is_eraser(RM_TOOL_ERASER), "the eraser");
  RM_CHECK(rm_pen_tool_is_eraser(RM_TOOL_ERASER_AREA), "the area eraser");
  RM_CHECK(!rm_pen_tool_is_eraser(RM_TOOL_FINELINER_2),
           "a fineliner is not one");
  RM_CHECK(rm_pen_tool_is_highlighter(RM_TOOL_HIGHLIGHTER_1),
           "the first highlighter");
  RM_CHECK(rm_pen_tool_is_highlighter(RM_TOOL_HIGHLIGHTER_2), "the second");
  RM_CHECK(!rm_pen_tool_is_highlighter(RM_TOOL_SHADER),
           "the shader is not one");
  RM_CHECK(!rm_pen_tool_is_eraser(RM_TOOL_UNKNOWN),
           "and a tool we cannot name is neither");
}

/* The version string is spelled from the three numbers rather than written
 * beside them, and the build reads the same three numbers for the package: one
 * edit is a release, and this is what shows the spelling holds together. */
static void the_version_string_is_spelled_from_its_numbers(void) {
  char expected[32];
  snprintf(expected, sizeof expected, "%d.%d.%d", RMLINES_VERSION_MAJOR,
           RMLINES_VERSION_MINOR, RMLINES_VERSION_PATCH);
  RM_CHECK(strcmp(RMLINES_VERSION_STRING, expected) == 0, "%s vs %s",
           RMLINES_VERSION_STRING, expected);
}

int main(void) {
  the_version_string_is_spelled_from_its_numbers();
  the_version_is_read_out_of_the_header();
  the_column_leaves_a_share_of_the_sheet();
  widths_and_settings_agree();
  heading_levels_come_from_the_whole_record();
  list_levels_come_from_the_low_nibble();
  an_unknown_style_survives_as_its_raw_value();
  list_families_group_the_styles();
  the_tools_answer_the_two_questions_about_them();
  return rm_test_report("model");
}
