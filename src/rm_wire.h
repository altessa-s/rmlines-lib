/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Wire-format primitives, block/value constants, bounded binary
 * readers/writers, and generic tag skipping. */

#ifndef RM_WIRE_H
#define RM_WIRE_H

#include <limits.h>

#include "rm_support.h"

/* ------------------------------------------------------------------ */
/* The parser and editor use the same tag and block numbers when walking the
 * page. */
enum rm_tag_type {
  RM_TAG_ID = 0xF,
  RM_TAG_LENGTH4 = 0xC,
  RM_TAG_BYTE8 = 0x8,
  RM_TAG_BYTE4 = 0x4,
  RM_TAG_BYTE1 = 0x1,
};

constexpr uint8_t RM_BLOCK_MIGRATION_INFO = 0x00;

/* The tree itself, the order of a node's children, and the tombstones an undo
 * leaves. The three carry the layers: 0x01 says which nodes exist, 0x04 links
 * a child group into its parent's list, and 0x08 is a position-holding
 * tombstone like an erased stroke's. */
constexpr uint8_t RM_BLOCK_SCENE_TREE = 0x01;

/* A tree node carries a layer's properties. The page's typed text has its own
 * block, RootText (0x07). */
constexpr uint8_t RM_BLOCK_TREE_NODE = 0x02;
constexpr uint8_t RM_BLOCK_SCENE_GLYPH_ITEM = 0x03;
constexpr uint8_t RM_BLOCK_SCENE_GROUP_ITEM = 0x04;
constexpr uint8_t RM_BLOCK_SCENE_LINE_ITEM = 0x05;
constexpr uint8_t RM_BLOCK_SCENE_TEXT_ITEM = 0x06;
constexpr uint8_t RM_BLOCK_ROOT_TEXT = 0x07;
constexpr uint8_t RM_BLOCK_UNDO_TOMBSTONE = 0x08;
constexpr uint8_t RM_BLOCK_AUTHOR_IDS = 0x09;
constexpr uint8_t RM_BLOCK_PAGE_INFO = 0x0A;
constexpr uint8_t RM_BLOCK_SCENE_INFO = 0x0D;

/* The attachments: 0x0E maps a 16-byte key to a file name in the document's
 * index, 0x0F places the image on the page by four vertices. */
constexpr uint8_t RM_BLOCK_ATTACHMENT_MAP = 0x0E;
constexpr uint8_t RM_BLOCK_IMAGE_ITEM = 0x0F;

constexpr uint8_t RM_VALUE_GLYPH_ITEM = 0x01;
constexpr uint8_t RM_VALUE_GROUP_ITEM = 0x02;

/* The value type inside a stroke block (`RM_BLOCK_SCENE_LINE_ITEM`). */
constexpr uint8_t RM_VALUE_LINE_ITEM = 0x03;
constexpr uint8_t RM_VALUE_IMAGE_ITEM = 0x07;

/* ------------------------------------------------------------------ */
/* Error helper                                                        */
/* ------------------------------------------------------------------ */

/* A wire length fits in 32 unsigned bits, while rm_error.value is a signed
 * long even on 64-bit Windows. */
static inline long rmi_error_size(size_t value) {
  return value > LONG_MAX ? LONG_MAX : (long)value;
}

/* Keep the format version and the numeric detail separately. A caller
 * reporting an older page can then name the generation it actually read. */
void rmi_error_set(rm_error* out_error, rm_status status, int version,
                   long value, const char* message);

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

/* Low-level reading of the `.rm` format: little-endian numbers, varuints,
 * tags and subblocks. The bytes are read in place and never copied: a
 * notebook is parsed page by page in a loop, and a copy would be a per-page
 * allocation for nothing. */
typedef struct rm_reader {
  const uint8_t* data;
  size_t size;
  size_t offset;
} rm_reader;

/* Ready a reader positioned at the start of `data`. */
void rm_reader_init(rm_reader* reader, const uint8_t* data, size_t size);

/* How many bytes stand between the position and the reader's own end. */
size_t rm_reader_remaining(const rm_reader* reader);

/* Where `length` bytes starting at the position end, when they fit inside
 * `limit`, and false when they do not.
 *
 * The sum is formed only once it is known to fit. Written the other way round
 * — add, then compare — an offset and a length that are each in range can
 * still carry past what a `size_t` holds on a 32-bit machine, and the wrapped
 * end passes every bound below it. Where such an end is seeked to, it walks
 * the reader backwards, and the loop that reads fields until the block ends
 * does not terminate. */
bool rm_fits_within(const rm_reader* reader, size_t length, size_t limit,
                    size_t* out_end);

/* Move the position to `position`. RM_ERR_UNEXPECTED_EOF when it lies past
 * the reader's own end. */
[[nodiscard]] rm_status rm_read_seek(rm_reader* reader, size_t position);
[[nodiscard]] rm_status rm_read_u8(rm_reader* reader, uint8_t* out);
[[nodiscard]] rm_status rm_read_u16(rm_reader* reader, uint16_t* out);
[[nodiscard]] rm_status rm_read_u32(rm_reader* reader, uint32_t* out);

/* Four little-endian bytes, as the float32 they are the bit pattern of. */
[[nodiscard]] rm_status rm_read_f32(rm_reader* reader, float* out);

/* Eight little-endian bytes, as the float64 they are the bit pattern of. */
[[nodiscard]] rm_status rm_read_f64(rm_reader* reader, double* out);

/* `out` points into the source bytes and lives as long as they do. */
[[nodiscard]] rm_status rm_read_bytes(rm_reader* reader, size_t length,
                                      const uint8_t** out);

/* Read a length4 body after its tag. The child cannot read past that body;
 * the parent advances to its end, including any padding the child ignores. */
[[nodiscard]] rm_status rm_read_subblock(rm_reader* reader, rm_reader* out);

/* The envelope every top-level block comes in: the payload's length, an
 * unknown byte, the two versions and the type. */
typedef struct rm_block_header {
  size_t start;
  size_t body;
  size_t end;
  uint8_t unknown;
  uint8_t min_version;
  uint8_t current_version;
  uint8_t type;
} rm_block_header;

/* Read one envelope and leave the reader at the payload. A payload that would
 * run past the reader's end is RM_ERR_UNEXPECTED_EOF: one reading of the
 * envelope for the parser and the editor both, so the editor can never walk a
 * page differently from the parse that accepted it. */
[[nodiscard]] rm_status rm_read_block_header(rm_reader* reader,
                                             rm_block_header* out);

/* A variable-length integer: 7 bits per byte, the top bit meaning "more to
 * come". A number too wide for 64 bits is refused rather than quietly
 * rewritten into a different one. */
[[nodiscard]] rm_status rm_read_varuint(rm_reader* reader, uint64_t* out);

/* A count the file declares, as a `size_t`. A varuint is 64 bits wide and a
 * count is an index into memory; a value above what the machine can address
 * is an error rather than a wrap. */
[[nodiscard]] rm_status rm_read_count(rm_reader* reader, size_t* out);

/* A tag: a varuint whose upper bits are the field index and lower bits the
 * value type. */
[[nodiscard]] rm_status rm_read_tag(rm_reader* reader, uint32_t* out_index,
                                    uint8_t* out_type);

/* Check that the expected tag comes next without moving the position on a
 * mismatch: the format has optional fields.
 *
 * A question, not a read: a tag that is not there, and bytes that are not a
 * tag at all, are both answered false with the position restored. */
bool rm_match_tag(rm_reader* reader, uint32_t index, uint8_t type);

/* Require the next tag and consume it on a match. Return
 * RM_ERR_MALFORMED_PAGE when the field is absent. */
[[nodiscard]] rm_status rm_expect_tag(rm_reader* reader, uint32_t index,
                                      uint8_t type);

/* A CRDT identifier: a byte plus a varuint. We have no use for most of them,
 * but skipping one correctly is essential — otherwise the whole parse slides
 * out of place. */
[[nodiscard]] rm_status rm_read_crdt(rm_reader* reader, uint32_t index,
                                     rm_crdt_id* out);

/* Like `rm_read_crdt`, the value thrown away. */
[[nodiscard]] rm_status rm_skip_crdt(rm_reader* reader, uint32_t index);

/* An identifier written with no tag in front of it. The list of paragraph
 * styles is keyed that way: the key stands bare, and only the fields after it
 * carry tags. */
[[nodiscard]] rm_status rm_read_bare_crdt(rm_reader* reader, rm_crdt_id* out);

/* A string as the format spells one: a length in **bytes**, a one-byte
 * encoding marker, then the bytes themselves.
 *
 * The length is a byte count and not a count of characters, whatever the
 * field is called elsewhere: outside ASCII the two are different numbers. In
 * one place because it is read in several — the typed text, a highlight's
 * words, an attachment's file name, a layer's name — and every copy of this
 * would be one more chance to get that wrong in one of them.
 *
 * `end` bounds the read. The length in front of the bytes is the file's word
 * and not a measurement, so it is clamped to what the field it sits in can
 * actually hold. Nothing left to read is an empty string and not an error —
 * an empty record is how the format fences a bold stretch.
 *
 * The result is copied into `arena` with ill-formed UTF-8 repaired, and the
 * position is left where the bytes ended. */
[[nodiscard]] rm_status rm_read_string(rm_reader* reader, size_t end,
                                       rm_arena* arena, rm_str* out);

/* Skip only values whose wire type determines their size. A returned subblock
 * is bounded to its own payload. */
[[nodiscard]] rm_status rmi_skip_tagged_value(rm_reader* reader, uint8_t type,
                                              rm_reader* out_subblock);

/* The envelope every scene item starts with: the parent node, the item's own
 * identifier, its neighbours in the parent's list, and the count of what was
 * deleted here. The deleted count is the last four bytes it reads. */
typedef struct rm_item_preamble {
  rm_crdt_id parent;
  rm_crdt_id id;
  rm_crdt_id left, right;
  uint32_t deleted;
} rm_item_preamble;

[[nodiscard]] rm_status rm_parse_item_preamble(rm_reader* reader,
                                               rm_item_preamble* out);

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* An identifier as the writer hands one out. */
typedef rm_crdt_id rm_wid;

/* A nested length is filled in after the body has been written. Offsets
 * survive a buffer reallocation. */
typedef struct rm_subblock_mark {
  size_t length_offset;
  size_t body_start;
} rm_subblock_mark;

size_t rmi_block_limit(void);
#ifdef RMLINES_TESTING
void rm_test_block_limit(size_t limit);
#endif
size_t rmi_varuint_size(uint64_t value);

void rm_write_u16(rm_buf* buf, uint16_t value);

/* Four bytes, little-endian. */
void rm_write_u32(rm_buf* buf, uint32_t value);

/* The float32's bit pattern, little-endian. */
void rm_write_f32(rm_buf* buf, float value);

/* The float64's bit pattern, little-endian. */
void rm_write_f64(rm_buf* buf, double value);

/* A varuint: 7 bits per byte, the top bit saying "more to come". */
void rm_write_varuint(rm_buf* buf, uint64_t value);

/* A tag: the field index in the upper bits, the value type in the lower. */
void rm_write_tag(rm_buf* buf, uint32_t index, uint8_t type);

/* A tagged identifier: the tag, then the author and counter. */
void rm_write_id(rm_buf* buf, uint32_t index, rm_wid value);

/* An identifier with no tag in front of it: the author, then the counter. */
void rm_write_bare_id(rm_buf* buf, rm_wid value);

/* A string as the format spells one: the length in bytes, the encoding
 * marker, then the bytes. */
void rm_write_string(rm_buf* buf, const char* text);
rm_subblock_mark rm_buf_begin_subblock(rm_buf* buf, uint32_t index);
void rm_buf_end_subblock(rm_buf* buf, rm_subblock_mark mark);

/* A tagged subblock: the tag, the length of what follows, and the bytes. */
void rm_write_block(rm_buf* buf, uint32_t index, const uint8_t* body,
                    size_t length);

/* A last-writer-wins field: who set the value, then the value.
 *
 * A typed page carries six of them, in three shapes — a byte, a float, a
 * subblock — and only the shape of the value differs. Written out each time,
 * the stamp would be the field easiest to leave at the wrong id. */
void rm_write_stamped_u8(rm_buf* buf, uint32_t index, rm_wid stamp,
                         uint8_t value);
void rm_write_stamped_f32(rm_buf* buf, uint32_t index, rm_wid stamp,
                          float value);
void rm_write_stamped_block(rm_buf* buf, uint32_t index, rm_wid stamp,
                            const uint8_t* body, size_t length);

/* An eight-byte header followed by the payload. The caller supplies both
 * versions because a block's minimum can differ from the version that wrote
 * it. */
void rm_write_top_block(rm_buf* file, uint8_t type, uint8_t min_version,
                        uint8_t current_version, const uint8_t* payload,
                        size_t length);

#endif /* RM_WIRE_H */
