/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* A count in the file is a declaration, not a measurement. Believed, it either
 * wraps into a different number — the worst way to be wrong — or reserves
 * memory nobody has. These reach into the reader directly because that is
 * where the arithmetic is; what a whole page declaring a number it does not
 * carry does is the parser's, and is tested through it. */

#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

/* `UINT64_MAX` is a valid varuint and, on a 32-bit machine, not a valid
 * `size_t`. The ten-byte form has to read back as the number it is. */
static void the_widest_varuint_reads_back_whole(void) {
  const uint8_t widest[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                            0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  rm_reader reader;
  rm_reader_init(&reader, widest, sizeof widest);
  uint64_t value = 0;
  RM_CHECK(rm_read_varuint(&reader, &value) == RM_OK,
           "the ten-byte form is a legal varuint");
  RM_CHECK(value == UINT64_MAX, "and it is the largest one");

  /* As a count it is refused: a count is an index into memory, and a value
   * no object can hold is a corrupted field rather than a large list. Read
   * as a plain `size_t` it would be believed on a 64-bit machine, and the
   * list parser above it would answer with the empty list it had got as far
   * as instead of reporting a block it could not read. */
  rm_reader_init(&reader, widest, sizeof widest);
  size_t count = 0;
  RM_CHECK(rm_read_count(&reader, &count) == RM_ERR_MALFORMED_PAGE,
           "a count no object can hold is refused, not wrapped");
}

/* The tenth byte has one bit's worth of room. Shifting the rest out in silence
 * would let a number the format cannot hold come back as another number that
 * it can. */
static void an_over_wide_varuint_is_refused(void) {
  const uint8_t too_wide[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0x03};
  rm_reader reader;
  rm_reader_init(&reader, too_wide, sizeof too_wide);
  uint64_t value = 0;
  RM_CHECK(rm_read_varuint(&reader, &value) == RM_ERR_MALFORMED_PAGE,
           "refused rather than narrowed");
}

/* A varuint with the continuation bit set on every byte and no end to it runs
 * out of room rather than out of the buffer. */
static void an_unterminated_varuint_ends(void) {
  const uint8_t never_ends[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                                0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  rm_reader reader;
  rm_reader_init(&reader, never_ends, sizeof never_ends);
  uint64_t value = 0;
  RM_CHECK(rm_read_varuint(&reader, &value) != RM_OK,
           "it does not read as a number");
}

/* Every scalar is little-endian, and a read that would run off the end moves
 * the position nowhere. */
static void scalars_are_little_endian_and_bounded(void) {
  const uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x80, 0x3F};
  rm_reader reader;
  rm_reader_init(&reader, bytes, sizeof bytes);
  uint16_t small = 0;
  uint32_t big = 0;
  RM_CHECK(rm_read_u16(&reader, &small) == RM_OK && small == 0x0201u,
           "uint16 %u", small);
  reader.offset = 0;
  RM_CHECK(rm_read_u32(&reader, &big) == RM_OK && big == 0x04030201u,
           "uint32 %u", big);
  reader.offset = 4;
  float one = 0;
  RM_CHECK(rm_read_f32(&reader, &one) == RM_OK && one == 1.0f, "float32 %f",
           (double)one);

  reader.offset = sizeof bytes - 1u;
  RM_CHECK(rm_read_u32(&reader, &big) == RM_ERR_UNEXPECTED_EOF,
           "a short read is refused");
  RM_CHECK(reader.offset == sizeof bytes - 1u, "and leaves the position alone");
}

/* A tag that is not the expected one is a question answered `false` with the
 * position restored — the format has optional fields, and asking must not cost
 * the reader its place. */
static void a_tag_that_is_not_there_costs_nothing(void) {
  const uint8_t bytes[] = {0x1C, 0x04, 0x00, 0x00, 0x00}; /* tag 1 / length4 */
  rm_reader reader;
  rm_reader_init(&reader, bytes, sizeof bytes);
  RM_CHECK(!rm_match_tag(&reader, 2, RM_TAG_LENGTH4), "the wrong index");
  RM_CHECK(reader.offset == 0, "the position is where it was");
  RM_CHECK(!rm_match_tag(&reader, 1, RM_TAG_BYTE4), "the wrong type");
  RM_CHECK(reader.offset == 0, "still where it was");
  RM_CHECK(rm_match_tag(&reader, 1, RM_TAG_LENGTH4),
           "and the right one is found");
  RM_CHECK(reader.offset == 1, "past the tag");
}

/* A tag whose index is wider than 32 bits is still a tag: an unknown field to
 * step over, not a broken stream. */
static void a_tag_with_a_huge_index_is_an_unknown_field(void) {
  /* 2^36 << 4 | length4: a varuint of six bytes. */
  const uint8_t bytes[] = {0x8C, 0x80, 0x80, 0x80, 0x80, 0x10};
  rm_reader reader;
  rm_reader_init(&reader, bytes, sizeof bytes);
  uint32_t index = 0;
  uint8_t type = 0;
  RM_CHECK(rm_read_tag(&reader, &index, &type) == RM_OK, "the tag reads");
  RM_CHECK(index == UINT32_MAX && type == RM_TAG_LENGTH4,
           "as no field anybody reads (%u/%u)", index, (unsigned)type);
  RM_CHECK(reader.offset == sizeof bytes, "and the reader is past it");
}

/* The length in front of a string is a byte count and not a count of
 * characters, and outside ASCII the two are different numbers. It is also the
 * file's word: a length past the end of the field it sits in is clamped, so
 * that a short or padded record cannot shift whatever is read after it. */
static void a_string_is_counted_in_bytes_and_clamped_to_its_field(void) {
  rm_arena* arena = rm_arena_create();
  /* length 4, marker 1, then "äö" — two scalars, four bytes. */
  const uint8_t bytes[] = {0x04, 0x01, 0xC3, 0xA4, 0xC3, 0xB6};
  rm_reader reader;
  rm_reader_init(&reader, bytes, sizeof bytes);
  rm_str text = {0};
  RM_CHECK(rm_read_string(&reader, sizeof bytes, arena, &text) == RM_OK,
           "it reads");
  RM_CHECK_EQ_STR(text.bytes, text.length, "äö", "two scalars in four bytes");
  RM_CHECK(rm_utf8_scalar_count(text.bytes, text.length) == 2,
           "and two is what it counts");

  /* The same bytes with the field cut short: the declaration says four, the
   * field holds two, and two is what comes back. */
  rm_reader_init(&reader, bytes, sizeof bytes);
  RM_CHECK(rm_read_string(&reader, 4, arena, &text) == RM_OK,
           "the short field reads");
  RM_CHECK(text.length == 2, "clamped to the field, not to the declaration");
  rm_arena_destroy(arena);
}

/* Ill-formed UTF-8 is repaired the way a Unicode-conformant decoder does —
 * one U+FFFD per maximal subpart. Matching that is not cosmetic: the file
 * numbers characters by scalar, so a byte sequence counted differently here
 * shifts every anchor and every style key behind it. */
static void broken_utf8_is_repaired_by_maximal_subpart(void) {
  char out[64];
  /* A lone continuation byte, a truncated three-byte sequence, and a valid
   * scalar behind them. */
  const char broken[] = {(char)0x80, (char)0xE2, (char)0x82, 'A'};
  size_t length = rm_utf8_repair(broken, sizeof broken, out);
  RM_CHECK_EQ_STR(out, length,
                  "\xEF\xBF\xBD\xEF\xBF\xBD"
                  "A",
                  "two replacements and an A");
  RM_CHECK(rm_utf8_scalar_count(out, length) == 3, "three scalars");

  /* An overlong form is not a scalar: every byte of it is its own
   * replacement, because the second byte is not a legal continuation of the
   * first. */
  const char overlong[] = {(char)0xC0, (char)0xAF};
  length = rm_utf8_repair(overlong, sizeof overlong, out);
  RM_CHECK(rm_utf8_scalar_count(out, length) == 2,
           "an overlong slash is two replacements");

  /* A surrogate half, encoded: the same. */
  const char surrogate[] = {(char)0xED, (char)0xA0, (char)0x80};
  length = rm_utf8_repair(surrogate, sizeof surrogate, out);
  RM_CHECK(rm_utf8_scalar_count(out, length) == 3, "a surrogate half is three");

  /* And what is already well-formed passes through untouched. */
  const char good[] = "Grüße 🖋";
  length = rm_utf8_repair(good, strlen(good), out);
  RM_CHECK_EQ_STR(out, length, good, "valid UTF-8 is not touched");
}

static void subblocks_and_string_counts_stay_bounded(void) {
  const uint8_t bytes[] = {1, 0, 0, 0, 0x80, 0x01};
  rm_reader reader, child;
  rm_reader_init(&reader, bytes, sizeof bytes);
  RM_CHECK(rm_read_subblock(&reader, &child) == RM_OK,
           "one-byte subblock opens");
  RM_CHECK_EQ_SIZE(reader.offset, 5, "the parent skips to the next field");
  rm_arena* arena = rm_arena_create();
  rm_str text;
  RM_CHECK(
      rm_read_string(&child, child.size, arena, &text) == RM_ERR_UNEXPECTED_EOF,
      "the string's varuint cannot borrow its final byte from the next field");
  uint8_t next = 0;
  RM_CHECK(rm_read_u8(&reader, &next) == RM_OK && next == 1,
           "the next field is intact");
  rm_reader_init(&reader, bytes, 4);
  RM_CHECK(rm_read_subblock(&reader, &child) == RM_ERR_MALFORMED_PAGE,
           "a body longer than its parent is refused");
  rm_arena_destroy(arena);
}

int main(void) {
  subblocks_and_string_counts_stay_bounded();
  the_widest_varuint_reads_back_whole();
  an_over_wide_varuint_is_refused();
  an_unterminated_varuint_ends();
  scalars_are_little_endian_and_bounded();
  a_tag_that_is_not_there_costs_nothing();
  a_tag_with_a_huge_index_is_an_unknown_field();
  a_string_is_counted_in_bytes_and_clamped_to_its_field();
  broken_utf8_is_repaired_by_maximal_subpart();
  return rm_test_report("reader");
}
