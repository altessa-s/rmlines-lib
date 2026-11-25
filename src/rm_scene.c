/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Parsing a notebook page (the `.rm` format, version 6).
 *
 * The strokes, the text, the glyphs, the scene tree and the attachments each
 * have a parser of their own. What goes unread is counted in
 * `rm_page.skipped_blocks`: a block of a type this parser does not know, and
 * a block of a known type that would not read. The service blocks every page
 * opens with hold nothing to draw and are stepped over uncounted — counting
 * them would put three on every page ever written and leave a real loss
 * indistinguishable from the floor. */

#include "rm_scene.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_log_internal.h"
#include "rmlines/parser.h"

#ifdef RMLINES_TESTING
static _Thread_local size_t rm_test_scene_reads = 0;

void rm_test_reset_scene_block_reads(void) { rm_test_scene_reads = 0; }

size_t rm_test_scene_block_reads(void) { return rm_test_scene_reads; }
#endif

/* ------------------------------------------------------------------ */
/* Format constants & block registry                                   */
/* ------------------------------------------------------------------ */

/* The scene info fields: whether the template and the imported document are
 * shown, and the sheet size. */
constexpr uint32_t TAG_BACKGROUND_VISIBLE = 2;
constexpr uint32_t TAG_ROOT_DOCUMENT_VISIBLE = 3;
constexpr uint32_t TAG_PAPER_SIZE = 5;

/* The same sheet as tag 5, unrounded — two float64. */
constexpr uint32_t TAG_CANVAS_SIZE = 7;
constexpr uint32_t TAG_STAMPED_SHEET_SIZE = 8;

/* Field indexes of a tree node: the visibility, the anchor, and the
 * horizontal the anchored handwriting counts its x from. */
constexpr uint32_t TAG_NODE_NAME = 2;
constexpr uint32_t TAG_VISIBILITY = 3;
constexpr uint32_t TAG_ANCHOR = 7;
constexpr uint32_t TAG_ANCHOR_ORIGIN = 10;

/* Fields of the root text block. */
constexpr uint32_t TAG_TEXT_ITEMS = 2;
constexpr uint32_t TAG_TEXT_POSITION = 3;

/* The width of the text column, declared outright. `byte4` by its tag and a
 * **float32** by its contents: 0x44900000 is 1152.0, and read as a uint32 the
 * same bytes are a billion and change. */
constexpr uint32_t TAG_COLUMN_WIDTH = 4;

/* The text's settings, whose `tag 1` is the same width once more, stamped —
 * the one that takes precedence. */
constexpr uint32_t TAG_TEXT_SETTINGS = 5;

/* The field a text record carries when it is not text at all but a marker:
 * the start or the end of a bold or an italic stretch or of a link, or an
 * image in the flow. */
constexpr uint32_t TAG_MARKER = 2;

/* The marker codes of a text record that are not faces. */
constexpr uint32_t MARK_LINK_START = 5;
constexpr uint32_t MARK_LINK_END = 6;
constexpr uint32_t MARK_INLINE_IMAGE = 0xFFFF'FFFFu;

/* What stands in the text for an image in its flow — the character the
 * tablet's own layout puts there, U+FFFC, in UTF-8. */
static const char OBJECT_REPLACEMENT[] = "\xEF\xBF\xBC";

/* The identifiers the format reserves for "before the text" and "after the
 * text". */
constexpr uint64_t ANCHOR_START_SENTINEL = 0xFFFF'FFFF'FFFEu;
constexpr uint64_t ANCHOR_END_SENTINEL = 0xFFFF'FFFF'FFFFu;

/* Compare the block's minimum reader version with the version understood
 * here. Sequence blocks also share the preamble that names the item's
 * neighbours. */
typedef struct rmi_block_spec {
  uint8_t supported_version;
  bool is_sequence;
} rmi_block_spec;

static const rmi_block_spec BLOCK_SPECS[] = {
    [RM_BLOCK_MIGRATION_INFO] = {1, false},
    [RM_BLOCK_SCENE_TREE] = {1, false},
    [RM_BLOCK_TREE_NODE] = {2, false},
    [RM_BLOCK_SCENE_GLYPH_ITEM] = {1, true},
    [RM_BLOCK_SCENE_GROUP_ITEM] = {1, true},
    [RM_BLOCK_SCENE_LINE_ITEM] = {2, true},
    [RM_BLOCK_SCENE_TEXT_ITEM] = {2, true},
    [RM_BLOCK_ROOT_TEXT] = {2, false},
    [RM_BLOCK_UNDO_TOMBSTONE] = {1, true},
    [RM_BLOCK_AUTHOR_IDS] = {1, false},
    [RM_BLOCK_PAGE_INFO] = {1, false},
    [RM_BLOCK_SCENE_INFO] = {1, false},
    [RM_BLOCK_ATTACHMENT_MAP] = {3, false},
    [RM_BLOCK_IMAGE_ITEM] = {2, true},
};

uint8_t rmi_block_supported_version(uint8_t block_type) {
  if (block_type >= sizeof BLOCK_SPECS / sizeof *BLOCK_SPECS) return 0;
  return BLOCK_SPECS[block_type].supported_version;
}

bool rmi_block_is_sequence(uint8_t block_type) {
  if (block_type >= sizeof BLOCK_SPECS / sizeof *BLOCK_SPECS) return false;
  return BLOCK_SPECS[block_type].is_sequence;
}

/* ------------------------------------------------------------------ */
/* Failure diagnostics                                                 */
/* ------------------------------------------------------------------ */

/* The block's failure, kept for the log when it is the first. The detail the
 * block parser left behind is taken along and cleared, so the next block
 * starts without one. */
static void ctx_note_failure(rmi_scene* ctx, rm_status status) {
  if (ctx->first_failure[0] == '\0') {
    if (ctx->detail[0] != '\0') {
      snprintf(ctx->first_failure, sizeof ctx->first_failure, "%s (%s)",
               rm_status_string(status), ctx->detail);
    } else {
      snprintf(ctx->first_failure, sizeof ctx->first_failure, "%s",
               rm_status_string(status));
    }
  }
  ctx->detail[0] = '\0';
}

/* A refusal with its reason: the reason is what the log line says, and "a
 * block would not read" alone sends whoever reads it to the bytes. */
static rm_status malformed(rmi_scene* ctx, const char* format, ...)
    RM_PRINTF(2, 3);

static rm_status malformed(rmi_scene* ctx, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(ctx->detail, sizeof ctx->detail, format, arguments);
  va_end(arguments);
  return RM_ERR_MALFORMED_PAGE;
}

/* Keep the preamble when there is nothing to draw. Neighbours can still name
 * this position, and the deleted length says whether anything was erased. */
static rmi_scene_item item_tombstone(const rm_item_preamble* preamble) {
  return (rmi_scene_item){
      .parent = preamble->parent,
      .id = preamble->id,
      .left = preamble->left,
      .right = preamble->right,
      .deleted = preamble->deleted,
      .kind = ITEM_TOMBSTONE,
  };
}

/* ------------------------------------------------------------------ */
/* Strokes                                                             */
/* ------------------------------------------------------------------ */

/* One v1 point is six float32 values (24 bytes). A v2 point packs the same
 * coordinates and pen data into 14 bytes. */
static rm_status point_size(uint8_t version, size_t* out) {
  switch (version) {
    case 1:
      *out = 0x18;
      return RM_OK;
    case 2:
      *out = 0x0E;
      return RM_OK;
    default:
      return RM_ERR_UNSUPPORTED_FORMAT;
  }
}

/* After the coordinates, version one stores speed, direction, width and
 * pressure, each a float32; version two stores speed and width as uint16 and
 * direction and pressure as bytes, in the order speed, width, direction,
 * pressure. */
static rm_status parse_point(rm_reader* reader, uint8_t version,
                             rm_stroke_point* out) {
  float x, y;
  RM_TRY(rm_read_f32(reader, &x));
  RM_TRY(rm_read_f32(reader, &y));

  /* A coordinate that is not a number is not a coordinate. It travels through
   * every geometry type without a complaint of any kind and comes out in the
   * sheet's own size, where a NaN width is a PDF no reader opens; worse,
   * whether it is noticed at all depends on where in the stroke it sits,
   * because `min` and `max` keep the argument they are given first. Refused
   * here, at the one door every point comes through. The stroke is lost whole
   * rather than mended: joining the neighbours of a dropped point draws a
   * line the user never made, and the caller already counts an unreadable
   * stroke as the loss it is. */
  if (!isfinite(x) || !isfinite(y)) return RM_ERR_MALFORMED_PAGE;

  if (version == 1) {
    float speed, ignored, width, pressure;
    RM_TRY(rm_read_f32(reader, &speed));
    RM_TRY(rm_read_f32(reader, &ignored));
    RM_TRY(rm_read_f32(reader, &width));
    RM_TRY(rm_read_f32(reader, &pressure));

    /* The same door for the width and the pressure: a NaN here is a NaN pen,
     * and it travels as far as a NaN coordinate does. Checked after the
     * scaling, because a finite float near the top overflows by it. */
    float scaled_width = width * 4.0f;
    float scaled_pressure = pressure * 255.0f;
    if (!isfinite(scaled_width) || !isfinite(scaled_pressure))
      return RM_ERR_MALFORMED_PAGE;

    /* Speed is scaled as the width is, by analogy: the scale of a version-one
     * speed is not known. Not a door: nothing is drawn by a speed, so the
     * stroke is not lost for one that is not a number — it reads as nought
     * instead, because a NaN or an infinity here reaches `rmdump` as a number
     * no JSON reader takes. Checked after the scaling, because a finite float
     * near the top overflows by it. */
    float scaled_speed = speed * 4.0f;
    *out = (rm_stroke_point){
        .x = x,
        .y = y,
        .width = scaled_width,
        .pressure = scaled_pressure,
        .speed = isfinite(scaled_speed) ? scaled_speed : 0.0f};
    return RM_OK;
  }

  uint16_t speed, width;
  uint8_t direction, pressure;
  RM_TRY(rm_read_u16(reader, &speed));
  RM_TRY(rm_read_u16(reader, &width));
  RM_TRY(rm_read_u8(reader, &direction));
  RM_TRY(rm_read_u8(reader, &pressure));
  *out = (rm_stroke_point){.x = x,
                           .y = y,
                           .width = (float)width,
                           .pressure = (float)pressure,
                           .speed = (float)speed};
  return RM_OK;
}

/* The tool the file's raw number names, or RM_TOOL_UNKNOWN for a number this
 * library has not seen. */
static rm_pen_tool tool_of(uint32_t raw) {
  switch (raw) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 21:
    case 22:
    case 23:
      return (rm_pen_tool)raw;
    default:
      return RM_TOOL_UNKNOWN;
  }
}

/* The palette index the file's raw number names, or `fallback` when it is
 * past the end of the palette. */
static rm_pen_color color_of(uint32_t raw, rm_pen_color fallback) {
  return raw <= (uint32_t)RM_COLOR_HIGHLIGHTER_GRAY ? (rm_pen_color)raw
                                                    : fallback;
}

/* The value declares its own length. Take a reader bounded by the stroke
 * block before trusting it: bounding the points by the declaration alone
 * would leave a stroke free to claim gigabytes. */
static rm_status parse_line_item(rmi_scene* ctx, rm_reader* reader,
                                 uint8_t point_version, rmi_scene_item* out) {
  /* The parent is a node of the tree. For handwriting anchored to text it is
   * what shows that its coordinates are counted from the text. */
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));

  /* Even a line with no value has a place in the chain. Later strokes may
   * name it as a neighbour. Only a positive deleted length counts as an
   * erasure. */
  *out = item_tombstone(&preamble);

  /* No value means the item was deleted, and that is an everyday matter: on
   * a long-edited page there is more erased than left. It must not be
   * treated as an error, otherwise the erased looks lost. */
  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) return RM_OK;

  rm_reader value;
  if (rm_read_subblock(reader, &value) != RM_OK) {
    return malformed(ctx, "a line item's value runs past its block");
  }
  if (preamble.deleted != 0) return RM_OK;
  reader = &value;

  uint8_t item_type;
  RM_TRY(rm_read_u8(reader, &item_type));
  if (item_type != RM_VALUE_LINE_ITEM) return RM_ERR_MALFORMED_PAGE;

  uint32_t tool_id, color_id;
  double thickness_scale;
  float ignored_f32;
  RM_TRY(rm_expect_tag(reader, 1, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &tool_id));
  RM_TRY(rm_expect_tag(reader, 2, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &color_id));
  RM_TRY(rm_expect_tag(reader, 3, RM_TAG_BYTE8));
  RM_TRY(rm_read_f64(reader, &thickness_scale));

  /* Refused with the stroke, as a coordinate is: the setting travels with the
   * stroke to whatever draws it, and a NaN here reaches `rmdump` as
   * `thickness: nan`, which no JSON reader takes. */
  if (!isfinite(thickness_scale)) return RM_ERR_MALFORMED_PAGE;
  RM_TRY(rm_expect_tag(reader, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_f32(reader, &ignored_f32));

  if (!rm_match_tag(reader, 5, RM_TAG_LENGTH4)) return RM_ERR_MALFORMED_PAGE;
  uint32_t points_length;
  RM_TRY(rm_read_u32(reader, &points_length));
  size_t size;
  RM_TRY(point_size(point_version, &size));
  if (points_length % size != 0) return RM_ERR_MALFORMED_PAGE;

  /* The points have to lie inside the value they are declared in. The
   * declaration alone is the file's word, and a uint32 of them is three
   * hundred million points — five gigabytes reserved on the word of a
   * sixty-byte file. Refused rather than clamped: a stroke whose points do
   * not fit is a broken stroke, and the caller counts it as one. */
  if (points_length > rm_reader_remaining(reader)) return RM_ERR_MALFORMED_PAGE;

  size_t count = points_length / size;
  rm_stroke_point* points = nullptr;
  if (count != 0) {
    points = rm_alloc_stroke_points(ctx->arena, count);
    if (points == nullptr) return RM_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < count; i += 1) {
      RM_TRY(parse_point(reader, point_version, &points[i]));
    }
  }

  /* The tail can carry a timestamp, a move ID and an explicit colour. Read it
   * by tag: on a stroke using the custom-colour slot, tag 8 is the only
   * record of the colour. */
  bool has_argb = false;
  uint32_t argb = 0;
  bool has_source = false;
  rm_crdt_id source = RM_CRDT_NONE;
  while (rm_reader_remaining(reader) > 0) {
    size_t saved = reader->offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(reader, &tag, &type) != RM_OK) {
      reader->offset = saved;
      break;
    }

    if (tag == 8 && type == RM_TAG_BYTE4) {
      if (rm_read_u32(reader, &argb) == RM_OK) {
        has_argb = true;
      } else {
        reader->offset = saved;
        break;
      }
    } else if (tag == 9 && type == RM_TAG_ID) {
      /* Tag 9 names the stroke an eraser cut this one from. On shader strokes
       * it tells pieces of one stroke from strokes that merely cross. Read
       * the identifier wholly inside the value. */
      if (rm_read_bare_crdt(reader, &source) == RM_OK) {
        has_source = true;
      } else {
        reader->offset = saved;
        break;
      }
    } else if (rmi_skip_tagged_value(reader, type, nullptr) != RM_OK) {
      /* Leave an unreadable tail inside this value. The outer reader already
       * stands at its end. */
      reader->offset = saved;
      break;
    }
  }

  *out = (rmi_scene_item){
      .parent = preamble.parent,
      .id = preamble.id,
      .left = preamble.left,
      .right = preamble.right,
      .deleted = preamble.deleted,
      .kind = ITEM_STROKE,
      .stroke =
          {
              .tool = tool_of(tool_id),
              .color = color_of(color_id, RM_COLOR_BLACK),
              .thickness_scale = thickness_scale,
              .points = points,
              .point_count = count,
              .has_argb = has_argb,
              .argb = argb,
              .id = rm_item_id_of(preamble.id),
              .has_source_id = has_source,
              .source_id = rm_item_id_of(source),
          },
  };
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* The tree, the groups and the tombstones */
/* ------------------------------------------------------------------ */

/* A record of the scene tree. The record that creates a node names it in tag
 * 1 with tag 2 at nought; every later record is a fresh one whose tag 2 names
 * the node it moves — out of the tree (`in_tree = 0`, a real removal: the
 * device's five-layer limit counts by these) or back in. The tablet does the
 * second when typed text is moved: the handwriting anchored to it is taken
 * out and put back, and a put-back read by its record's identifier would
 * leave the node removed. A record that puts a node in names its parent in
 * tag 4, and that parent can be another than the one before.
 *
 * Everything is read inside the block — `reader` ends where the block does: a
 * parent that does not fit is a block that does not read, not a parent taken
 * from the next block's bytes. */
static rm_status parse_scene_tree(rm_reader* reader, rmi_tree_event* out) {
  rm_crdt_id record, target;
  uint8_t flag;
  RM_TRY(rm_read_crdt(reader, 1, &record));
  RM_TRY(rm_read_crdt(reader, 2, &target));
  RM_TRY(rm_expect_tag(reader, 3, RM_TAG_BYTE1));
  RM_TRY(rm_read_u8(reader, &flag));
  bool in_tree = flag != 0;
  bool names = !rm_crdt_equal(target, RM_CRDT_NONE);
  *out = (rmi_tree_event){.node = names ? target : record, .in_tree = in_tree};
  if (in_tree && rm_match_tag(reader, 4, RM_TAG_LENGTH4)) {
    rm_reader place;
    RM_TRY(rm_read_subblock(reader, &place));
    RM_TRY(rm_read_crdt(&place, 1, &out->parent));
    out->has_parent = true;
  }
  return RM_OK;
}

/* A group's place in its parent's list (0x04): the same preamble as a stroke,
 * and a value naming the child node. A tombstone names nothing — the
 * reference is thrown out with the deletion.
 *
 * A value that is there but not of that shape is refused rather than answered
 * with a tombstone: a tombstone is what the scene walk reads as "the user
 * deleted this", and a link the file still carries was not deleted, it was
 * lost — along with the place of everything the group held. */
static rm_status parse_group_item(rmi_scene* ctx, rm_reader* reader,
                                  rmi_scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);

  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) return RM_OK;

  rm_reader value;
  if (rm_read_subblock(reader, &value) != RM_OK) {
    return malformed(ctx, "a group item's value runs past its block");
  }
  if (preamble.deleted != 0) return RM_OK;
  uint8_t value_type;
  RM_TRY(rm_read_u8(&value, &value_type));
  if (value_type != RM_VALUE_GROUP_ITEM) {
    return malformed(ctx, "a group item's value is of type %u, not a group",
                     (unsigned)value_type);
  }
  rm_crdt_id node;
  if (rm_read_crdt(&value, 2, &node) != RM_OK) {
    return malformed(ctx, "a group item names no node");
  }
  out->kind = ITEM_GROUP;
  out->group = node;
  return RM_OK;
}

/* The tombstone an undo leaves (0x08): the preamble alone, no value. Read for
 * its position in the chain, nothing else. */
static rm_status parse_scene_tombstone(rm_reader* reader, rmi_scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);
  return RM_OK;
}

/* Read as much of the node as its field boundaries allow. The editor can also
 * ask for the name and the exact byte range of the winning anchor. */
rm_status rmi_parse_tree_node(rm_reader* reader, size_t end,
                              rm_arena* name_arena, rmi_tree_node* out,
                              rm_anchor_field* location) {
  *out = (rmi_tree_node){0};
  RM_TRY(rm_read_crdt(reader, 1, &out->id));

  while (reader->offset < end) {
    size_t saved = reader->offset;
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) {
      reader->offset = saved;
      return RM_OK;
    }
    switch (type) {
      case RM_TAG_LENGTH4: {
        size_t length_at = reader->offset;
        uint32_t length;
        if (rm_read_u32(reader, &length) != RM_OK) {
          reader->offset = saved;
          return RM_OK;
        }

        /* A field that runs past the block ends the node where it stands:
         * what was read before it is still the node's. */
        size_t sub_end;
        if (!rm_fits_within(reader, length, end, &sub_end)) {
          reader->offset = saved;
          return RM_OK;
        }
        rm_reader field = *reader;
        field.size = sub_end;
        reader->offset = sub_end;
        rm_reader* outer = reader;
        reader = &field;
        if (index == TAG_ANCHOR) {
          rm_crdt_id stamp = RM_CRDT_NONE;
          bool stamp_ok = !rm_match_tag(reader, 1, RM_TAG_ID) ||
                          rm_read_bare_crdt(reader, &stamp) == RM_OK;

          rm_crdt_id anchor;
          if (stamp_ok && rm_read_crdt(reader, 2, &anchor) == RM_OK &&
              !rm_stamp_newer(out->target_stamp, stamp)) {
            out->target_stamp = stamp;
            out->has_anchor = true;
            out->anchor = anchor;
            if (location)
              *location = (rm_anchor_field){out->id,   stamp,          anchor,
                                            length_at, reader->offset, sub_end};
          }
        } else if (index == TAG_ANCHOR_ORIGIN) {
          /* The stamped spelling: a last-write-wins record — the timestamp,
           * then the value. This is what the tablet writes, for this field
           * and for the three beside it; the format also allows the bare
           * spelling read below. A field lost to the difference would mean a
           * silently recomputed horizontal. */
          rm_crdt_id stamp = RM_CRDT_NONE;
          bool stamp_ok = !rm_match_tag(reader, 1, RM_TAG_ID) ||
                          rm_read_bare_crdt(reader, &stamp) == RM_OK;
          float value;

          /* Left unset when it is not a place, the way the text origin is:
           * the horizontal is optional, and a renderer without it recomputes
           * — a renderer with a NaN draws the handwriting nowhere. */
          if (stamp_ok && rm_match_tag(reader, 2, RM_TAG_BYTE4) &&
              rm_read_f32(reader, &value) == RM_OK && isfinite(value) &&
              !rm_stamp_newer(out->origin_stamp, stamp)) {
            out->origin_stamp = stamp;
            out->has_origin_x = true;
            out->origin_x = (double)value;
          }
        } else if (index == TAG_VISIBILITY) {
          /* The timestamp of the last write and then the flag. The timestamp
           * is kept: of two edits of one node's visibility, the newer wins. A
           * layer never hidden or shown carries (0, 0); every hide and every
           * show is an edit with a real identifier. */
          rm_crdt_id stamp = RM_CRDT_NONE;
          bool stamp_ok = !rm_match_tag(reader, 1, RM_TAG_ID) ||
                          rm_read_bare_crdt(reader, &stamp) == RM_OK;

          /* A flag cut short is no "hidden": only a nought read hides the
           * layer. */
          if (stamp_ok && rm_match_tag(reader, 2, RM_TAG_BYTE1)) {
            uint8_t flag;
            if (rm_read_u8(reader, &flag) == RM_OK &&
                !rm_stamp_newer(out->visibility_stamp, stamp)) {
              out->has_visible = true;
              out->visibility_stamp = stamp;
              out->visible = flag != 0;
            }
          }
        } else if (index == TAG_NODE_NAME && name_arena != nullptr) {
          rm_crdt_id stamp = RM_CRDT_NONE;

          if (rm_read_crdt(reader, 1, &stamp) == RM_OK &&
              rm_match_tag(reader, 2, RM_TAG_LENGTH4)) {
            rm_reader str_sub;
            if (rm_read_subblock(reader, &str_sub) == RM_OK &&
                !rm_stamp_newer(out->name_stamp, stamp)) {
              rm_str s = {0};
              rm_status sstatus =
                  rm_read_string(&str_sub, str_sub.size, name_arena, &s);
              if (sstatus == RM_ERR_OUT_OF_MEMORY) return RM_ERR_OUT_OF_MEMORY;
              if (sstatus == RM_OK) {
                out->has_name = true;
                out->name_stamp = stamp;
                out->name = s;
              }
            }
          }
        }

        reader = outer;
        break;
      }
      case RM_TAG_BYTE4: {
        if (index == TAG_ANCHOR_ORIGIN) {
          float value;
          if (rm_read_f32(reader, &value) != RM_OK) {
            reader->offset = saved;
            return RM_OK;
          }

          if (isfinite(value) &&
              rm_crdt_equal(out->origin_stamp, RM_CRDT_NONE)) {
            out->has_origin_x = true;
            out->origin_x = (double)value;
          }
        } else {
          uint32_t ignored;
          if (rm_read_u32(reader, &ignored) != RM_OK) {
            reader->offset = saved;
            return RM_OK;
          }
        }
        break;
      }
      case RM_TAG_BYTE8: {
        double ignored;

        if (rm_read_f64(reader, &ignored) != RM_OK) {
          reader->offset = saved;
          return RM_OK;
        }
        break;
      }
      case RM_TAG_BYTE1: {
        uint8_t ignored;

        if (rm_read_u8(reader, &ignored) != RM_OK) {
          reader->offset = saved;
          return RM_OK;
        }
        break;
      }
      case RM_TAG_ID: {
        rm_crdt_id ignored;
        if (rm_read_bare_crdt(reader, &ignored) != RM_OK) {
          reader->offset = saved;
          return RM_OK;
        }
        break;
      }
      default:
        reader->offset = saved;

        /* An unknown type — going further into the block blind is not an
         * option. */
        return RM_OK;
    }
  }
  return RM_OK;
}

/* An anchor edit needs the winning field's byte offsets as well as the
 * character it names. */
rm_status rm_read_node_anchor(rm_reader body, rm_anchor_field* anchor) {
  rmi_tree_node node;
  *anchor = (rm_anchor_field){0};
  return rmi_parse_tree_node(&body, body.size, nullptr, &node, anchor);
}

/* ------------------------------------------------------------------ */
/* Scene info                                                          */
/* ------------------------------------------------------------------ */

/* The sheet's size belongs to SceneInfo. A Paper Pro page can still say 1404
 * × 1872 in .content when zoomMode is bestFit: those dimensions are left over
 * from a manual zoom, while the actual sheet is 1620 × 2160. Prefer the
 * newest stamped dimensions, then the unrounded canvas, then whole paper
 * units. Fields of known size can be stepped over. An unreadable field ends
 * the scan and leaves the earlier measurements and visibility flags in force.
 */
static void parse_scene_info(rmi_scene* ctx, rm_reader* reader, size_t end) {
  bool has_paper = false, has_canvas = false;
  rm_size paper = {0}, canvas = {0};

  while (reader->offset < end) {
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) break;
    rm_reader field = {0};
    if (rmi_skip_tagged_value(reader, type, &field) != RM_OK) break;
    if (type != RM_TAG_LENGTH4) continue;
    size_t length = rm_reader_remaining(&field);
    rm_reader* outer = reader;
    reader = &field;

    if (index == TAG_PAPER_SIZE && length >= 8) {
      uint32_t width, height;
      if (rm_read_u32(reader, &width) == RM_OK &&
          rm_read_u32(reader, &height) == RM_OK && width > 0 && height > 0) {
        paper = (rm_size){.width = (double)width, .height = (double)height};
        has_paper = true;
      }
    } else if (index == TAG_CANVAS_SIZE || index == TAG_STAMPED_SHEET_SIZE) {
      rm_crdt_id stamp = RM_CRDT_NONE;
      rm_reader dimensions = *reader;
      if (index == TAG_STAMPED_SHEET_SIZE &&
          (rm_read_crdt(reader, 1, &stamp) != RM_OK ||
           !rm_match_tag(reader, 2, RM_TAG_LENGTH4) ||
           rm_read_subblock(reader, &dimensions) != RM_OK)) {
        reader = outer;
        continue;
      }

      double width, height;
      if (rm_read_f64(&dimensions, &width) == RM_OK &&
          rm_read_f64(&dimensions, &height) == RM_OK && isfinite(width) &&
          isfinite(height) && width > 0 && height > 0) {
        canvas = (rm_size){.width = width, .height = height};
        has_canvas = true;
        if (index == TAG_STAMPED_SHEET_SIZE &&
            !rm_stamp_newer(ctx->sheet_stamp, stamp)) {
          ctx->has_stamped_sheet = ctx->has_paper_size = true;
          ctx->sheet_stamp = stamp;
          ctx->paper_size = canvas;
        }
      }
    } else if (index == TAG_BACKGROUND_VISIBLE ||
               index == TAG_ROOT_DOCUMENT_VISIBLE) {
      /* The timestamp of the last write, then the flag — the same shape as a
       * layer's visibility in the tree node. */
      rm_crdt_id stamp = RM_CRDT_NONE;
      (void)rm_read_crdt(reader, 1, &stamp);

      rm_crdt_id* previous = index == TAG_BACKGROUND_VISIBLE
                                 ? &ctx->background_stamp
                                 : &ctx->document_stamp;
      uint8_t flag;
      if (rm_match_tag(reader, 2, RM_TAG_BYTE1) &&
          rm_read_u8(reader, &flag) == RM_OK &&
          !rm_stamp_newer(*previous, stamp)) {
        *previous = stamp;
        if (index == TAG_BACKGROUND_VISIBLE)
          ctx->background_visible = flag != 0;
        else
          ctx->root_document_visible = flag != 0;
      }
    }

    reader = outer;
  }

  if (ctx->has_stamped_sheet) return;
  if (has_canvas) {
    ctx->paper_size = canvas;
    ctx->has_paper_size = true;
    ctx->has_canvas_size = true;
  } else if (has_paper && !ctx->has_canvas_size) {
    ctx->paper_size = paper;
    ctx->has_paper_size = true;
  }
}

/* ------------------------------------------------------------------ */
/* Highlights                                                          */
/* ------------------------------------------------------------------ */

/* Step over an optional field of a value's tail when it is there and ends
 * inside the value; otherwise leave the reader where it stood. The reader is
 * bounded by the value, and the bound is the field's end, not its tag's: an
 * identifier is a byte and a varuint, and a tail cut short in the middle of
 * one must not carry the reader into the next block. */
static void skip_optional_tail(rm_reader* reader, uint32_t index,
                               uint8_t type) {
  size_t saved = reader->offset;
  if (!rm_match_tag(reader, index, type)) return;
  bool skipped = rmi_skip_tagged_value(reader, type, nullptr) == RM_OK;
  if (!skipped) reader->offset = saved;
}

/* The marker over the text of an imported PDF.
 *
 * The same item preamble as a stroke — parent, identifier, neighbours, the
 * length of what was deleted — and then a value of its own: the place in the
 * text layer, the highlighted text, the colour, and the rectangles: tag 2
 * `start`, 3 `length`, 4 the colour, 5 the text, 6 the rectangles, 10 an
 * explicit colour.
 *
 * The rectangles and the colour are what drawing needs; the text is read as
 * well, because it is the only record of *what* was highlighted short of
 * reading the PDF back — and the page under the band may be a scan with no
 * text layer to read.
 *
 * `end` is the end of the block. It is not decoration: the rectangles come
 * with a count of their own, and a count is a declaration rather than a
 * promise — reserving on it hands a sixty-byte blob the keys to gigabytes. */

/* Keep the highlight's neighbours even when it carries no rectangles. The
 * block walk distinguishes an erased mark from a missing drawing by its
 * deleted length. */
static rm_status parse_glyph_item(rmi_scene* ctx, rm_reader* reader, size_t end,
                                  rmi_scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));

  *out = item_tombstone(&preamble);

  /* Without a value, an erased mark ends immediately after its deleted
   * length. A live preamble still holds a chain position, but its missing
   * rectangles count as a loss in the block walk. */
  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    if (preamble.deleted != 0 && reader->offset != end) {
      return malformed(ctx, "unexpected data after a deleted highlight");
    }
    return RM_OK;
  }

  /* The value declares its length, and it is held to the block: what
   * follows is read inside the value and nowhere else. Its type is not
   * checked — the tablet writes 0x01, but a highlight refused on a type
   * byte spelt otherwise would vanish from the page for nothing the
   * drawing needs. */
  uint32_t ignored_u32;
  uint8_t ignored_u8;
  rm_reader value;
  if (rm_read_subblock(reader, &value) != RM_OK) {
    return malformed(ctx, "a highlight's value runs past its block");
  }

  /* A deleted length removes the highlight whatever follows it, as it removes
   * a stroke from the scene: the editor and the scene walk read it so, and a
   * highlight the page shows has to be one the editor can erase. */
  if (preamble.deleted != 0) return RM_OK;
  reader = &value;
  end = reader->size;
  RM_TRY(rm_read_u8(reader, &ignored_u8));

  /* `start` and `length` are absent on some pages, and their absence is not
   * an error: they place the highlight in the text layer, which we do not
   * need — the rectangles say where to draw. */
  if (rm_match_tag(reader, 2, RM_TAG_BYTE4))
    RM_TRY(rm_read_u32(reader, &ignored_u32));
  if (rm_match_tag(reader, 3, RM_TAG_BYTE4))
    RM_TRY(rm_read_u32(reader, &ignored_u32));

  uint32_t color_id;
  RM_TRY(rm_expect_tag(reader, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(reader, &color_id));

  /* The reader is put at the field's end whatever its length claimed, so a
   * short or padded field cannot shift the rectangles after it. */
  bool has_text = false;
  rm_str text = {.bytes = "", .length = 0};
  if (rm_match_tag(reader, 5, RM_TAG_LENGTH4)) {
    uint32_t text_length;
    RM_TRY(rm_read_u32(reader, &text_length));
    size_t text_end;
    if (!rm_fits_within(reader, text_length, end, &text_end)) text_end = end;
    rm_str decoded;
    RM_TRY(rm_read_string(reader, text_end, ctx->arena, &decoded));
    if (decoded.length != 0) {
      has_text = true;
      text = decoded;
    }
    RM_TRY(rm_read_seek(reader, text_end));
  }

  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) {
    return malformed(ctx, "a highlight has no rectangles");
  }
  rm_reader rectangle_data;
  if (rm_read_subblock(reader, &rectangle_data) != RM_OK) {
    return malformed(ctx, "a highlight's rectangles run past its value");
  }
  size_t count;
  RM_TRY(rm_read_count(&rectangle_data, &count));

  /* Four float64 per rectangle, all inside the rectangle list. Refuse the
   * count before allocating or reading any following field as coordinates. */
  size_t room = rm_reader_remaining(&rectangle_data) / 32u;
  if (count > room) {
    return malformed(
        ctx, "a highlight declares %zu rectangles, more than its field holds",
        count);
  }
  rm_vec gathered;
  rm_vec_init(&gathered, sizeof(rm_rect));
  if (!rm_vec_reserve(&gathered, count)) {
    rm_vec_free(&gathered);
    return RM_ERR_OUT_OF_MEMORY;
  }

  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i += 1) {
    double x, y, width, height;
    if (rm_read_f64(&rectangle_data, &x) != RM_OK ||
        rm_read_f64(&rectangle_data, &y) != RM_OK ||
        rm_read_f64(&rectangle_data, &width) != RM_OK ||
        rm_read_f64(&rectangle_data, &height) != RM_OK) {
      status = RM_ERR_UNEXPECTED_EOF;
      break;
    }

    /* The whole highlight goes, not the one rectangle: the marker is drawn as
     * a run over a line of text, and a run with a hole in it is a highlight
     * of something the user did not highlight. Refused rather than answered
     * empty so that the caller counts it. */
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height)) {
      status = RM_ERR_MALFORMED_PAGE;
      break;
    }
    rm_rect rectangle = {.x = x, .y = y, .width = width, .height = height};
    if (!rm_vec_push(&gathered, &rectangle)) {
      status = RM_ERR_OUT_OF_MEMORY;
      break;
    }
  }
  if (status != RM_OK) {
    rm_vec_free(&gathered);
    return status;
  }

  size_t written = gathered.count;
  if (written == 0) {
    rm_vec_free(&gathered);
    return malformed(ctx, "a highlight has no rectangles");
  }
  rm_rect* rectangles =
      rm_arena_alloc(ctx->arena, written * sizeof *rectangles);
  if (rectangles == nullptr) {
    rm_vec_free(&gathered);
    return RM_ERR_OUT_OF_MEMORY;
  }
  memcpy(rectangles, gathered.data, written * sizeof *rectangles);
  rm_vec_free(&gathered);

  /* The second form of a highlight: instead of `start` and `length`, the
   * first and the last character as identifiers, and whether the last one is
   * included (`first`/`last`/`includeLastId`). They stand between the
   * rectangles and the colour, and an unread one hides the colour from the
   * match below. Each is stepped over whole, and only while it lies wholly
   * inside the value. */
  skip_optional_tail(reader, 7, RM_TAG_ID);
  skip_optional_tail(reader, 8, RM_TAG_ID);
  skip_optional_tail(reader, 9, RM_TAG_BYTE1);

  bool has_argb = false;
  uint32_t argb = 0;
  if (rm_match_tag(reader, 10, RM_TAG_BYTE4)) {
    has_argb = rm_read_u32(reader, &argb) == RM_OK;
  }

  out->kind = ITEM_GLYPH;
  out->glyph = (rm_highlight){
      .rectangles = rectangles,
      .rectangle_count = written,
      .color = color_of(color_id, RM_COLOR_YELLOW),
      .has_argb = has_argb,
      .argb = argb,
      .has_text = has_text,
      .text = text,
      .id = rm_item_id_of(preamble.id),
  };
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Attachments and images                                              */
/* ------------------------------------------------------------------ */

/* One entry of the attachment map: the key and, when it carries one, the
 * name. */
static rm_status parse_attachment_entry(rmi_scene* ctx, rm_reader* row,
                                        rm_vec* out) {
  const uint8_t* key;
  RM_TRY(rm_read_bytes(row, 16, &key));
  if (!rm_match_tag(row, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader name_box;
  RM_TRY(rm_read_subblock(row, &name_box));
  rm_crdt_id stamp = RM_CRDT_NONE;
  if (rm_match_tag(&name_box, 1, RM_TAG_ID)) {
    RM_TRY(rm_read_bare_crdt(&name_box, &stamp));
  }
  if (!rm_match_tag(&name_box, 2, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader text;
  RM_TRY(rm_read_subblock(&name_box, &text));
  rm_str name;
  RM_TRY(rm_read_string(&text, text.size, ctx->arena, &name));
  if (name.length == 0) return RM_OK;
  rmi_attachment entry = {.name = name, .stamp = stamp};
  memcpy(entry.key, key, 16);
  return rm_vec_push(out, &entry) ? RM_OK : RM_ERR_OUT_OF_MEMORY;
}

/* The attachment map (0x0E): a 16-byte opaque key to a file name in the
 * document's index. One block declares every attachment of the page — it is a
 * list, not a record per asset.
 *
 * A list that runs out early — a count larger than its entries, an entry that
 * is not one, an entry past the block — keeps the names before it and is no
 * loss. An entry that fails inside costs the block's whole map: nothing of it
 * is merged, and the caller counts it. */
static rm_status parse_attachment_map(rmi_scene* ctx, rm_reader* reader) {
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader list;
  RM_TRY(rm_read_subblock(reader, &list));
  size_t count;
  RM_TRY(rm_read_count(&list, &count));

  rm_vec entries;
  rm_vec_init(&entries, sizeof(rmi_attachment));
  rm_status status = RM_OK;
  for (size_t i = 0; i < count; i += 1) {
    if (rm_reader_remaining(&list) == 0 ||
        !rm_match_tag(&list, 0, RM_TAG_LENGTH4))
      break;
    uint32_t entry_length;
    status = rm_read_u32(&list, &entry_length);
    if (status != RM_OK) goto done;
    size_t entry_end;
    if (!rm_fits_within(&list, entry_length, list.size, &entry_end)) break;
    rm_reader row = list;
    row.size = entry_end;
    status = parse_attachment_entry(ctx, &row, &entries);
    if (status != RM_OK) goto done;
    list.offset = entry_end;
  }

  /* Merge only after the entire block has been validated. */
  for (size_t i = 0; i < entries.count; i += 1) {
    const rmi_attachment* entry = rm_vec_at(&entries, i);
    uint64_t existing_at;
    if (rm_keymap_get(&ctx->attachment_index, entry->key, &existing_at)) {
      rmi_attachment* existing =
          rm_vec_at(&ctx->attachments, (size_t)existing_at);

      /* Legacy map snapshots use the untouched stamp and replace by key. */
      if (rm_crdt_equal(entry->stamp, RM_CRDT_NONE) ||
          !rm_stamp_newer(existing->stamp, entry->stamp))
        *existing = *entry;
    } else if (!rm_vec_push(&ctx->attachments, entry) ||
               !rm_keymap_set(&ctx->attachment_index, entry->key,
                              ctx->attachments.count - 1u)) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }

done:
  rm_vec_free(&entries);
  return status;
}

/* An image key can be bare or stamped. Keep its declared byte length so only
 * a full attachment key can place the picture. */
rm_status rmi_parse_image_key(rm_reader keyed, bool* out_has_stamp,
                              rm_crdt_id* out_stamp, const uint8_t** out_key,
                              size_t* out_key_len) {
  *out_has_stamp = false;
  *out_stamp = RM_CRDT_NONE;
  *out_key = nullptr;
  *out_key_len = 0;

  if (rm_reader_remaining(&keyed) == 16u) {
    *out_key_len = 16u;
    return rm_read_bytes(&keyed, 16u, out_key);
  }
  if (rm_match_tag(&keyed, 1, RM_TAG_ID)) {
    RM_TRY(rm_read_bare_crdt(&keyed, out_stamp));
    *out_has_stamp = true;
  }

  /* The key sits in a tag-2 wrapper of its own; the bare spelling,
   * which the format also allows, is read as a fallback. */
  rm_reader key_data = keyed;
  if (rm_reader_remaining(&keyed) != 16u &&
      rm_match_tag(&keyed, 2, RM_TAG_LENGTH4)) {
    RM_TRY(rm_read_subblock(&keyed, &key_data));
  }
  *out_key_len = rm_reader_remaining(&key_data);
  if (*out_key_len >= 16u) RM_TRY(rm_read_bytes(&key_data, 16u, out_key));

  return RM_OK;
}

/* An image on the page (0x0F): the same preamble as a stroke, and a value of
 * type 7 — the attachment's key and the four vertices (x, y, u, v) that place,
 * scale, crop or turn it. (The format has a second way, an image standing in
 * the text itself — a record of the root text keyed into block 0x0E with no
 * 0x0F item at all; that one is a character of the text here, not drawn.)
 *
 * Both the value and the vertex list have their own byte boundaries. A
 * placement needs exactly sixteen floats inside the declared list.
 *
 * Once the value is there, a tombstone is not an honest answer: it
 * reads as "the user erased it", and whatever goes wrong past that point was
 * not erased, it was lost. So every shape the value can be wrong in is a
 * refusal, and the caller counts it. */
static rm_status parse_image_item(rmi_scene* ctx, rm_reader* reader,
                                  rmi_scene_item* out) {
  rm_item_preamble preamble;
  RM_TRY(rm_parse_item_preamble(reader, &preamble));
  *out = item_tombstone(&preamble);

  if (!rm_match_tag(reader, 6, RM_TAG_LENGTH4)) return RM_OK;

  rm_reader value;
  if (rm_read_subblock(reader, &value) != RM_OK) {
    return malformed(ctx, "an image item's value runs past its block");
  }
  if (preamble.deleted != 0) return RM_OK;
  reader = &value;
  uint8_t value_type;
  RM_TRY(rm_read_u8(reader, &value_type));
  if (value_type != RM_VALUE_IMAGE_ITEM) {
    return malformed(ctx, "an image item's value is of type %u, not an image",
                     (unsigned)value_type);
  }

  const uint8_t* key = nullptr;
  size_t key_length = 0;
  if (rm_match_tag(reader, 1, RM_TAG_LENGTH4)) {
    rm_reader keyed;
    RM_TRY(rm_read_subblock(reader, &keyed));
    bool has_stamp = false;
    rm_crdt_id ignored_stamp = RM_CRDT_NONE;
    RM_TRY(rmi_parse_image_key(keyed, &has_stamp, &ignored_stamp, &key,
                               &key_length));
  }
  if (rm_match_tag(reader, 2, RM_TAG_ID)) {
    rm_crdt_id ignored;
    RM_TRY(rm_read_bare_crdt(reader, &ignored));
  }

  rm_image_vertex vertices[4] = {0};
  size_t floats = 0;
  if (rm_match_tag(reader, 3, RM_TAG_LENGTH4)) {
    rm_reader vertex_data;
    RM_TRY(rm_read_subblock(reader, &vertex_data));
    RM_TRY(rm_read_count(&vertex_data, &floats));
    if (floats > rm_reader_remaining(&vertex_data) / 4u) {
      return malformed(
          ctx, "an image declares %zu vertex floats, more than its list holds",
          floats);
    }

    /* Four vertices place, scale and crop the image, and there is no placing
     * it on three. Whole vertices are counted: one to three floats over the
     * sixteen are no fifth vertex. The count is held to that before a float
     * is read; every float of the list must be a number all the same. */
    if (floats / 4u == 4u) {
      for (size_t i = 0; i < floats; i += 1) {
        float number;
        RM_TRY(rm_read_f32(&vertex_data, &number));
        if (!isfinite(number)) {
          return malformed(ctx, "an image vertex is not a finite one");
        }
        if (i < 16) {
          rm_image_vertex* vertex = &vertices[i / 4u];

          switch (i % 4u) {
            case 0:
              vertex->x = number;
              break;
            case 1:
              vertex->y = number;
              break;
            case 2:
              vertex->u = number;
              break;
            default:
              vertex->v = number;
              break;
          }
        }
      }
    }
  }

  /* The key must supply sixteen bytes to look up an attachment. A longer
   * field contributes its first sixteen. The picture still needs all four
   * corners. */
  if (key == nullptr) {
    return malformed(ctx, "an image key is %zu bytes, not 16", key_length);
  }
  if (floats / 4u != 4u) {
    return malformed(ctx, "an image has %zu vertices, not 4", floats / 4u);
  }

  /* The four vertices suffice when there is no index list. With a list,
   * the tablet draws these two triangles in this order. */
  if (rm_match_tag(reader, 4, RM_TAG_LENGTH4)) {
    rm_reader index_data;
    if (rm_read_subblock(reader, &index_data) != RM_OK) {
      return malformed(ctx, "an image's index list runs past its value");
    }
    size_t idx_count = 0;
    RM_TRY(rm_read_count(&index_data, &idx_count));
    if (idx_count > rm_reader_remaining(&index_data) / 4u) {
      return malformed(
          ctx, "an image declares %zu indices, more than its list holds",
          idx_count);
    }
    if (idx_count != 6u) {
      return malformed(ctx, "an image has %zu indices, not the 6 of a quad",
                       idx_count);
    }

    static constexpr uint32_t kQuad[6] = {0, 1, 2, 2, 3, 0};
    for (size_t i = 0; i < 6u; i += 1) {
      uint32_t idx;
      RM_TRY(rm_read_u32(&index_data, &idx));
      if (idx != kQuad[i]) {
        return malformed(ctx, "an image quad index %zu is %u, not %u", i,
                         (unsigned)idx, (unsigned)kQuad[i]);
      }
    }
  }

  out->kind = ITEM_IMAGE;
  out->image_key = key;
  memcpy(out->vertices, vertices, sizeof vertices);
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Typed text                                                          */
/* ------------------------------------------------------------------ */

/* Distinguish a list that ends between rows from one that ends inside a
 * length prefix. Callers use that boundary to decide how much can be kept. */
rmi_row_envelope_status rmi_next_text_row_envelope(rm_reader* list,
                                                   size_t list_end,
                                                   size_t* out_row_start,
                                                   rm_reader* out_row) {
  rm_reader bounded = *list;
  bounded.size = list_end < list->size ? list_end : list->size;
  if (out_row_start != nullptr) *out_row_start = bounded.offset;
  if (bounded.offset >= bounded.size) return RM_ROW_EOF;
  if (!rm_match_tag(&bounded, 0, RM_TAG_LENGTH4)) return RM_ROW_STOP;
  uint32_t item_length;
  if (rm_read_u32(&bounded, &item_length) != RM_OK) return RM_ROW_SHORT_PREFIX;
  size_t item_end;
  if (!rm_fits_within(&bounded, item_length, bounded.size, &item_end))
    return RM_ROW_STOP;
  *out_row = bounded;
  out_row->size = item_end;
  list->offset = item_end;
  return RM_ROW_OK;
}

/* Drawing needs the text or marker. Editing also needs to know whether an
 * unfamiliar suffix would be lost if the row were split. */
rm_status rmi_decode_text_row_tag6(rm_arena* arena, rm_reader* row,
                                   text_item* item,
                                   bool* out_has_value_suffix) {
  if (out_has_value_suffix != nullptr) *out_has_value_suffix = false;
  item->text = (rm_str){.bytes = "", .length = 0};
  item->has_marker = false;
  item->marker = 0;
  item->opaque_span = 0;

  if (!rm_match_tag(row, 6, RM_TAG_LENGTH4)) {
    if (rm_reader_remaining(row) > 0 && out_has_value_suffix != nullptr)
      *out_has_value_suffix = true;
    return RM_OK;
  }

  /* Held to the record: a value that claims more than its record holds would
   * otherwise read the next record's bytes as its own — and a marker found
   * there would quietly turn this record's text into nothing. Clamped rather
   * than refused, as the string inside it is. */
  uint32_t value_length;
  RM_TRY(rm_read_u32(row, &value_length));
  size_t value_end;
  bool clamped = !rm_fits_within(row, value_length, row->size, &value_end);
  if (clamped) {
    value_end = row->size;
    if (out_has_value_suffix != nullptr) *out_has_value_suffix = true;
  }
  rm_reader value = *row;
  value.size = value_end;
  row->offset = value_end;
  if (rm_reader_remaining(row) > 0 && out_has_value_suffix != nullptr)
    *out_has_value_suffix = true;

  if (rm_reader_remaining(&value) == 0) return RM_OK;

  rm_reader probe = value;
  size_t declared = 0;
  RM_TRY(rm_read_count(&probe, &declared));
  if (rm_reader_remaining(&probe) == 0) {
    if (out_has_value_suffix != nullptr) *out_has_value_suffix = true;
    return RM_OK;
  }
  uint8_t encoding = 0;
  RM_TRY(rm_read_u8(&probe, &encoding));

  if (encoding == 1u) {
    if (declared > rm_reader_remaining(&probe) &&
        out_has_value_suffix != nullptr)
      *out_has_value_suffix = true;
    RM_TRY(rm_read_string(&value, value.size, arena, &item->text));

    /* A nonzero marker after the string makes this row markup. The string may
     * itself be a link address or an inline image key. Drawing it as ordinary
     * letters would lose the link or the image. Bold and italic are recorded
     * here too, even when the paragraph calls itself plain. A marker cut
     * short inside the value leaves the decoded string as text. */
    rm_reader tmp = value;
    if (rm_match_tag(&tmp, TAG_MARKER, RM_TAG_BYTE4)) {
      uint32_t code = 0;
      if (rm_read_u32(&tmp, &code) == RM_OK) {
        value = tmp;
        if (code != 0) {
          item->has_marker = true;
          item->marker = code;
        }
      } else if (out_has_value_suffix != nullptr) {
        *out_has_value_suffix = true;
      }
    }
    if (rm_reader_remaining(&value) > 0 && out_has_value_suffix != nullptr)
      *out_has_value_suffix = true;
    return RM_OK;
  }

  if (out_has_value_suffix != nullptr) *out_has_value_suffix = true;
  size_t available = rm_reader_remaining(&probe);
  size_t skip_len = declared < available ? declared : available;
  const uint8_t* unused = nullptr;
  RM_TRY(rm_read_bytes(&probe, skip_len, &unused));
  uint32_t code = 0;
  if (declared <= available && rm_match_tag(&probe, TAG_MARKER, RM_TAG_BYTE4) &&
      rm_read_u32(&probe, &code) == RM_OK && code != 0) {
    item->opaque_span = 1u;
  } else {
    item->opaque_span = skip_len > 0 ? (uint64_t)skip_len : 1u;
  }
  return RM_OK;
}

/* One record of the text, held to its own bytes. */
rm_status rmi_parse_text_row(rmi_scene* ctx, rm_reader* row, text_item* item) {
  *item = (text_item){0};
  RM_TRY(rm_read_crdt(row, 2, &item->id));
  RM_TRY(rm_read_crdt(row, 3, &item->left));
  RM_TRY(rm_read_crdt(row, 4, &item->right));
  RM_TRY(rm_expect_tag(row, 5, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(row, &item->deleted));
  item->text = (rm_str){.bytes = "", .length = 0};
  if (item->deleted != 0) return RM_OK;
  return rmi_decode_text_row_tag6(ctx->arena, row, item, nullptr);
}

/* The items lie inside a container and a counted list, one bounded row per
 * item. A missing row leaves the earlier items standing. A row that fails
 * inside costs the list. The parent reader has already stepped over the
 * container, ready to look for paragraph styles. */
static rm_status parse_text_items(rmi_scene* ctx, rm_reader* reader,
                                  rm_vec* out, bool* out_envelope_error) {
  *out_envelope_error = false;
  if (!rm_match_tag(reader, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader container;
  if (rm_read_subblock(reader, &container) != RM_OK) {
    *out_envelope_error = true;
    return RM_ERR_MALFORMED_PAGE;
  }
  if (!rm_match_tag(&container, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader list;
  if (rm_read_subblock(&container, &list) != RM_OK) {
    *out_envelope_error = true;
    return RM_ERR_MALFORMED_PAGE;
  }
  size_t count;
  RM_TRY(rm_read_count(&list, &count));

  for (size_t i = 0; i < count; i += 1) {
    rm_reader row;
    rmi_row_envelope_status env =
        rmi_next_text_row_envelope(&list, list.size, nullptr, &row);
    if (env == RM_ROW_EOF || env == RM_ROW_STOP) break;
    if (env == RM_ROW_SHORT_PREFIX) return RM_ERR_UNEXPECTED_EOF;

    text_item item;
    rm_status status = rmi_parse_text_row(ctx, &row, &item);
    if (status != RM_OK) return status;
    if (!rm_vec_push(out, &item)) return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

/* The value of one paragraph style, held to its own bytes. */
rm_status rmi_parse_style_value(rm_reader* reader,
                                rm_paragraph_format* out_format,
                                bool* out_has_format) {
  *out_has_format = false;
  *out_format = (rm_paragraph_format){0};
  if (!rm_match_tag(reader, 1, RM_TAG_BYTE1)) return RM_OK;
  rm_paragraph_format format = {0};
  RM_TRY(rm_read_u8(reader, &format.raw));

  /* The value goes on past the first field, and reading only the first would
   * run two different presentations together: an emphasised paragraph and a
   * subheading both carry 3, and apart they are told only by these. */
  if (rm_match_tag(reader, 2, RM_TAG_BYTE1)) {
    RM_TRY(rm_read_u8(reader, &format.second));
    format.has_second = true;
  }
  if (rm_match_tag(reader, 3, RM_TAG_BYTE4)) {
    RM_TRY(rm_read_u32(reader, &format.third));
    format.has_third = true;
  }
  *out_format = format;
  *out_has_format = true;
  return RM_OK;
}

static rm_status parse_style_value(rm_reader* reader, rm_crdt_id key,
                                   rm_crdt_id stamp, rm_vec* out,
                                   bool* out_decoded) {
  *out_decoded = true;
  rm_paragraph_format format;
  bool has_format = false;
  if (rmi_parse_style_value(reader, &format, &has_format) != RM_OK) {
    *out_decoded = false;
    return RM_OK;
  }
  if (!has_format) return RM_OK;
  rmi_style_entry entry = {.key = key, .stamp = stamp, .format = format};

  /* Stored as a pair, not interpreted field by field: telling the known
   * shapes apart is all the page needs. Two are known: (2, 3) on the second
   * heading, and (3, state << 8 | family << 4 | depth) on a list item —
   * families 0…2, depths 0…2, and the state bits 8 (ticked) and 9 (something
   * nested is ticked) that a checkbox item may carry, the first level
   * included. Logged so that the unknown reports itself — a third shape would
   * otherwise arrive in silence. */
  bool further = rm_paragraph_format_has_further_fields(&entry.format);

  bool heading_pair = entry.format.has_second && entry.format.second == 2 &&
                      entry.format.has_third && entry.format.third == 3;
  bool nested_list_pair = entry.format.has_second && entry.format.second == 3 &&
                          entry.format.has_third &&
                          (entry.format.third >> 10) == 0 &&
                          ((entry.format.third >> 4) & 0x0Fu) <= 2 &&
                          (entry.format.third & 0x0Fu) <= 2;
  if (further && !heading_pair && !nested_list_pair) {
    RM_LOG_DEBUGF("page: paragraph style %u carries an unseen pair (%d, %ld)",
                  (unsigned)entry.format.raw,
                  entry.format.has_second ? (int)entry.format.second : -1,
                  entry.format.has_third ? (long)entry.format.third : -1L);
  }
  if (!rm_vec_push(out, &entry)) return RM_ERR_OUT_OF_MEMORY;
  if (rm_paragraph_format_style(&entry.format) == RM_PARA_UNKNOWN) {
    /* A style we do not know is kept and drawn as plain text, and said out
     * loud, so that a new style value reports itself rather than passing for
     * plain text in silence. */
    RM_LOG_DEBUGF("render: unknown paragraph style %u",
                  (unsigned)entry.format.raw);
  }
  return RM_OK;
}

/* The style list follows the text items. Each record starts with a bare
 * character ID, then the optional write stamp and a bounded style value. A
 * list that runs out keeps the styles read so far. */
static rm_status parse_paragraph_styles(rm_reader* reader, rm_vec* out,
                                        bool* out_envelope_error) {
  *out_envelope_error = false;
  if (!rm_match_tag(reader, 2, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader container;
  if (rm_read_subblock(reader, &container) != RM_OK) {
    *out_envelope_error = true;
    return RM_ERR_MALFORMED_PAGE;
  }
  if (!rm_match_tag(&container, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader list;
  if (rm_read_subblock(&container, &list) != RM_OK) {
    *out_envelope_error = true;
    return RM_ERR_MALFORMED_PAGE;
  }
  size_t count;
  if (rm_read_count(&list, &count) != RM_OK) return RM_OK;

  for (size_t i = 0; i < count; i += 1) {
    if (rm_reader_remaining(&list) == 0) break;
    rm_crdt_id key;
    if (rm_read_bare_crdt(&list, &key) != RM_OK) break;
    rm_crdt_id stamp = RM_CRDT_NONE;
    if (rm_match_tag(&list, 1, RM_TAG_ID) &&
        rm_read_bare_crdt(&list, &stamp) != RM_OK)
      break;
    if (!rm_match_tag(&list, 2, RM_TAG_LENGTH4)) break;
    rm_reader value;
    if (rm_read_subblock(&list, &value) != RM_OK) break;

    bool decoded = false;
    rm_status status = parse_style_value(&value, key, stamp, out, &decoded);
    if (status != RM_OK) return status;
    if (!decoded) break;
  }
  return RM_OK;
}

/* A paragraph and an offset into one `uint64_t`, the value
 * `place_of_character` maps an identifier to. */
static uint64_t anchor_pack(size_t paragraph, size_t offset) {
  return (uint64_t)(uint32_t)paragraph << 32 | (uint64_t)(uint32_t)offset;
}

/* The paragraph and offset `anchor_pack` packed into `packed`. */
static rm_text_anchor anchor_unpack(uint64_t packed) {
  return (rm_text_anchor){
      .paragraph = (int32_t)(uint32_t)(packed >> 32),
      .offset = (int32_t)(uint32_t)(packed & 0xFFFF'FFFFu),
  };
}

/* Start a fresh, empty paragraph at the end of the layout. False on
 * allocation failure. */
static bool layout_push_line(rmi_text_layout* layout) {
  rmi_line_build line;
  rm_buf_init(&line.text);
  rm_vec_init(&line.faces, sizeof(uint8_t));
  return rm_vec_push(&layout->lines, &line);
}

/* Release every paragraph's text and faces, and the layout's own tables. */
static void layout_free(rmi_text_layout* layout) {
  for (size_t i = 0; i < layout->lines.count; i += 1) {
    rmi_line_build* line = rm_vec_at(&layout->lines, i);
    rm_buf_free(&line->text);
    rm_vec_free(&line->faces);
  }
  rm_vec_free(&layout->lines);
  rm_vec_free(&layout->styles);
  rm_idmap_free(&layout->place_of_character);
  rm_idmap_free(&layout->place_ambiguous);
  rm_idmap_free(&layout->style_line_of_key);
  rm_vec_free(&layout->flat_ids);
  rm_vec_free(&layout->flat_inline_image);
}

constexpr uint8_t FACE_BOLD = 1u;
constexpr uint8_t FACE_ITALIC = 2u;

/* Keep a place for drawing even if two rows claim the same character. Editing
 * must also know that an anchor to it is ambiguous. */
static bool record_place(rmi_text_layout* layout, rm_crdt_id id,
                         uint64_t packed) {
  if (!rm_crdt_equal(id, RM_CRDT_NONE) &&
      rm_idmap_contains(&layout->place_of_character, id) &&
      !rm_idmap_set(&layout->place_ambiguous, id, 1))
    return false;
  return rm_idmap_set(&layout->place_of_character, id, packed);
}

/* Collect character IDs alongside the public text only when an edit needs to
 * trace it back to the file. */
static bool record_flat_scalar(rmi_text_layout* layout, bool collect_map,
                               rm_crdt_id id, bool inline_image) {
  if (!collect_map) return true;
  uint8_t img = inline_image ? 1u : 0u;
  return rm_vec_push(&layout->flat_ids, &id) &&
         rm_vec_push(&layout->flat_inline_image, &img);
}

/* The text, its line numbers and its faces — one walk of the items in
 * document order.
 *
 * One walk and not three, so that what gets drawn, what the handwriting is
 * positioned against and what is set in bold cannot drift apart. */
static rm_status compose(const text_item* items, size_t count, bool collect_map,
                         rmi_text_layout* layout) {
  uint32_t* order = nullptr;
  if (!rm_text_record_order(items, count, &order)) {
    return RM_ERR_OUT_OF_MEMORY;
  }

  rm_status status = RM_OK;
  if (!layout_push_line(layout)) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }

  uint8_t face = 0;

  /* Scalars in the current paragraph get their own counter: a count of
   * characters as a reader sees them is not what the file numbers, and a
   * combining mark does not grow it while the file does give it an
   * identifier. */
  size_t scalars = 0;

  for (size_t step = 0; step < count; step += 1) {
    const text_item* item = &items[order[step]];
    if (item->deleted != 0) continue;
    if (item->has_marker) {
      switch (item->marker) {
        case 1:
          face |= FACE_BOLD;
          break;
        case 2:
          face &= (uint8_t)~FACE_BOLD;
          break;
        case 3:
          face |= FACE_ITALIC;
          break;
        case 4:
          face &= (uint8_t)~FACE_ITALIC;
          break;

        /* A link of typed text: kept in the file, not drawn as anything of
         * its own. Its characters are the ordinary records between the two
         * marks, and they stay text. */
        case MARK_LINK_START:
        case MARK_LINK_END:
          break;

        /* An image in the flow is one character of the text — the object
         * replacement character — and takes its place in the paragraph, so
         * the offsets after it are the tablet's. A deleted one is no
         * character at all. */
        case MARK_INLINE_IMAGE: {
          if (layout->lines.count - 1u >= (size_t)INT32_MAX ||
              scalars >= (size_t)INT32_MAX - 1u) {
            status = RM_ERR_MALFORMED_PAGE;
            goto done;
          }

          size_t paragraph = layout->lines.count - 1u;
          rmi_line_build* line = rm_vec_at(&layout->lines, paragraph);
          rm_buf_append(&line->text, OBJECT_REPLACEMENT,
                        sizeof OBJECT_REPLACEMENT - 1u);
          if (line->text.error != RM_BUF_OK ||
              !rm_vec_push(&line->faces, &face) ||
              !record_flat_scalar(layout, collect_map, item->id, true)) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }

          scalars += 1;
          if (!record_place(layout, item->id,
                            anchor_pack(paragraph, scalars))) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }
          break;
        }
        default:
          RM_LOG_DEBUGF("render: unknown text mark %lu",
                        (unsigned long)item->marker);
      }
      continue;
    }

    if (item->text.length == 0) {
      if (!rm_crdt_equal(item->id, RM_CRDT_NONE)) {
        size_t paragraph = layout->lines.count - 1u;
        if (paragraph >= (size_t)INT32_MAX || scalars > (size_t)INT32_MAX) {
          status = RM_ERR_MALFORMED_PAGE;
          goto done;
        }
        if (!record_place(layout, item->id, anchor_pack(paragraph, scalars))) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
      }
      continue;
    }

    /* By Unicode scalars, not by graphemes: the file numbers characters as
     * scalars. On NFC Latin and Cyrillic the two agree; on Arabic diacritics
     * they do not, and a grapheme count shifts every style key and anchor
     * after the first such mark. */
    uint64_t offset = 0;
    size_t at = 0;
    while (at < item->text.length) {
      uint32_t scalar;
      size_t next =
          rm_utf8_next(item->text.bytes, item->text.length, at, &scalar);

      /* The characters run on from the item's own identifier. On a corrupted
       * file that identifier can sit at the top of the range, and the sum
       * would wrap into somebody else's character. The text is still worth
       * drawing; what is lost is the anchoring of handwriting to these
       * characters. */
      bool has_id = item->id.counter <= UINT64_MAX - offset;
      rm_crdt_id id =
          has_id ? rm_crdt_make(item->id.author, item->id.counter + offset)
                 : RM_CRDT_NONE;
      offset += 1;

      size_t paragraph = layout->lines.count - 1u;
      if (paragraph >= (size_t)INT32_MAX || scalars >= (size_t)INT32_MAX - 1u) {
        status = RM_ERR_MALFORMED_PAGE;
        goto done;
      }

      /* A newline ends its paragraph, and the pen that typed it stood at the
       * end of that paragraph rather than at the start of the next one. It is
       * the paragraph's character after the last letter, and the pen stands
       * before the character an anchor names — so one past the letters, not
       * on the last of them. */
      if (scalar == '\n') {
        if (has_id &&
            !record_place(layout, id, anchor_pack(paragraph, scalars + 1u))) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        if (has_id && !rm_crdt_equal(id, RM_CRDT_NONE) &&
            !rm_idmap_set(&layout->style_line_of_key, id, paragraph + 1u)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }

        if (!record_flat_scalar(layout, collect_map, id, false)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        if (layout->lines.count >= (size_t)INT32_MAX) {
          status = RM_ERR_MALFORMED_PAGE;
          goto done;
        }
        if (!layout_push_line(layout)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
        scalars = 0;
      } else {
        rmi_line_build* line = rm_vec_at(&layout->lines, paragraph);
        rm_buf_append(&line->text, item->text.bytes + at, next - at);
        if (line->text.error != RM_BUF_OK ||
            !rm_vec_push(&line->faces, &face) ||
            !record_flat_scalar(layout, collect_map, id, false)) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }

        /* The character's own number, counted from one: the renderer stands
         * the pen before the character an anchor names. */
        scalars += 1;
        if (has_id &&
            !record_place(layout, id, anchor_pack(paragraph, scalars))) {
          status = RM_ERR_OUT_OF_MEMORY;
          goto done;
        }
      }
      at = next;
    }
  }

done:
  free(order);
  return status;
}

/* The place an anchor points at.
 *
 * Most anchors name a character, and the answer is the pen position just past
 * it. Two do not: the format has sentinel identifiers for the start and the
 * end of the text, and handwriting placed before or after everything typed is
 * anchored to those instead. They must not be left to the "nowhere" fallback:
 * treating a sentinel as line zero drags every stroke anchored to it to the
 * top of the text. */
static bool layout_anchor(const rmi_text_layout* layout, rm_crdt_id anchor,
                          rm_text_anchor* out) {
  if (rm_crdt_equal(anchor, rm_crdt_make(0, ANCHOR_START_SENTINEL))) {
    *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
    return true;
  }
  if (rm_crdt_equal(anchor, rm_crdt_make(0, ANCHOR_END_SENTINEL))) {
    /* Just past the last character, the way any anchor is — and a newline,
     * like every newline, ends its own paragraph: text ending in a Return
     * puts the pen at the end of the paragraph that Return closes, not in the
     * empty one after it. On a page of nineteen Returns the block anchored
     * here stands at paragraph 18, one line under paragraph 17's. Skipping
     * empty paragraphs would put it at the top, across other blocks; the
     * empty last paragraph would put it a line too low. The offset is one
     * past the letters: an anchor names the character the pen stands before,
     * and after the text that is the one that would come next. No text at all
     * — no lines, or the one empty line the parser starts with — ends where
     * it starts. */
    size_t count = layout->lines.count;
    if (count == 0) {
      *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
      return true;
    }

    size_t last = count - 1u;
    const rmi_line_build* line = rm_vec_at(&layout->lines, last);
    if (count == 1 && line->faces.count == 0) {
      *out = (rm_text_anchor){.paragraph = 0, .offset = 0};
      return true;
    }
    if (line->faces.count == 0 && last > 0) {
      last -= 1u;
      line = rm_vec_at(&layout->lines, last);
    }
    if (last > (size_t)INT32_MAX || line->faces.count >= (size_t)INT32_MAX)
      return false;
    *out = (rm_text_anchor){
        .paragraph = (int32_t)last,
        .offset = (int32_t)line->faces.count + 1,
    };
    return true;
  }

  uint64_t packed;
  if (rm_idmap_get(&layout->place_of_character, anchor, &packed)) {
    *out = anchor_unpack(packed);
    return true;
  }
  return false;
}

/* Walk the settings by wire type. A field with a known width can be skipped
 * on the way to a later width register. */
bool rmi_next_text_settings_field(rm_reader* settings, uint32_t* out_tag,
                                  uint8_t* out_type, rm_reader* out_field) {
  *out_field = (rm_reader){0};
  if (settings->offset >= settings->size) return false;
  if (rm_read_tag(settings, out_tag, out_type) != RM_OK ||
      rmi_skip_tagged_value(settings, *out_type, out_field) != RM_OK) {
    settings->offset = settings->size;
    return false;
  }
  return true;
}

/* Tag 5 holds settings. Its tag 1 holds the width's write stamp followed by a
 * tag-2 float32. Keep the newest finite, nonnegative width. Zero leaves the
 * legacy width in use. */
static void read_settings_width_value(uint32_t index, uint8_t type,
                                      rm_reader field, rm_crdt_id* previous,
                                      bool* out_has_width, double* out_width) {
  if (index != 1 || type != RM_TAG_LENGTH4) return;
  rm_crdt_id stamp;
  float value;
  if (rm_read_crdt(&field, 1, &stamp) != RM_OK ||
      !rm_match_tag(&field, 2, RM_TAG_BYTE4) ||
      rm_read_f32(&field, &value) != RM_OK) {
    return;
  }
  double result = (double)value;
  if (!isfinite(result) || result < 0.0 || rm_stamp_newer(*previous, stamp))
    return;
  *previous = stamp;
  *out_width = result;
  *out_has_width = true;
}

/* The root text block: the paragraphs, the styles keyed to them, and the box
 * they are set in. */
static rm_status parse_root_text(rmi_scene* ctx, rm_reader* reader, size_t end,
                                 bool* out_found) {
  *out_found = false;
  rm_crdt_id ignored;
  RM_TRY(rm_read_crdt(reader, 1, &ignored));

  rm_vec items;
  rm_vec_init(&items, sizeof(text_item));
  rmi_text_layout layout = {0};
  rm_vec_init(&layout.lines, sizeof(rmi_line_build));
  rm_vec_init(&layout.styles, sizeof(rmi_style_entry));
  rm_idmap_init(&layout.place_of_character);
  rm_idmap_init(&layout.style_line_of_key);
  rm_vec_init(&layout.flat_ids, sizeof(rm_crdt_id));
  rm_vec_init(&layout.flat_inline_image, sizeof(uint8_t));

  bool has_origin = false;
  bool has_settings_width = false;
  rm_crdt_id width_stamp = RM_CRDT_NONE;
  double settings_width = 0.0;
  rm_status status = RM_OK;

  while (reader->offset < end) {
    size_t saved = reader->offset;
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(reader, &index, &type) != RM_OK) {
      reader->offset = saved;
      break;
    }

    /* The column width is the one field of this block that is not a subblock,
     * and stopping at it would leave it unread. */
    if (type == RM_TAG_BYTE4) {
      float raw;
      if (rm_read_f32(reader, &raw) != RM_OK) {
        reader->offset = saved;
        break;
      }
      double value = (double)raw;
      if (index == TAG_COLUMN_WIDTH && isfinite(value) && value > 0) {
        layout.has_column_width = true;
        layout.column_width = value;
      }
      continue;
    }
    if (type != RM_TAG_LENGTH4) {
      reader->offset = saved;
      break;
    }

    uint32_t length;
    if (rm_read_u32(reader, &length) != RM_OK) {
      if (index == TAG_TEXT_ITEMS) {
        status = RM_ERR_MALFORMED_PAGE;
        goto done;
      }
      reader->offset = saved;
      break;
    }
    size_t sub_end;
    if (!rm_fits_within(reader, length, end, &sub_end)) {
      if (index == TAG_TEXT_ITEMS) {
        status = RM_ERR_MALFORMED_PAGE;
        goto done;
      }
      reader->offset = saved;
      break;
    }

    if (index == TAG_TEXT_ITEMS) {
      /* The text items and paragraph styles are siblings inside one field.
       * Step over the item container before looking for the styles, including
       * past any intervening fields whose lengths are known. */
      rm_reader inner = *reader;
      inner.size = sub_end;
      rm_vec_free(&items);
      rm_vec_init(&items, sizeof(text_item));
      bool items_env_err = false;
      status = parse_text_items(ctx, &inner, &items, &items_env_err);
      if (status == RM_ERR_OUT_OF_MEMORY || items_env_err) goto done;

      /* An unreadable item discards the item list. With its container
       * bounded, we can still look for styles. A broken container boundary or
       * allocation failure stops this parse. */
      if (status != RM_OK) {
        rm_vec_free(&items);
        rm_vec_init(&items, sizeof(text_item));
        status = RM_OK;
      }
      rm_vec_free(&layout.styles);
      rm_vec_init(&layout.styles, sizeof(rmi_style_entry));
      bool found_styles = false;

      while (inner.offset < sub_end) {
        size_t sib_saved = inner.offset;
        uint32_t sib_tag;
        uint8_t sib_type;
        if (rm_read_tag(&inner, &sib_tag, &sib_type) != RM_OK) break;
        if (sib_tag == 2 && sib_type == RM_TAG_LENGTH4) {
          inner.offset = sib_saved;
          found_styles = true;
          break;
        }
        if (rmi_skip_tagged_value(&inner, sib_type, nullptr) != RM_OK) break;
      }

      if (found_styles) {
        bool styles_env_err = false;
        status =
            parse_paragraph_styles(&inner, &layout.styles, &styles_env_err);
        if (status == RM_ERR_OUT_OF_MEMORY || styles_env_err) goto done;
        if (status != RM_OK) {
          rm_vec_free(&layout.styles);
          rm_vec_init(&layout.styles, sizeof(rmi_style_entry));
          status = RM_OK;
        }
      }
    } else if (index == TAG_TEXT_POSITION && length >= 16) {
      double x, y;
      status = rm_read_f64(reader, &x);
      if (status != RM_OK) goto done;
      status = rm_read_f64(reader, &y);
      if (status != RM_OK) goto done;

      /* Left unset when it is not a place: the block ends by refusing text
       * nobody can lay out, whole and counted, the same as text that will not
       * parse. */
      if (isfinite(x) && isfinite(y)) {
        layout.origin_x = x;
        layout.origin_y = y;
        has_origin = true;
      }
    } else if (index == TAG_TEXT_SETTINGS) {
      rm_reader settings = *reader;
      settings.size = sub_end;
      uint32_t stag;
      uint8_t stype;
      rm_reader sfield;
      while (rmi_next_text_settings_field(&settings, &stag, &stype, &sfield)) {
        read_settings_width_value(stag, stype, sfield, &width_stamp,
                                  &has_settings_width, &settings_width);
      }
    }
    status = rm_read_seek(reader, sub_end);
    if (status != RM_OK) goto done;
  }

  /* A positive width in the winning settings register takes precedence over
   * tag 4. A zero setting leaves the legacy width in use. */
  if (has_settings_width && settings_width > 0.0) {
    layout.has_column_width = true;
    layout.column_width = settings_width;
  }

  if (!has_origin) {
    status = RM_OK;
    goto done;
  }

  rm_vec parts;
  rm_vec_init(&parts, sizeof(text_item));
  status = rm_split_text_runs_checked(items.data, items.count, &parts);
  if (status == RM_OK)
    status = compose(parts.data, parts.count, ctx->collect_flat_map, &layout);
  rm_vec_free(&parts);
  if (status != RM_OK) goto done;

  rm_vec_free(&items);

  /* Only a usable root replaces the previous text. A bounded list can lose
   * rows or styles and still leave the root usable. A broken container
   * boundary or missing origin leaves the previous root standing. */
  if (ctx->has_text) layout_free(&ctx->text);
  ctx->text = layout;
  ctx->has_text = true;
  *out_found = true;
  return RM_OK;

done:
  rm_vec_free(&items);
  layout_free(&layout);
  return status;
}

/* ------------------------------------------------------------------ */
/* Scene walk & placement                                              */
/* ------------------------------------------------------------------ */

typedef struct scene_key_ctx {
  const rmi_scene_item* items;
} scene_key_ctx;

/* The grouping key for a scene item: the node it hangs on. */
static rm_crdt_id scene_parent_key(size_t index, void* ctx) {
  return ((scene_key_ctx*)ctx)->items[index].parent;
}

bool rmi_scene_group_by_parent(const rmi_scene_item* items, size_t count,
                               rm_grouping* out) {
  scene_key_ctx key_ctx = {.items = items};
  return rm_grouping_build(out, count, scene_parent_key, &key_ctx);
}

/* Supply an in-memory group edge for each live placement missing its ordering
 * link. Both drawing and layer enumeration order these edges last. */
bool rmi_append_unlinked_placements(rmi_scene* ctx,
                                    const rmi_tree_state* tree) {
  rm_idmap linked = {0};
  bool ok = false;
  const rmi_scene_item* items = ctx->items.data;
  for (size_t i = 0; i < ctx->items.count; ++i) {
    const rmi_scene_item* item = &items[i];
    if (item->kind == ITEM_GROUP && !item->deleted &&
        rmi_tree_links(tree, item->parent, item->group) &&
        !rm_idmap_set(&linked, item->group, 1))
      goto done;
  }

  for (size_t i = 0; i < ctx->tree_events.count; ++i) {
    const rmi_tree_event* event = &tree->events[i];
    uint64_t latest;
    if (!rm_idmap_get(&tree->placed_by, event->node, &latest) || latest != i ||
        rm_idmap_contains(&tree->removed, event->node) ||
        rm_idmap_contains(&linked, event->node))
      continue;
    rmi_scene_item edge = {.kind = ITEM_GROUP,
                           .group = event->node,
                           .parent = event->parent,
                           .placement_only = true};
    if (!rm_vec_push(&ctx->items, &edge)) goto done;
  }
  ok = true;
done:
  rm_idmap_free(&linked);
  return ok;
}

typedef struct walk_frame {
  rm_crdt_id node;
  uint32_t* children;
  size_t child_count;
  size_t next;
} walk_frame;

typedef struct scene_walk {
  const rmi_scene_item* items;
  const rm_grouping* children_of;
  const rmi_tree_state* tree;
  bool* placed;
  rm_idmap visited_nodes;
  rm_vec strokes;
  rm_vec images;
  rm_vec* glyphs;
  rm_vec* events;
  bool failed;
} scene_walk;

/* Record the mixed order of groups, ink and pictures when an edit needs to
 * check what moved. */
static void push_scene_event(scene_walk* walk, rm_crdt_id parent, rm_crdt_id id,
                             rmi_item_kind kind) {
  if (walk->events == nullptr) return;
  rmi_scene_event event = {.parent = parent, .id = id, .kind = kind};
  if (!rm_vec_push(walk->events, &event)) walk->failed = true;
}

/* Order the node's children with rmi_chain_order. The caller frees the
 * resulting index array. */
static uint32_t* children_in_chain_order(const scene_walk* walk,
                                         rm_crdt_id node, size_t* out_count) {
  const uint32_t* children;
  size_t count;
  if (!rm_grouping_find(walk->children_of, node, &children, &count)) {
    *out_count = 0;
    return calloc(1, sizeof(uint32_t));
  }
  return rmi_chain_order(children, count, walk->items, out_count);
}

/* Depth first with a stack of its own rather than with the call stack: groups
 * nest as deeply as a page has of them, and the depth is the file's to
 * choose. A stack overflow is not an error one can catch — it is the end of
 * the process. Each frame is a node, its children in chain order, and how far
 * down them the walk has got. */
static void walk_from(scene_walk* walk, rm_crdt_id root) {
  if (rm_idmap_contains(&walk->tree->removed, root) ||
      rm_idmap_contains(&walk->visited_nodes, root))
    return;
  if (!rm_idmap_set(&walk->visited_nodes, root, 1)) {
    walk->failed = true;
    return;
  }

  rm_vec stack;
  rm_vec_init(&stack, sizeof(walk_frame));
  walk_frame first = {.node = root, .next = 0};
  first.children = children_in_chain_order(walk, root, &first.child_count);
  if (first.children == nullptr || !rm_vec_push(&stack, &first)) {
    free(first.children);
    walk->failed = true;
    rm_vec_free(&stack);
    return;
  }

  while (stack.count > 0) {
    walk_frame* frame = rm_vec_at(&stack, stack.count - 1);
    bool descended = false;
    while (frame->next < frame->child_count) {
      uint32_t index = frame->children[frame->next];
      frame->next += 1;
      if (walk->placed[index]) continue;
      walk->placed[index] = true;
      if (walk->items[index].deleted != 0) continue;

      switch (walk->items[index].kind) {
        case ITEM_STROKE: {
          rmi_ordered_stroke entry = {.item = index, .parent = frame->node};
          if (!rm_vec_push(&walk->strokes, &entry)) walk->failed = true;
          push_scene_event(walk, frame->node, walk->items[index].id,
                           ITEM_STROKE);
          break;
        }
        case ITEM_GROUP: {
          rm_crdt_id child = walk->items[index].group;

          /* A stale link: the child is drawn, if at all, from its present
           * parent. */
          if (!rmi_tree_links(walk->tree, frame->node, child)) continue;
          if (rm_idmap_contains(&walk->tree->removed, child)) continue;
          if (rm_idmap_contains(&walk->visited_nodes, child)) continue;
          if (!rm_idmap_set(&walk->visited_nodes, child, 1)) {
            walk->failed = true;
            break;
          }
          push_scene_event(walk, frame->node, walk->items[index].id,
                           ITEM_GROUP);

          /* The parent stays on the stack with its place kept, and the child
           * goes on top of it: that is what "depth first" is once the call
           * stack is not doing the remembering. */
          walk_frame next = {.node = child, .next = 0};
          next.children =
              children_in_chain_order(walk, child, &next.child_count);
          if (next.children == nullptr || !rm_vec_push(&stack, &next)) {
            free(next.children);
            walk->failed = true;
          }

          descended = true;
          break;
        }
        case ITEM_IMAGE: {
          /* Its place in the sequence travels as the number of strokes before
           * it: the z-order is the sequence. */
          rmi_placed_image entry = {
              .item = index,
              .strokes_before = walk->strokes.count,
              .parent = frame->node,
          };
          if (!rm_vec_push(&walk->images, &entry)) walk->failed = true;
          push_scene_event(walk, frame->node, walk->items[index].id,
                           ITEM_IMAGE);
          break;
        }

        case ITEM_GLYPH:
          if (!rm_vec_push(walk->glyphs, &walk->items[index].glyph))
            walk->failed = true;
          push_scene_event(walk, frame->node, walk->items[index].id,
                           ITEM_GLYPH);
          break;
        case ITEM_TOMBSTONE:
          break;
      }
      if (descended) break;
    }
    if (descended) continue;
    walk_frame* top = rm_vec_at(&stack, stack.count - 1);
    free(top->children);
    stack.count -= 1;
  }
  rm_vec_free(&stack);
}

/* The drawing omits hidden layers. An editor can ask for their items too, and
 * for the set already gone before visibility was applied. */
bool rmi_scene_order_items_ex(rmi_scene* ctx, rm_vec* out_strokes,
                              rm_vec* out_images, rm_vec* glyphs,
                              bool include_hidden, rm_vec* out_events,
                              rm_idmap* out_edit_removed) {
  bool ok = false;
  rmi_tree_state tree = {0};
  rm_idmap interior;
  rm_idmap_init(&interior);

  rm_grouping children_of = {0};
  bool* placed = nullptr;

  if (!rmi_tree_state_build(ctx->tree_events.data, ctx->tree_events.count,
                            &tree))
    goto done;

  if ((tree.removed.count || tree.has_root_placements) &&
      !rmi_removed_subtrees(ctx->items.data, ctx->items.count, &tree))
    goto done;

  if (out_edit_removed != nullptr) {
    size_t c = 0;
    rm_crdt_id key;
    uint64_t val;
    while (rm_idmap_next(&tree.removed, &c, &key, &val)) {
      if (!rm_idmap_set(out_edit_removed, key, val)) goto done;
    }
  }

  /* Propagate suppression over both placement records and live group links
   * before selecting roots. Missing ordering links can interrupt a DFS. */
  if (!include_hidden && ctx->hidden_nodes.count > 0) {
    size_t cursor = 0;
    rm_crdt_id hidden;
    uint64_t value;
    while (rm_idmap_next(&ctx->hidden_nodes, &cursor, &hidden, &value)) {
      if (!rm_idmap_set(&tree.removed, hidden, 1)) goto done;
    }
    if (!rmi_removed_subtrees(ctx->items.data, ctx->items.count, &tree))
      goto done;
  }

  if (!rmi_append_unlinked_placements(ctx, &tree)) goto done;
  const rmi_scene_item* items = ctx->items.data;
  size_t count = ctx->items.count;
  scene_key_ctx key_ctx = {.items = items};
  if (!rm_grouping_build(&children_of, count, scene_parent_key, &key_ctx))
    goto done;

  /* A group item that names a node the records have since put elsewhere is a
   * stale link: the node is reached from where it is now, or not at all. */
  for (size_t i = 0; i < count; i += 1) {
    if (items[i].kind != ITEM_GROUP || items[i].deleted ||
        !rmi_tree_links(&tree, items[i].parent, items[i].group))
      continue;
    if (!rm_idmap_set(&interior, items[i].group, 1)) goto done;
  }

  placed = calloc(count == 0 ? 1 : count, sizeof *placed);
  if (placed == nullptr) goto done;

  scene_walk walk = {
      .items = items,
      .children_of = &children_of,
      .tree = &tree,
      .placed = placed,
      .glyphs = glyphs,
      .events = out_events,
  };
  rm_idmap_init(&walk.visited_nodes);
  walk.strokes = *out_strokes;
  walk.images = *out_images;

  /* Start at the page root when the tree places layers there. On pages with
   * incomplete links, also walk surviving parents that no live group item
   * names. */
  if (tree.has_root_placements) {
    walk_from(&walk, RM_CRDT_ROOT);
    if (walk.failed) goto done_walk;
  }

  for (size_t i = 0; i < count; i += 1) {
    if (rm_idmap_contains(&interior, items[i].parent)) continue;
    if (placed[i]) continue;
    walk_from(&walk, items[i].parent);
    if (walk.failed) goto done_walk;
  }

  /* Whatever the walk never reached — in file order. A node the records moved
   * under a removed or hidden one goes with it here too. */
  for (size_t i = 0; i < count; i += 1) {
    if (placed[i]) continue;
    if (items[i].deleted != 0) continue;

    rm_crdt_id parent = items[i].parent;
    if (rm_idmap_contains(&tree.removed, parent)) continue;
    if (items[i].kind == ITEM_STROKE) {
      rmi_ordered_stroke entry = {.item = (uint32_t)i, .parent = parent};
      if (!rm_vec_push(&walk.strokes, &entry)) goto done_walk;
      push_scene_event(&walk, parent, items[i].id, ITEM_STROKE);
    } else if (items[i].kind == ITEM_IMAGE) {
      rmi_placed_image entry = {
          .item = (uint32_t)i,
          .strokes_before = walk.strokes.count,
          .parent = parent,
      };
      if (!rm_vec_push(&walk.images, &entry)) goto done_walk;
      push_scene_event(&walk, parent, items[i].id, ITEM_IMAGE);
    } else if (items[i].kind == ITEM_GLYPH) {
      if (!rm_vec_push(glyphs, &items[i].glyph)) goto done_walk;
      push_scene_event(&walk, parent, items[i].id, ITEM_GLYPH);
    }
  }
  ok = !walk.failed;

done_walk:
  rm_idmap_free(&walk.visited_nodes);
  *out_strokes = walk.strokes;
  *out_images = walk.images;

done:
  free(placed);
  rm_grouping_free(&children_of);
  rm_idmap_free(&interior);
  rmi_tree_state_free(&tree);
  return ok;
}

/* Walk visible layers depth first, drawing each node's children in chain
 * order. Reordering a layer rewrites no stroke. Omit hidden and removed
 * subtrees, and keep surviving ink in file order when its neighbour links do
 * not add up. */
bool rmi_scene_order_items(rmi_scene* ctx, rm_vec* out_strokes,
                           rm_vec* out_images, rm_vec* glyphs) {
  return rmi_scene_order_items_ex(ctx, out_strokes, out_images, glyphs, false,
                                  nullptr, nullptr);
}

/* The anchor an item takes from the node it hangs on, when that node carries
 * one the laid-out text can locate — the one translation both anchored
 * handwriting and anchored images go through. The place says which character;
 * the horizontal comes with the node, straight from the file, and travels
 * alongside. */
bool rmi_scene_anchor_under(const rmi_scene* ctx, rm_crdt_id parent,
                            rm_text_anchor* out) {
  if (!ctx->has_text) return false;
  uint64_t index;
  if (!rm_idmap_get(&ctx->anchor_of_node, parent, &index)) return false;
  const rmi_node_anchor* node = rm_vec_at(&ctx->node_anchors, (size_t)index);
  rm_text_anchor place;
  if (!node->has_target || !layout_anchor(&ctx->text, node->anchor, &place))
    return false;
  place.has_origin_x = node->has_origin_x;
  place.origin_x = node->origin_x;
  *out = place;
  return true;
}

/* The file name the attachment map gave `key`, or null when the map does not
 * know it. */
const rm_str* rmi_scene_attachment_name(const rmi_scene* ctx,
                                        const uint8_t* key) {
  uint64_t at;
  if (!rm_keymap_get(&ctx->attachment_index, key, &at)) return nullptr;
  const rmi_attachment* entry = rm_vec_at(&ctx->attachments, (size_t)at);
  return &entry->name;
}

/* ------------------------------------------------------------------ */
/* Block dispatch & tombstones splitting                               */
/* ------------------------------------------------------------------ */

/* Release everything gathered into the context except its arena, which the
 * page built from it still owns. */
void rmi_scene_free_scratch(rmi_scene* ctx) {
  rm_vec_free(&ctx->items);
  rm_vec_free(&ctx->tree_events);
  rm_vec_free(&ctx->attachments);
  rm_keymap_free(&ctx->attachment_index);
  rm_vec_free(&ctx->node_anchors);
  rm_idmap_free(&ctx->anchor_of_node);
  rm_idmap_free(&ctx->hidden_nodes);
  rm_idmap_free(&ctx->visibility_of_node);
  rm_vec_free(&ctx->visibility_stamps);
  if (ctx->has_text) layout_free(&ctx->text);
}

void rmi_scene_destroy(rmi_scene* ctx) {
  rmi_scene_free_scratch(ctx);
  rm_arena_destroy(ctx->arena);
  ctx->arena = nullptr;
}

/* Fold one node's visibility record into the running tables, newer stamp
 * winning over older. */
static rm_status apply_visibility(rmi_scene* ctx, const rmi_tree_node* node) {
  if (!node->has_visible) return RM_OK;
  uint64_t index;
  if (rm_idmap_get(&ctx->visibility_of_node, node->id, &index)) {
    rm_crdt_id* stamp = rm_vec_at(&ctx->visibility_stamps, (size_t)index);

    /* Counters are page-wide. Author breaks concurrent ties; an older record
     * arriving later must not undo a newer visibility edit. */
    if (rm_stamp_newer(*stamp, node->visibility_stamp)) return RM_OK;
    *stamp = node->visibility_stamp;
  } else {
    index = ctx->visibility_stamps.count;
    if (!rm_vec_push(&ctx->visibility_stamps, &node->visibility_stamp) ||
        !rm_idmap_set(&ctx->visibility_of_node, node->id, index)) {
      return RM_ERR_OUT_OF_MEMORY;
    }
  }
  if (node->visible) {
    (void)rm_idmap_take(&ctx->hidden_nodes, node->id, nullptr);
  } else if (!rm_idmap_set(&ctx->hidden_nodes, node->id, 1)) {
    return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

/* Count the deletion once per stored block. Its length reserves identifiers
 * and may exceed one. */
static rm_status keep_scene_item(rmi_scene* ctx, rmi_scene_item* item,
                                 size_t block_start) {
  item->block_start = block_start;
  if (!rm_vec_push(&ctx->items, item)) return RM_ERR_OUT_OF_MEMORY;

  /* A value-less item holds its place even when its deletion count is zero.
   * A longer deletion still came from a single block in the file. */
  if (item->deleted != 0) ctx->deleted_items += 1;
  return RM_OK;
}

/* Walk the top-level envelopes and count unreadable contents one block at a
 * time. A broken envelope leaves no boundary to resume at. An unsupported
 * minimum reader version or an allocation failure also stops the page. */
static rm_status parse_blocks(rmi_scene* ctx, rm_error* error) {
  rm_reader* reader = &ctx->reader;

  while (rm_reader_remaining(reader) >= 8) {
    rm_block_header block;
    RM_TRY(rm_read_block_header(reader, &block));
    size_t block_end = block.end;
    uint8_t current_version = block.current_version;
    uint8_t block_type = block.type;
    uint8_t max_min = rmi_block_supported_version(block_type);
    if (max_min != 0 && block.min_version > max_min) {
      if (error) {
        *error = (rm_error){.status = RM_ERR_UNSUPPORTED_BLOCK_VERSION,
                            .value = block_type,
                            .version = block.min_version};
        snprintf(error->message, sizeof error->message,
                 "block 0x%02x at offset %zu requires reader %u (supported %u)",
                 block_type, block.start, block.min_version, max_min);
      }
      return RM_ERR_UNSUPPORTED_BLOCK_VERSION;
    }

    rm_reader inner = *reader;
    inner.size = block_end;
    switch (block_type) {
      case RM_BLOCK_SCENE_LINE_ITEM: {
        /* A stroke we did not understand is a lost piece of the drawing, but
         * no reason to lose the whole page. */
        rmi_scene_item item;
        rm_status status = parse_line_item(
            ctx, &inner, current_version > 2 ? 2 : current_version, &item);
        if (status == RM_OK) {
          RM_TRY(keep_scene_item(ctx, &item, block.start));
        } else if (status == RM_ERR_OUT_OF_MEMORY) {
          return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->unreadable_strokes += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      /* Every block below counts itself only when its own parse failed, which
       * is what keeps `skipped_blocks` meaning what the field says: the
       * pieces of this page that will not be drawn. A block of a type we
       * understand and read is not a loss and is not counted — counted
       * regardless, the number would say "blocks other than strokes" and
       * could not tell a loss from an ordinary page. */
      case RM_BLOCK_SCENE_TREE: {
        /* Which nodes are in the tree. The one authority on the list of
         * layers: a deleted layer keeps its 0x02 and its 0x04, and only this
         * block says it is gone. */
        rmi_tree_event event;
        if (parse_scene_tree(&inner, &event) == RM_OK) {
          if (!rm_vec_push(&ctx->tree_events, &event))
            return RM_ERR_OUT_OF_MEMORY;
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      case RM_BLOCK_SCENE_GROUP_ITEM: {
        /* The link of a child group into its parent's list of items — what
         * the order of layers is made of. Unread, the layer it named comes
         * unhooked from the tree and its strokes fall back to file order —
         * kept, but not where they were. */
        rmi_scene_item item;
        rm_status status = parse_group_item(ctx, &inner, &item);
        if (status == RM_OK) {
          RM_TRY(keep_scene_item(ctx, &item, block.start));
        } else {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      case RM_BLOCK_UNDO_TOMBSTONE: {
        /* The tombstone an undo leaves: the same preamble as an erased
         * stroke's, no value. It holds a position in the chain, and dropping
         * it would unhook whatever hangs on it. */
        rmi_scene_item item;
        if (parse_scene_tombstone(&inner, &item) == RM_OK) {
          RM_TRY(keep_scene_item(ctx, &item, block.start));
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      case RM_BLOCK_ATTACHMENT_MAP: {
        /* The names of the page's attachments, keyed the way the image items
         * name them. Unread, a page of captures comes out blank — the tablet
         * rasterises a capture whole, and nothing of it lives anywhere else. */
        rm_status status = parse_attachment_map(ctx, &inner);
        if (status != RM_OK) {
          if (status == RM_ERR_OUT_OF_MEMORY) return status;
          ctx->other_blocks += 1;
        }
        break;
      }

      case RM_BLOCK_IMAGE_ITEM: {
        /* A picture that will not parse is a lost picture, and a picture is
         * the whole of a captured page: counted, and its failure kept for the
         * log, the same as a stroke's. */
        rmi_scene_item item;
        rm_status status = parse_image_item(ctx, &inner, &item);
        if (status == RM_OK) {
          RM_TRY(keep_scene_item(ctx, &item, block.start));
        } else {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      case RM_BLOCK_TREE_NODE: {
        rmi_tree_node node;
        if (rmi_parse_tree_node(&inner, block_end, nullptr, &node, nullptr) ==
            RM_OK) {
          if (node.has_anchor || node.has_origin_x) {
            uint64_t index;
            if (!rm_idmap_get(&ctx->anchor_of_node, node.id, &index)) {
              index = ctx->node_anchors.count;
              if (!rm_vec_push(&ctx->node_anchors, &(rmi_node_anchor){0}) ||
                  !rm_idmap_set(&ctx->anchor_of_node, node.id, index))
                return RM_ERR_OUT_OF_MEMORY;
            }

            rmi_node_anchor* a = rm_vec_at(&ctx->node_anchors, index);
            if (node.has_anchor &&
                !rm_stamp_newer(a->target_stamp, node.target_stamp)) {
              a->anchor = node.anchor;
              a->target_stamp = node.target_stamp;
              a->has_target = true;
            }
            if (node.has_origin_x &&
                !rm_stamp_newer(a->origin_stamp, node.origin_stamp)) {
              a->origin_x = node.origin_x;
              a->origin_stamp = node.origin_stamp;
              a->has_origin_x = true;
            }
          }
          RM_TRY(apply_visibility(ctx, &node));
        } else {
          ctx->other_blocks += 1;
        }
        break;
      }

      /* An unusable RootText block counts as a loss and leaves an earlier
       * usable block standing. Damage confined to a bounded list can leave
       * the root usable. */
      case RM_BLOCK_ROOT_TEXT: {
        bool found = false;
        rm_status status = parse_root_text(ctx, &inner, block_end, &found);
        if (status == RM_ERR_OUT_OF_MEMORY) return status;
        if (!found) ctx->other_blocks += 1;
        break;
      }

      case RM_BLOCK_SCENE_GLYPH_ITEM: {
        /* The marker over the text of an imported PDF. Not a stroke: it
         * carries the highlighted text and ready-made rectangles, so skipping
         * it drops the highlight silently. A highlight the user took off is a
         * tombstone, as ordinary as an erased stroke, and is not counted as a
         * loss — only a highlight that is there and would not read is. */
        rmi_scene_item item;
        rm_status status = parse_glyph_item(ctx, &inner, block_end, &item);
        if (status == RM_ERR_OUT_OF_MEMORY) return status;

        if (status == RM_OK) {
          RM_TRY(keep_scene_item(ctx, &item, block.start));

          /* The neighbours can still use the preamble. A live highlight
           * also promised rectangles, so report the missing drawing. */
          if (item.kind == ITEM_TOMBSTONE && item.deleted == 0) {
            status = malformed(
                ctx, "a highlight has neither a value nor a deletion");
          }
        }

        if (status != RM_OK) {
          ctx->other_blocks += 1;
          ctx_note_failure(ctx, status);
        }
        break;
      }

      case RM_BLOCK_SCENE_INFO:
        /* Never counted: this one does not fail. It reads the fields it knows
         * and leaves the rest at their defaults, so no outcome here loses
         * content. */
        parse_scene_info(ctx, &inner, block_end);
        break;

      /* The service blocks every page opens with, in the order the tablet
       * writes them: the author table, the migration record, the page
       * counters. All three are known and hold nothing that is drawn, so
       * stepping over them by length loses no content and must not be counted
       * as loss — counted, they would put three on every page and a
       * hundred-page notebook would report three hundred phantom losses. */
      case RM_BLOCK_AUTHOR_IDS:
      case RM_BLOCK_MIGRATION_INFO:
      case RM_BLOCK_PAGE_INFO:
        /* The other three of the service quartet. Stepped over by length on
         * purpose and never counted: nothing in them is drawn, so passing
         * them over is not a loss. */
        break;

      default:
        /* A block of a type this parser does not know — the one case where
         * stepping over it by length is all there is to do. */
        ctx->other_blocks += 1;
        break;
    }

    RM_TRY(rm_read_seek(reader, block_end));
  }

  /* Fewer bytes than a block header are left behind: not a block, and not a
   * reason to lose the page that came before them. */
  return RM_OK;
}

/* The header, checked the way every door into a page checks it: bytes too
 * short or of another shape are a format nobody can name, and a header of
 * another generation is named by its version. */
static rm_status check_header(const uint8_t* data, size_t size,
                              rm_error* out_error) {
  /* Too short to hold a header: the stream ends inside the first field there
   * is to read. */
  if (size < RM_HEADER_LENGTH) {
    rmi_error_set(out_error, RM_ERR_UNEXPECTED_EOF, 0, 0, nullptr);
    return RM_ERR_UNEXPECTED_EOF;
  }
  if (memcmp(data, RM_HEADER, RM_HEADER_LENGTH) != 0) {
    /* A page of an older generation is not a broken file, and the two must
     * not arrive as one error: the caller leaves the page out and tells the
     * user which format it is in, and it can only say that about a version it
     * has actually read. */
    int version = 0;
    if (rm_header_version((const char*)data, RM_HEADER_LENGTH, &version)) {
      rmi_error_set(out_error, RM_ERR_UNSUPPORTED_VERSION, version, 0, nullptr);
      return RM_ERR_UNSUPPORTED_VERSION;
    }
    char seen[33] = {0};
    memcpy(seen, data, 32);
    rmi_error_set(out_error, RM_ERR_UNSUPPORTED_FORMAT, 0, 0, seen);
    return RM_ERR_UNSUPPORTED_FORMAT;
  }
  return RM_OK;
}

/* Cuts from different authors belong to different counter ranges. Order by
 * author first so a search can stay within one range. */
static int compare_scene_cuts(const void* a, const void* b) {
  const rm_crdt_id* x = a;
  const rm_crdt_id* y = b;
  if (x->author != y->author) return (int)x->author - (int)y->author;
  return (x->counter > y->counter) - (x->counter < y->counter);
}

typedef struct scene_span_ref {
  uint64_t start, last;
  uint32_t orig_idx;
  uint8_t author;
} scene_span_ref;

static int compare_scene_span_refs(const void* a, const void* b) {
  const scene_span_ref* x = a;
  const scene_span_ref* y = b;
  if (x->author != y->author) return (int)x->author - (int)y->author;
  if (x->start != y->start)
    return (x->start > y->start) - (x->start < y->start);
  if (x->last != y->last) return (x->last > y->last) - (x->last < y->last);
  return (x->orig_idx > y->orig_idx) - (x->orig_idx < y->orig_idx);
}

/* A scene tombstone also reserves consecutive scalar IDs. Introduce virtual
 * tombstones only at the boundaries other items actually name. */
static rm_status split_scene_tombstones(rmi_scene* ctx) {
  const rmi_scene_item* items = ctx->items.data;
  size_t count = ctx->items.count;
  size_t i = 0;
  while (i < count && items[i].deleted <= 1) ++i;
  if (i == count) return RM_OK;

  rm_vec cuts, expanded, rows, parts, split_pool;
  rm_vec_init(&cuts, sizeof(rm_crdt_id));
  rm_vec_init(&expanded, sizeof(rmi_scene_item));
  rm_vec_init(&rows, sizeof(text_item));
  rm_vec_init(&parts, sizeof(text_item));
  rm_vec_init(&split_pool, sizeof(rmi_scene_item));
  scene_span_ref* refs = nullptr;
  uint32_t* first_part = nullptr;
  uint32_t* part_count = nullptr;
  rm_status status = RM_ERR_OUT_OF_MEMORY;

  for (size_t k = 0; k < count; ++k) {
    rm_crdt_id left = items[k].left, right = items[k].right;
    if (!rm_crdt_equal(left, RM_CRDT_NONE) && left.counter < UINT64_MAX) {
      ++left.counter;
      if (!rm_vec_push(&cuts, &left)) goto done;
    }
    if (!rm_crdt_equal(right, RM_CRDT_NONE) && !rm_vec_push(&cuts, &right))
      goto done;
  }
  rm_crdt_id* cut = cuts.data;
  size_t unique_cuts = 0;
  if (cuts.count > 0) {
    qsort(cut, cuts.count, sizeof *cut, compare_scene_cuts);
    for (size_t k = 0; k < cuts.count; ++k) {
      if (unique_cuts == 0 || !rm_crdt_equal(cut[unique_cuts - 1], cut[k])) {
        cut[unique_cuts++] = cut[k];
      }
    }
  }
  if (unique_cuts == 0) {
    status = RM_OK;
    goto done;
  }

  refs = calloc(count, sizeof *refs);
  first_part = calloc(count, sizeof *first_part);
  part_count = calloc(count, sizeof *part_count);
  if (!refs || !first_part || !part_count) goto done;

  for (size_t k = 0; k < count; ++k) {
    refs[k] = (scene_span_ref){
        .start = items[k].id.counter,
        .last = rmi_span_last(items[k].id.counter, items[k].deleted),
        .orig_idx = (uint32_t)k,
        .author = items[k].id.author,
    };
  }
  qsort(refs, count, sizeof *refs, compare_scene_span_refs);

  for (size_t comp_lo = 0; comp_lo < count;) {
    uint8_t author = refs[comp_lo].author;
    uint64_t comp_start = refs[comp_lo].start;
    uint64_t comp_last = refs[comp_lo].last;
    size_t comp_hi = comp_lo + 1u;
    bool needs_split = items[refs[comp_lo].orig_idx].deleted > 1;
    while (comp_hi < count && refs[comp_hi].author == author &&
           refs[comp_hi].start <= comp_last) {
      if (refs[comp_hi].last > comp_last) comp_last = refs[comp_hi].last;
      if (items[refs[comp_hi].orig_idx].deleted > 1) needs_split = true;
      comp_hi += 1u;
    }
    if (!needs_split) {
      comp_lo = comp_hi;
      continue;
    }

    /* Find distinct global cuts inside (comp_start, comp_last] on author. */
    size_t cut_lo = 0, cut_hi_bound = unique_cuts;
    while (cut_lo < cut_hi_bound) {
      size_t mid = cut_lo + (cut_hi_bound - cut_lo) / 2u;
      if (cut[mid].author < author ||
          (cut[mid].author == author && cut[mid].counter <= comp_start)) {
        cut_lo = mid + 1u;
      } else {
        cut_hi_bound = mid;
      }
    }
    size_t cut_hi = cut_lo;
    while (cut_hi < unique_cuts && cut[cut_hi].author == author &&
           cut[cut_hi].counter <= comp_last) {
      cut_hi += 1u;
    }
    size_t comp_cuts = cut_hi - cut_lo;
    if (comp_cuts == 0) {
      comp_lo = comp_hi;
      continue;
    }

    rows.count = 0;
    parts.count = 0;
    for (size_t k = comp_lo; k < comp_hi; ++k) {
      const rmi_scene_item* it = &items[refs[k].orig_idx];
      text_item row = {.id = it->id,
                       .left = it->left,
                       .right = it->right,
                       .deleted = it->deleted};
      if (!rm_vec_push(&rows, &row)) goto done;
    }

    size_t comp_count = comp_hi - comp_lo;
    size_t base = comp_cuts <= SIZE_MAX - 16u ? comp_cuts + 16u : SIZE_MAX;
    size_t comp_budget = comp_count <= (SIZE_MAX - base) / 4u
                             ? comp_count * 4u + base
                             : SIZE_MAX;
    rm_status split_status = rm_split_text_runs_with_cuts(
        rows.data, rows.count, cut + cut_lo, comp_cuts, comp_budget, &parts);
    if (split_status == RM_ERR_OUT_OF_MEMORY) goto done;
    if (split_status == RM_ERR_MALFORMED_PAGE) {
      ctx->other_blocks += 1u;
      (void)malformed(ctx, "overlapping scene tombstones exceed split budget");
      ctx_note_failure(ctx, RM_ERR_MALFORMED_PAGE);
      comp_lo = comp_hi;
      continue;
    }

    for (size_t p = 0; p < parts.count; ++p) {
      const text_item* part = rm_vec_at(&parts, p);
      uint32_t orig_idx = refs[comp_lo + part->source].orig_idx;
      rmi_scene_item item = items[orig_idx];
      item.id = part->id;
      item.left = part->left;
      item.right = part->right;
      item.deleted = part->deleted;
      if (split_pool.count > UINT32_MAX) {
        status = RM_ERR_TOO_MANY_TO_NUMBER;
        goto done;
      }
      if (part_count[orig_idx] == 0)
        first_part[orig_idx] = (uint32_t)split_pool.count;
      part_count[orig_idx] += 1;
      if (!rm_vec_push(&split_pool, &item)) goto done;
    }
    comp_lo = comp_hi;
  }

  /* Emit items in their original file order so queries like
   * rm_highlighter_items remain order-preserving across unrelated tombstones.
   */
  const rmi_scene_item* pool = split_pool.data;
  for (size_t orig = 0; orig < count; ++orig) {
    if (part_count[orig] == 0) {
      if (!rm_vec_push(&expanded, &items[orig])) goto done;
    } else {
      for (uint32_t k = 0; k < part_count[orig]; ++k) {
        if (!rm_vec_push(&expanded, &pool[first_part[orig] + k])) goto done;
      }
    }
  }
  rm_vec_free(&ctx->items);
  ctx->items = expanded;
  expanded = (rm_vec){0};
#ifdef RMLINES_TESTING
  rm_test_set_split_output_count(ctx->items.count);
#endif
  status = RM_OK;

done:
  free(part_count);
  free(first_part);
  free(refs);
  rm_vec_free(&split_pool);
  rm_vec_free(&parts);
  rm_vec_free(&rows);
  rm_vec_free(&expanded);
  rm_vec_free(&cuts);
  return status;
}

/* One pass over a page's blocks, everything gathered and nothing yet decided
 * about what is drawn: hidden layers are in here with the rest. On success
 * the context is the caller's to free, arena included; on failure nothing is
 * left behind. */
rm_status rmi_scene_read_blocks_ex(const uint8_t* data, size_t size,
                                   bool collect_flat_map, rmi_scene* ctx,
                                   rm_error* out_error) {
#ifdef RMLINES_TESTING
  rm_test_scene_reads += 1u;
#endif
  RM_TRY(check_header(data, size, out_error));

  *ctx = (rmi_scene){
      .collect_flat_map = collect_flat_map,
      .background_visible = true,
      .root_document_visible = true,
  };
  ctx->arena = rm_arena_create();
  if (ctx->arena == nullptr) {
    rmi_error_set(out_error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
    return RM_ERR_OUT_OF_MEMORY;
  }
  rm_reader_init(&ctx->reader, data, size);
  ctx->reader.offset = RM_HEADER_LENGTH;
  rm_vec_init(&ctx->items, sizeof(rmi_scene_item));
  rm_vec_init(&ctx->tree_events, sizeof(rmi_tree_event));
  rm_vec_init(&ctx->attachments, sizeof(rmi_attachment));
  rm_keymap_init(&ctx->attachment_index);
  rm_vec_init(&ctx->node_anchors, sizeof(rmi_node_anchor));
  rm_idmap_init(&ctx->anchor_of_node);
  rm_idmap_init(&ctx->hidden_nodes);
  rm_idmap_init(&ctx->visibility_of_node);
  rm_vec_init(&ctx->visibility_stamps, sizeof(rm_crdt_id));

  rm_status status = parse_blocks(ctx, out_error);
  if (status == RM_OK) status = split_scene_tombstones(ctx);
  if (status != RM_OK) {
    rmi_scene_destroy(ctx);
    if (status != RM_ERR_UNSUPPORTED_BLOCK_VERSION)
      rmi_error_set(out_error, status, 0, 0, nullptr);
  }
  return status;
}

rm_status rmi_scene_read_blocks(const uint8_t* data, size_t size,
                                rmi_scene* ctx, rm_error* out_error) {
  return rmi_scene_read_blocks_ex(data, size, false, ctx, out_error);
}
