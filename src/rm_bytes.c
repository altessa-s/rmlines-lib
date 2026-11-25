/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The byte level of the format, both ways: little-endian scalars, varuints,
 * tags, identifiers and tagged subblocks.
 *
 * Every byte the writing half puts down is one the tablet itself writes, and
 * nothing below may change one of them unless the tablet is shown to accept
 * the change. */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rm_wire_write.h"
#include "rmlines/parser.h"

bool rm_header_version(const char* header, size_t length, int* out_version) {
  if (header == nullptr) return false;
  constexpr size_t prefix = sizeof(RM_HEADER_PREFIX) - 1u;
  if (length < prefix) return false;
  if (memcmp(header, RM_HEADER_PREFIX, prefix) != 0) return false;

  /* A single leading digit is what the format writes; the padding spaces
   * behind it stop the scan. An empty run means the header only looks like
   * one. */
  size_t at = prefix;
  int value = 0;
  size_t digits = 0;
  while (at < length && header[at] >= '0' && header[at] <= '9') {
    int digit = header[at] - '0';
    if (value > (INT_MAX - digit) / 10) {
      value = INT_MAX;
    } else {
      value = value * 10 + digit;
    }
    digits += 1;
    at += 1;
  }
  if (digits == 0) return false;
  if (out_version != nullptr) *out_version = value;
  return true;
}

/* When the caller asks for an error, keep the status, version, numeric value
 * and optional message together. */
void rmi_error_set(rm_error* out_error, rm_status status, int version,
                   long value, const char* message) {
  if (out_error == nullptr) return;
  *out_error = (rm_error){.status = status, .version = version, .value = value};
  if (message != nullptr) {
    snprintf(out_error->message, sizeof out_error->message, "%s", message);
  }
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

void rm_reader_init(rm_reader* reader, const uint8_t* data, size_t size) {
  *reader = (rm_reader){.data = data, .size = size, .offset = 0};
}

rm_status rm_read_seek(rm_reader* reader, size_t position) {
  if (position > reader->size) return RM_ERR_UNEXPECTED_EOF;
  reader->offset = position;
  return RM_OK;
}

/* Claim `length` bytes and answer where they begin, so that a number is read
 * out of the bytes where they lie: a point costs four to six of these reads,
 * and a copy per read would be an allocation per coordinate of every stroke. */
static rm_status rm_advance(rm_reader* reader, size_t length,
                            const uint8_t** out) {
  if (length > reader->size - reader->offset) return RM_ERR_UNEXPECTED_EOF;
  *out = reader->data + reader->offset;
  reader->offset += length;
  return RM_OK;
}

rm_status rm_read_bytes(rm_reader* reader, size_t length, const uint8_t** out) {
  return rm_advance(reader, length, out);
}

rm_status rm_read_subblock(rm_reader* reader, rm_reader* out) {
  uint32_t length;
  RM_TRY(rm_read_u32(reader, &length));
  size_t end;
  if (!rm_fits_within(reader, length, reader->size, &end))
    return RM_ERR_MALFORMED_PAGE;
  *out = *reader;
  out->size = end;
  reader->offset = end;
  return RM_OK;
}

rm_status rm_read_block_header(rm_reader* reader, rm_block_header* out) {
  out->start = reader->offset;
  uint32_t length;
  RM_TRY(rm_read_u32(reader, &length));
  RM_TRY(rm_read_u8(reader, &out->unknown));
  RM_TRY(rm_read_u8(reader, &out->min_version));
  RM_TRY(rm_read_u8(reader, &out->current_version));
  RM_TRY(rm_read_u8(reader, &out->type));
  out->body = reader->offset;
  if (!rm_fits_within(reader, length, reader->size, &out->end))
    return RM_ERR_UNEXPECTED_EOF;
  return RM_OK;
}

rm_status rm_read_u8(rm_reader* reader, uint8_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 1, &at));
  *out = at[0];
  return RM_OK;
}

rm_status rm_read_u16(rm_reader* reader, uint16_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 2, &at));
  *out = (uint16_t)((uint16_t)at[0] | (uint16_t)at[1] << 8);
  return RM_OK;
}

rm_status rm_read_u32(rm_reader* reader, uint32_t* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 4, &at));
  *out = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 |
         (uint32_t)at[3] << 24;
  return RM_OK;
}

rm_status rm_read_f32(rm_reader* reader, float* out) {
  uint32_t pattern;
  RM_TRY(rm_read_u32(reader, &pattern));
  static_assert(sizeof(float) == sizeof(uint32_t), "a float32 is four bytes");
  memcpy(out, &pattern, sizeof pattern);
  return RM_OK;
}

rm_status rm_read_f64(rm_reader* reader, double* out) {
  const uint8_t* at;
  RM_TRY(rm_advance(reader, 8, &at));
  uint64_t pattern = 0;
  for (int i = 0; i < 8; i += 1) pattern |= (uint64_t)at[i] << (8 * i);
  static_assert(sizeof(double) == sizeof(uint64_t), "a float64 is eight bytes");
  memcpy(out, &pattern, sizeof pattern);
  return RM_OK;
}

rm_status rm_read_varuint(rm_reader* reader, uint64_t* out) {
  uint64_t result = 0;
  unsigned shift = 0;
  for (;;) {
    uint8_t byte;
    RM_TRY(rm_read_u8(reader, &byte));
    /* The tenth byte has one bit's worth of room left. A shift drops
     * whatever does not fit without a word, so a number too wide for the
     * type would arrive as a different number — refused instead of
     * quietly rewritten. */
    if (shift == 63 && (byte & 0x7Eu) != 0) return RM_ERR_MALFORMED_PAGE;
    result |= (uint64_t)(byte & 0x7Fu) << shift;
    shift += 7;
    if ((byte & 0x80u) == 0) break;
    if (shift >= 64) return RM_ERR_MALFORMED_PAGE;
  }
  *out = result;
  return RM_OK;
}

rm_status rm_read_count(rm_reader* reader, size_t* out) {
  uint64_t value;
  RM_TRY(rm_read_varuint(reader, &value));
  /* Refused against PTRDIFF_MAX and not against SIZE_MAX, which on a 64-bit
   * machine would let every varuint through. A count is an index into
   * memory, and a value no object can hold is not a count — it is a
   * corrupted field, and saying so here is what makes the list parsers fail
   * as a block they could not read rather than quietly return the empty
   * list they had got as far as. */
  if (value > (uint64_t)PTRDIFF_MAX) return RM_ERR_MALFORMED_PAGE;
  *out = (size_t)value;
  return RM_OK;
}

rm_status rm_read_tag(rm_reader* reader, uint32_t* out_index,
                      uint8_t* out_type) {
  uint64_t raw;
  RM_TRY(rm_read_varuint(reader, &raw));
  uint64_t index = raw >> 4;

  /* A field index past what a 32-bit number holds is still a tag: it names no
   * field anybody reads, and a loop over a block's fields steps over it like
   * any other unknown one. Held at the top of the range, which no field of
   * the format comes near — its own run to single digits. */
  *out_index = index > UINT32_MAX ? UINT32_MAX : (uint32_t)index;
  *out_type = (uint8_t)(raw & 0xFu);
  return RM_OK;
}

bool rm_match_tag(rm_reader* reader, uint32_t index, uint8_t type) {
  size_t saved = reader->offset;
  if (rm_reader_remaining(reader) == 0) return false;
  uint32_t found_index;
  uint8_t found_type;
  if (rm_read_tag(reader, &found_index, &found_type) == RM_OK &&
      found_index == index && found_type == type) {
    return true;
  }
  reader->offset = saved;
  return false;
}

rm_status rm_expect_tag(rm_reader* reader, uint32_t index, uint8_t type) {
  return rm_match_tag(reader, index, type) ? RM_OK : RM_ERR_MALFORMED_PAGE;
}

rm_status rm_read_bare_crdt(rm_reader* reader, rm_crdt_id* out) {
  uint8_t author;
  uint64_t counter;
  RM_TRY(rm_read_u8(reader, &author));
  RM_TRY(rm_read_varuint(reader, &counter));
  *out = rm_crdt_make(author, counter);
  return RM_OK;
}

rm_status rm_read_crdt(rm_reader* reader, uint32_t index, rm_crdt_id* out) {
  RM_TRY(rm_expect_tag(reader, index, RM_TAG_ID));
  return rm_read_bare_crdt(reader, out);
}

rm_status rm_skip_crdt(rm_reader* reader, uint32_t index) {
  rm_crdt_id ignored;
  return rm_read_crdt(reader, index, &ignored);
}

rm_status rm_read_string(rm_reader* reader, size_t end, rm_arena* arena,
                         rm_str* out) {
  if (end > reader->size || reader->offset > end) return RM_ERR_MALFORMED_PAGE;
  rm_reader bounded = *reader;
  bounded.size = end;
  rm_reader* parent = reader;
  reader = &bounded;
  *out = (rm_str){.bytes = "", .length = 0};
  if (reader->offset >= end) return RM_OK;

  size_t byte_count;
  RM_TRY(rm_read_count(reader, &byte_count));
  if (reader->offset >= end) {
    parent->offset = reader->offset;
    return RM_OK;
  }
  uint8_t marker;
  RM_TRY(rm_read_u8(reader, &marker));

  size_t room = end - reader->offset;
  size_t available = byte_count < room ? byte_count : room;
  const uint8_t* bytes;
  RM_TRY(rm_read_bytes(reader, available, &bytes));

  size_t length;
  char* repaired;
  if (rm_utf8_is_valid((const char*)bytes, available)) {
    repaired = rm_arena_strndup(arena, (const char*)bytes, available);
    if (repaired == nullptr) return RM_ERR_OUT_OF_MEMORY;
    length = available;
  } else {
    size_t bound = rm_utf8_repair_bound(available);
    if (!bound) return RM_ERR_OUT_OF_MEMORY;
    repaired = rm_arena_alloc(arena, bound);
    if (repaired == nullptr) return RM_ERR_OUT_OF_MEMORY;
    length = rm_utf8_repair((const char*)bytes, available, repaired);
  }
  *out = (rm_str){.bytes = repaired, .length = length};
  parent->offset = reader->offset;
  return RM_OK;
}

/* Skip by the wire type, so an unfamiliar field with a known width need not
 * hide the fields that follow. A length4 child can be handed back bounded to
 * its own bytes. */
rm_status rmi_skip_tagged_value(rm_reader* reader, uint8_t type,
                                rm_reader* out_subblock) {
  const uint8_t* bytes;
  rm_crdt_id id;
  rm_reader sub = {0};
  switch (type) {
    case RM_TAG_ID:
      return rm_read_bare_crdt(reader, &id);
    case RM_TAG_LENGTH4:
      RM_TRY(rm_read_subblock(reader, &sub));
      if (out_subblock != nullptr) *out_subblock = sub;
      return RM_OK;
    case RM_TAG_BYTE1:
    case RM_TAG_BYTE4:
    case RM_TAG_BYTE8:
      return rm_read_bytes(reader, type, &bytes);
    default:
      return RM_ERR_MALFORMED_PAGE;
  }
}

/* The envelope every scene item starts with */
rm_status rm_parse_item_preamble(rm_reader* reader, rm_item_preamble* out) {
  RM_TRY(rm_read_crdt(reader, 1, &out->parent));
  RM_TRY(rm_read_crdt(reader, 2, &out->id));
  RM_TRY(rm_read_crdt(reader, 3, &out->left));
  RM_TRY(rm_read_crdt(reader, 4, &out->right));
  RM_TRY(rm_expect_tag(reader, 5, RM_TAG_BYTE4));
  return rm_read_u32(reader, &out->deleted);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

#ifdef RMLINES_TESTING
static _Thread_local size_t rm_test_max_block_bytes = UINT32_MAX;

void rm_test_block_limit(size_t limit) { rm_test_max_block_bytes = limit; }

size_t rmi_block_limit(void) {
  return rm_test_max_block_bytes > UINT32_MAX ? UINT32_MAX
                                              : rm_test_max_block_bytes;
}
#else
size_t rmi_block_limit(void) { return UINT32_MAX; }
#endif

/* A four-byte length can describe at most UINT32_MAX bytes. Tests lower that
 * limit so the same refusal can be exercised with an ordinary page. */
static bool rm_length_fits_block(size_t length) {
  return length <= rmi_block_limit();
}

size_t rmi_varuint_size(uint64_t value) {
  size_t count = 1u;
  while (value >= 0x80u) {
    value >>= 7;
    count += 1u;
  }
  return count;
}

void rm_write_u16(rm_buf* buf, uint16_t value) {
  rm_buf_append_byte(buf, (uint8_t)(value & 0xFFu));
  rm_buf_append_byte(buf, (uint8_t)(value >> 8));
}

void rm_write_u32(rm_buf* buf, uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    rm_buf_append_byte(buf, (uint8_t)((value >> shift) & 0xFFu));
  }
}

void rm_write_f32(rm_buf* buf, float value) {
  uint32_t pattern;
  memcpy(&pattern, &value, sizeof pattern);
  rm_write_u32(buf, pattern);
}

void rm_write_f64(rm_buf* buf, double value) {
  uint64_t pattern;
  memcpy(&pattern, &value, sizeof pattern);
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    rm_buf_append_byte(buf, (uint8_t)((pattern >> shift) & 0xFFu));
  }
}

void rm_write_varuint(rm_buf* buf, uint64_t value) {
  uint64_t rest = value;
  do {
    uint8_t byte = (uint8_t)(rest & 0x7Fu);
    rest >>= 7;
    if (rest != 0) byte |= 0x80u;
    rm_buf_append_byte(buf, byte);
  } while (rest != 0);
}

void rm_write_tag(rm_buf* buf, uint32_t index, uint8_t type) {
  rm_write_varuint(buf, (uint64_t)index << 4 | (uint64_t)type);
}

void rm_write_bare_id(rm_buf* buf, rm_wid value) {
  rm_buf_append_byte(buf, value.author);
  rm_write_varuint(buf, value.counter);
}

void rm_write_id(rm_buf* buf, uint32_t index, rm_wid value) {
  rm_write_tag(buf, index, RM_TAG_ID);
  rm_write_bare_id(buf, value);
}

void rm_write_string(rm_buf* buf, const char* text) {
  size_t length = text == nullptr ? 0 : strlen(text);
  rm_write_varuint(buf, (uint64_t)length);
  rm_buf_append_byte(buf, 1); /* encoding: UTF-8 */
  rm_buf_append(buf, text, length);
}

/* Reserve the length prefix before writing the body. Keep offsets because
 * growing the buffer can move its bytes. */
rm_subblock_mark rm_buf_begin_subblock(rm_buf* buf, uint32_t index) {
  rm_write_tag(buf, index, RM_TAG_LENGTH4);
  size_t length_offset = buf->count;
  rm_write_u32(buf, 0);
  return (rm_subblock_mark){.length_offset = length_offset,
                            .body_start = buf->count};
}

/* Fill the reserved prefix only after the nested body has finished growing.
 * Its final byte count must fit the prefix. */
void rm_buf_end_subblock(rm_buf* buf, rm_subblock_mark mark) {
  if (buf == nullptr || buf->error != RM_BUF_OK) return;
  if (buf->count < mark.body_start) {
    rmi_buf_fail(buf, RM_BUF_ERR_OOM);
    return;
  }
  size_t length = buf->count - mark.body_start;
  if (!rm_length_fits_block(length)) {
    rmi_buf_fail(buf, RM_BUF_ERR_TOO_LONG);
    return;
  }
  rm_write_u32_at(buf, mark.length_offset, (uint32_t)length);
}

/* The length prefix of a subblock is four bytes wide. A body too large for it
 * is refused rather than written behind a truncated prefix: the bytes would
 * parse back as a block nobody wrote, and a writer that quietly produces a
 * different page than it was given is the one failure this one must not have.
 * Reported the way every other failure here is — on the buffer, checked once
 * at the end. */
void rm_write_block(rm_buf* buf, uint32_t index, const uint8_t* body,
                    size_t length) {
  if (!rm_length_fits_block(length)) {
    rmi_buf_fail(buf, RM_BUF_ERR_TOO_LONG);
    return;
  }
  rm_write_tag(buf, index, RM_TAG_LENGTH4);
  rm_write_u32(buf, (uint32_t)length);
  rm_buf_append(buf, body, length);
}

void rm_write_stamped_u8(rm_buf* buf, uint32_t index, rm_wid stamp,
                         uint8_t value) {
  rm_subblock_mark mark = rm_buf_begin_subblock(buf, index);
  rm_write_id(buf, 1, stamp);
  rm_write_tag(buf, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(buf, value);
  rm_buf_end_subblock(buf, mark);
}

void rm_write_stamped_f32(rm_buf* buf, uint32_t index, rm_wid stamp,
                          float value) {
  rm_subblock_mark mark = rm_buf_begin_subblock(buf, index);
  rm_write_id(buf, 1, stamp);
  rm_write_tag(buf, 2, RM_TAG_BYTE4);
  rm_write_f32(buf, value);
  rm_buf_end_subblock(buf, mark);
}

void rm_write_stamped_block(rm_buf* buf, uint32_t index, rm_wid stamp,
                            const uint8_t* value, size_t length) {
  rm_subblock_mark mark = rm_buf_begin_subblock(buf, index);
  rm_write_id(buf, 1, stamp);
  rm_write_block(buf, 2, value, length);
  rm_buf_end_subblock(buf, mark);
}

void rm_write_top_block(rm_buf* file, uint8_t type, uint8_t min_version,
                        uint8_t current_version, const uint8_t* payload,
                        size_t length) {
  /* The same four-byte prefix, and the same refusal. */
  if (!rm_length_fits_block(length)) {
    rmi_buf_fail(file, RM_BUF_ERR_TOO_LONG);
    return;
  }
  rm_write_u32(file, (uint32_t)length);
  rm_buf_append_byte(file, 0);
  rm_buf_append_byte(file, min_version);
  rm_buf_append_byte(file, current_version);
  rm_buf_append_byte(file, type);
  rm_buf_append(file, payload, length);
}

void rmi_write_text_settings_width(rm_buf* out, rm_wid stamp, float width) {
  rm_subblock_mark settings = rm_buf_begin_subblock(out, 5);
  rm_write_stamped_f32(out, 1, stamp, width);
  rm_buf_end_subblock(out, settings);
}

static size_t tag_header_size(uint32_t index, uint8_t type) {
  return rmi_varuint_size(((uint64_t)index << 4) | (uint64_t)type);
}

static size_t tagged_id_size(uint32_t index, rm_wid id) {
  return tag_header_size(index, RM_TAG_ID) + 1u + rmi_varuint_size(id.counter);
}

/* Check the points, their containing value and the whole item separately.
 * Each has a four-byte length, and the identifiers in the preamble can take
 * different numbers of bytes. */
bool rmi_line_item_wire_fits(const rm_draft_stroke* stroke, rm_wid parent,
                             rm_wid id, rm_wid left, rm_wid right) {
  if (stroke == nullptr) return false;
  size_t limit = rmi_block_limit();
  if (stroke->point_count > SIZE_MAX / 14u) return false;
  size_t points_len = stroke->point_count * 14u;
  if (points_len > limit) return false;

  bool emit_argb = stroke->has_argb && stroke->color == RM_COLOR_HIGHLIGHT;
  size_t value_fixed = 1u + (tag_header_size(1, RM_TAG_BYTE4) + 4u) +
                       (tag_header_size(2, RM_TAG_BYTE4) + 4u) +
                       (tag_header_size(3, RM_TAG_BYTE8) + 8u) +
                       (tag_header_size(4, RM_TAG_BYTE4) + 4u) +
                       (tag_header_size(5, RM_TAG_LENGTH4) + 4u) +
                       (emit_argb ? tagged_id_size(6, RM_CRDT_NONE) +
                                        tag_header_size(8, RM_TAG_BYTE4) + 4u
                                  : 0u);
  if (limit < value_fixed || points_len > limit - value_fixed) return false;
  size_t value_len = value_fixed + points_len;

  size_t body_overhead = tagged_id_size(1, parent) + tagged_id_size(2, id) +
                         tagged_id_size(3, left) + tagged_id_size(4, right) +
                         tag_header_size(5, RM_TAG_BYTE4) + 4u +
                         tag_header_size(6, RM_TAG_LENGTH4) + 4u;
  if (limit < body_overhead || value_len > limit - body_overhead) return false;
  return true;
}

/* `0x05` SceneLineItem: the preamble and the stroke itself.
 *
 * One writer for both kinds of stroke — the anchored ones differ only in
 * which node they name as their parent. */
void rm_write_line_item(rm_buf* out, const rm_draft_stroke* stroke,
                        rm_wid parent, rm_wid id, rm_wid previous,
                        rm_wid right) {
  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, id);
  rm_write_id(out, 3, previous);
  rm_write_id(out, 4, right);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);

  rm_subblock_mark value = rm_buf_begin_subblock(out, 6);
  rm_buf_append_byte(out, RM_VALUE_LINE_ITEM);
  rm_write_tag(out, 1, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)stroke->tool);
  rm_write_tag(out, 2, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)stroke->color);
  rm_write_tag(out, 3, RM_TAG_BYTE8);
  rm_write_f64(out, stroke->thickness);
  rm_write_tag(out, 4, RM_TAG_BYTE4);
  rm_write_f32(out, 0);

  /* The four numbers beside the coordinates, in version 2's order — speed,
   * **width**, direction, pressure — and not version 1's, which is speed,
   * direction, width, pressure: swapped, the strokes come out the right shape
   * and the wrong thickness. */
  rm_subblock_mark points = rm_buf_begin_subblock(out, 5);
  for (size_t i = 0; i < stroke->point_count; i += 1) {
    const rm_draft_point* point = &stroke->points[i];
    rm_write_f32(out, point->x);
    rm_write_f32(out, point->y);
    rm_write_u16(out, point->speed);
    rm_write_u16(out, point->width);
    rm_buf_append_byte(out, point->direction);
    rm_buf_append_byte(out, point->pressure);
  }
  rm_buf_end_subblock(out, points);

  /* The palette's custom-colour slot carries the colour in tag 8. The writer
   * puts an empty timestamp at tag 6 ahead of it and omits the move
   * identifier at tag 7. */
  if (stroke->has_argb && stroke->color == RM_COLOR_HIGHLIGHT) {
    rm_write_id(out, 6, RM_CRDT_NONE);
    rm_write_tag(out, 8, RM_TAG_BYTE4);
    rm_write_u32(out, stroke->argb);
  }
  rm_buf_end_subblock(out, value);
}

size_t rm_reader_remaining(const rm_reader* reader) {
  return reader->size - reader->offset;
}

bool rm_fits_within(const rm_reader* reader, size_t length, size_t limit,
                    size_t* out_end) {
  if (reader->offset > limit || length > limit - reader->offset) return false;
  *out_end = reader->offset + length;
  return true;
}
