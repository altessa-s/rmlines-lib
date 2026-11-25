/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Stroke insertion, affine transformation, and point-range cutting. */

#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "rm_edit_ops.h"
#include "rm_scene.h"
#include "rm_wire_write.h"
#include "rmlines/parser.h"

static bool zero_id(rm_item_id id) { return !id.author && !id.counter; }

/* Compare the scene including hidden layers. An edit that changes their order
 * would otherwise appear correct until the user shows them again.
 *
 * Fills the four vectors with the page's strokes, pictures, highlights and
 * drawing events in drawing order. On success the caller releases them with
 * destroy_scene_snapshot; on failure nothing is left to release. */
static rm_status collect_scene_snapshot(const uint8_t* data, size_t size,
                                        rmi_scene* scene, rm_vec* strokes,
                                        rm_vec* images, rm_vec* glyphs,
                                        rm_vec* events, rm_error* error) {
  rm_vec_init(strokes, sizeof(rmi_ordered_stroke));
  rm_vec_init(images, sizeof(rmi_placed_image));
  rm_vec_init(glyphs, sizeof(rm_highlight));
  rm_vec_init(events, sizeof(rmi_scene_event));
  RM_TRY(rmi_scene_read_blocks(data, size, scene, error));
  if (!rmi_scene_order_items_ex(scene, strokes, images, glyphs, true, events,
                                nullptr)) {
    rm_vec_free(strokes);
    rm_vec_free(images);
    rm_vec_free(glyphs);
    rm_vec_free(events);
    rmi_scene_destroy(scene);
    rmi_error_set(error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
    return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

static void destroy_scene_snapshot(rmi_scene* scene, rm_vec* strokes,
                                   rm_vec* images, rm_vec* glyphs,
                                   rm_vec* events) {
  rm_vec_free(strokes);
  rm_vec_free(images);
  rm_vec_free(glyphs);
  rm_vec_free(events);
  rmi_scene_destroy(scene);
}

/* Two drawing events name the same item under the same parent. */
static bool events_equal(rmi_scene_event a, rmi_scene_event b) {
  return rm_crdt_equal(a.parent, b.parent) && rm_crdt_equal(a.id, b.id) &&
         a.kind == b.kind;
}

/* After inserting the line, walk the scene again to check its requested place
 * and the relative order of the earlier items. The candidate is owned by the
 * caller and left unverified for the session's completeness check. */
rm_status rmi_edit_insert_stroke(const uint8_t* data, size_t size,
                                 rm_item_id parent, rm_item_id id,
                                 rm_item_id left, rm_item_id right,
                                 const rm_draft_stroke* stroke,
                                 rmi_edit_candidate* out, rm_error* error) {
  *out = (rmi_edit_candidate){0};
  if (!data || !stroke || !stroke->points || !stroke->point_count ||
      !id.author || !id.counter || zero_id(parent))
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (!zero_id(left) && rm_item_id_equal(left, right))
    return rm_edit_result(error, RM_ERR_UNVERIFIED);
  if (!rmi_line_item_wire_fits(stroke, rm_crdt_of(parent), rm_crdt_of(id),
                               rm_crdt_of(left), rm_crdt_of(right)))
    return rm_edit_result(error, RM_ERR_TOO_MANY_TO_NUMBER);
  if (!rmi_pen_tool_valid(stroke->tool) ||
      !rmi_stroke_color_valid(stroke->tool, stroke->color, stroke->has_argb) ||
      !isfinite(stroke->thickness) ||
      !rmi_pen_thickness_valid(stroke->tool, stroke->thickness))
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (size < RM_HEADER_LENGTH)
    return rm_edit_result(error, RM_ERR_UNEXPECTED_EOF);
  for (size_t i = 0; i < stroke->point_count; ++i) {
    if (!isfinite(stroke->points[i].x) || !isfinite(stroke->points[i].y))
      return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  }

  RM_TRY(rm_edit_result(error, rmi_scene_check_ids(data, size, id, 1, error)));

  /* A parent with a properties block of its own counts as existing even
   * without a placement; the walk also leaves `r` at the end of the last
   * whole block, where the new stroke goes. */
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  bool has_metadata = false;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    rm_status status = rm_read_block_header(&r, &b);
    if (status != RM_OK) return rm_edit_result(error, status);
    if (b.type == RM_BLOCK_TREE_NODE) {
      rm_reader body = r;
      body.size = b.end;
      rm_crdt_id node;
      status = rm_read_crdt(&body, 1, &node);
      if (status != RM_OK) return rm_edit_result(error, status);
      if (rm_crdt_equal(node, rm_crdt_of(parent))) has_metadata = true;
    }
    r.offset = b.end;
  }
  RM_TRY(rmi_scene_check_location(data, size, parent, left, right, has_metadata,
                                  error));

  rmi_scene orig_scene = {0};
  rm_vec orig_strokes, orig_images, orig_glyphs, orig_events;
  RM_TRY(collect_scene_snapshot(data, size, &orig_scene, &orig_strokes,
                                &orig_images, &orig_glyphs, &orig_events,
                                error));

  rm_buf payload = {0}, block = {0}, output = {0};
  rm_write_line_item(&payload, stroke, rm_crdt_of(parent), rm_crdt_of(id),
                     rm_crdt_of(left), rm_crdt_of(right));
  rm_write_top_block(&block, RM_BLOCK_SCENE_LINE_ITEM, 2, 2, payload.bytes,
                     payload.count);
  rmi_buf_propagate(&block, &payload);
  rm_buf_free(&payload);

  /* The new block goes after the last whole block, ahead of any trailing
   * bytes too short to be one. Its place in the drawing comes from its
   * neighbour links, not from its position in the file. */
  rm_buf_append(&output, data, r.offset);
  rm_buf_append(&output, block.bytes, block.count);
  rm_buf_append(&output, data + r.offset, size - r.offset);
  rmi_buf_propagate(&output, &block);
  rm_buf_free(&block);

  rm_status buf_st = rmi_buf_status(&output);
  if (buf_st != RM_OK) {
    rm_buf_free(&output);
    destroy_scene_snapshot(&orig_scene, &orig_strokes, &orig_images,
                           &orig_glyphs, &orig_events);
    return rm_edit_result(error, buf_st);
  }
  size_t out_size = 0;
  uint8_t* bytes = rm_buf_take(&output, &out_size);
  if (!bytes) {
    destroy_scene_snapshot(&orig_scene, &orig_strokes, &orig_images,
                           &orig_glyphs, &orig_events);
    return rm_edit_result(error, RM_ERR_OUT_OF_MEMORY);
  }

  rmi_scene cand_scene = {0};
  rm_vec cand_strokes, cand_images, cand_glyphs, cand_events;
  rm_status snap_st =
      collect_scene_snapshot(bytes, out_size, &cand_scene, &cand_strokes,
                             &cand_images, &cand_glyphs, &cand_events, error);
  if (snap_st != RM_OK) {
    free(bytes);
    destroy_scene_snapshot(&orig_scene, &orig_strokes, &orig_images,
                           &orig_glyphs, &orig_events);
    return rm_edit_result(error, snap_st);
  }

  rm_crdt_id cparent = rm_crdt_of(parent);
  rm_crdt_id cid = rm_crdt_of(id);
  rm_crdt_id cleft = rm_crdt_of(left);
  rm_crdt_id cright = rm_crdt_of(right);
  const rmi_scene_event* orig_ev = orig_events.data;
  const rmi_scene_event* cand_ev = cand_events.data;
  /* Exactly one event more than before, and that event is the new stroke
   * under its parent, present once. */
  bool verified = cand_events.count == orig_events.count + 1u;
  size_t found_at = SIZE_MAX;

  for (size_t i = 0; verified && i < cand_events.count; ++i) {
    if (rm_crdt_equal(cand_ev[i].parent, cparent) &&
        rm_crdt_equal(cand_ev[i].id, cid) && cand_ev[i].kind == ITEM_STROKE) {
      if (found_at != SIZE_MAX) {
        verified = false;
        break;
      }
      found_at = i;
    }
  }
  if (found_at == SIZE_MAX) verified = false;
  /* With the new stroke skipped, every other item keeps its order. */
  if (verified) {
    for (size_t oi = 0, ci = 0; oi < orig_events.count; ++oi, ++ci) {
      if (ci == found_at) ++ci;
      if (!events_equal(orig_ev[oi], cand_ev[ci])) {
        verified = false;
        break;
      }
    }
  }
  /* The requested neighbours must draw on either side of the new stroke. */
  if (verified && !zero_id(left)) {
    bool saw_left = false;

    for (size_t i = 0; i < found_at; ++i) {
      if (rm_crdt_equal(cand_ev[i].parent, cparent) &&
          rm_crdt_equal(cand_ev[i].id, cleft)) {
        saw_left = true;
        break;
      }
    }
    if (!saw_left) verified = false;
  }
  if (verified && !zero_id(right)) {
    bool saw_right = false;
    for (size_t i = found_at + 1u; i < cand_events.count; ++i) {
      if (rm_crdt_equal(cand_ev[i].parent, cparent) &&
          rm_crdt_equal(cand_ev[i].id, cright)) {
        saw_right = true;
        break;
      }
    }
    if (!saw_right) verified = false;
  }

  destroy_scene_snapshot(&cand_scene, &cand_strokes, &cand_images, &cand_glyphs,
                         &cand_events);
  destroy_scene_snapshot(&orig_scene, &orig_strokes, &orig_images, &orig_glyphs,
                         &orig_events);
  if (!verified) {
    free(bytes);
    return rm_edit_result(error, RM_ERR_UNVERIFIED);
  }
  out->bytes = bytes;
  out->size = out_size;
  out->verified = false;
  return RM_OK;
}

/* A field after the points in a line's value, as a byte range of the value.
 * Tag and type zero mark bytes that would not read as a field; they run to
 * the end of the value. */
typedef struct line_tail_field {
  uint32_t tag;
  uint8_t type;
  size_t start;
  size_t end;
} line_tail_field;

/* A decoded stroke. The readers are views into the page's bytes: `value` is
 * the whole value, `points` its samples, `item_tail` whatever the item holds
 * after the value. `thickness_offset` is where the thickness is stored, so the
 * value's leading bytes can be copied up to it. `stride` is the size of one
 * sample. */
typedef struct line {
  rm_item_preamble item;
  rm_reader value, points, item_tail;
  rm_vec tail_fields; /* line_tail_field */
  uint32_t tool;
  rm_crdt_id timestamp, move, source;
  double thickness;
  float start;
  bool timestamp_present, move_present, argb_present, source_present;
  uint32_t argb;
  size_t thickness_offset, stride;
} line;

static void line_free(line* l) { rm_vec_free(&l->tail_fields); }

/* Keep the raw range of each tail field so rebuilding a cut stroke can
 * preserve unfamiliar bytes in their original order. */
static rm_status push_tail_field(line* l, uint32_t tag, uint8_t type,
                                 size_t start, size_t end) {
  line_tail_field f = {.tag = tag, .type = type, .start = start, .end = end};
  return rm_vec_push(&l->tail_fields, &f) ? RM_OK : RM_ERR_OUT_OF_MEMORY;
}

/* Keep views into the original point data and tail. A cut can copy whole
 * samples without round-tripping the tablet's pen data through floats.
 *
 * Returns RM_ERR_ITEMS_NOT_FOUND for a tombstone and RM_ERR_UNVERIFIED for a
 * value this code does not know how to rewrite. The caller frees `l` with
 * line_free whatever the result. */
static rm_status decode_line(rm_reader body, rm_block_header header, line* l) {
  rm_vec_init(&l->tail_fields, sizeof(line_tail_field));
  RM_TRY(rm_parse_item_preamble(&body, &l->item));
  if (l->item.deleted || !rm_match_tag(&body, 6, RM_TAG_LENGTH4))
    return RM_ERR_ITEMS_NOT_FOUND;
  RM_TRY(rm_read_subblock(&body, &l->value));
  l->item_tail = body;
  rm_reader r = l->value;
  uint8_t kind;
  uint32_t color;
  RM_TRY(rm_read_u8(&r, &kind));

  if (kind != RM_VALUE_LINE_ITEM || !header.current_version ||
      header.min_version > 2)
    return RM_ERR_UNVERIFIED;
  RM_TRY(rm_expect_tag(&r, 1, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&r, &l->tool));
  RM_TRY(rm_expect_tag(&r, 2, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&r, &color));
  RM_TRY(rm_expect_tag(&r, 3, RM_TAG_BYTE8));
  l->thickness_offset = r.offset;
  RM_TRY(rm_read_f64(&r, &l->thickness));
  RM_TRY(rm_expect_tag(&r, 4, RM_TAG_BYTE4));
  RM_TRY(rm_read_f32(&r, &l->start));
  RM_TRY(rm_expect_tag(&r, 5, RM_TAG_LENGTH4));
  RM_TRY(rm_read_subblock(&r, &l->points));
  /* Version 1 stores every sample field as a float; later versions pack the
   * fields after x and y into six bytes. */
  l->stride = header.current_version == 1 ? 24 : 14;
  if (rm_reader_remaining(&l->points) % l->stride) return RM_ERR_MALFORMED_PAGE;

  /* Parse tail fields in any order while preserving their wire positions. */
  while (rm_reader_remaining(&r) > 0) {
    size_t field_start = r.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&r, &tag, &type) != RM_OK)
      return push_tail_field(l, 0, 0, field_start, r.size);

    if (tag == 6 && type == RM_TAG_ID) {
      if (rm_read_bare_crdt(&r, &l->timestamp) != RM_OK)
        return push_tail_field(l, 0, 0, field_start, r.size);
      l->timestamp_present = true;
    } else if (tag == 7 && type == RM_TAG_ID) {
      if (rm_read_bare_crdt(&r, &l->move) != RM_OK)
        return push_tail_field(l, 0, 0, field_start, r.size);
      l->move_present = true;
    } else if (tag == 8 && type == RM_TAG_BYTE4) {
      if (rm_read_u32(&r, &l->argb) != RM_OK)
        return push_tail_field(l, 0, 0, field_start, r.size);
      l->argb_present = true;
    } else if (tag == 9 && type == RM_TAG_ID) {
      if (rm_read_bare_crdt(&r, &l->source) != RM_OK)
        return push_tail_field(l, 0, 0, field_start, r.size);
      l->source_present = true;
    } else if (rmi_skip_tagged_value(&r, type, nullptr) != RM_OK) {
      return push_tail_field(l, 0, 0, field_start, r.size);
    }
    RM_TRY(push_tail_field(l, tag, type, field_start, r.offset));
  }
  return RM_OK;
}

/* Transform the coordinates and scale the width in its original
 * representation. Speed, direction and pressure travel with the sample.
 * A coordinate or a width that would not fit its field refuses the edit
 * with RM_ERR_INVALID_ARGUMENT rather than being clamped. */
static rm_status transform_points(const line* l, rm_reader r,
                                  rm_edit_transform m, double scale,
                                  rm_buf* out) {
  while (rm_reader_remaining(&r)) {
    float x, y;
    RM_TRY(rm_read_f32(&r, &x));
    RM_TRY(rm_read_f32(&r, &y));
    double xx = m.a * x + m.c * y + m.tx, yy = m.b * x + m.d * y + m.ty;
    if (!isfinite(xx) || fabs(xx) > FLT_MAX || !isfinite(yy) ||
        fabs(yy) > FLT_MAX)
      return RM_ERR_INVALID_ARGUMENT;
    rm_write_f32(out, (float)xx);
    rm_write_f32(out, (float)yy);
    size_t before = l->stride == 24 ? 8 : 2;
    const uint8_t* bytes;
    RM_TRY(rm_read_bytes(&r, before, &bytes));
    rm_buf_append(out, bytes, before);
    if (l->stride == 24) {
      float width;
      RM_TRY(rm_read_f32(&r, &width));

      double scaled = (double)width * scale;
      if (!isfinite(scaled) || fabs(scaled) > FLT_MAX ||
          !isfinite((float)scaled * 4.0f))
        return RM_ERR_INVALID_ARGUMENT;
      rm_write_f32(out, (float)scaled);
    } else {
      uint16_t width;
      RM_TRY(rm_read_u16(&r, &width));
      double w = round(width * scale);
      if (w > UINT16_MAX) return RM_ERR_INVALID_ARGUMENT;
      rm_write_u16(out, (uint16_t)w);
    }
    size_t remaining = l->stride == 24 ? 4 : 2;
    RM_TRY(rm_read_bytes(&r, remaining, &bytes));
    rm_buf_append(out, bytes, remaining);
  }
  return rmi_buf_status(out);
}

/* Give a surviving piece its own ID. For a cut, advance the starting distance
 * along the original points so the later piece starts at the same place along
 * the stroke.
 *
 * The piece covers samples [from, to) and is appended to `file` as a new
 * block with the original's header versions. It keeps the original's parent
 * and right neighbour and every value field it can read, in their order.
 * Each sample carries its drawn width. Scale that width by the square root
 * of the area factor, |determinant|. Tag 3 records the tablet's pen setting
 * and travels with the stroke even when the drawing is resized. */
static rm_status piece(rm_buf* file, rm_block_header h, const line* l,
                       rm_crdt_id id, rm_crdt_id left, size_t from, size_t to,
                       const rm_edit_transform* transform) {
  rm_reader points = l->points;
  points.offset += from * l->stride;
  points.size = l->points.offset + to * l->stride;
  double start = isfinite(l->start) ? (double)l->start : 0.0;
  double scale = 1.0;
  if (!transform && from) {
    rm_reader r = l->points;
    float x0 = 0, y0 = 0;
    for (size_t i = 0; i <= from; ++i) {
      float x, y;
      RM_TRY(rm_read_f32(&r, &x));
      RM_TRY(rm_read_f32(&r, &y));
      if (i) start += hypot((double)x - x0, (double)y - y0);
      x0 = x;
      y0 = y;
      r.offset += l->stride - 8;
    }
  }

  if (transform) {
    scale =
        sqrt(fabs(transform->a * transform->d - transform->b * transform->c));
    if (scale == 0.0) return RM_ERR_INVALID_ARGUMENT;
  }
  if (!isfinite(start) || fabs(start) > FLT_MAX || !isfinite(scale))
    return RM_ERR_INVALID_ARGUMENT;
  /* A live item: preamble with deletion count zero, then the value up to the
   * thickness copied as it was, then the new start distance and samples. */
  rm_buf body = {0};
  rm_status status = RM_OK;
  rm_write_id(&body, 1, l->item.parent);
  rm_write_id(&body, 2, id);
  rm_write_id(&body, 3, left);
  rm_write_id(&body, 4, l->item.right);
  rm_write_tag(&body, 5, RM_TAG_BYTE4);
  rm_write_u32(&body, 0);
  rm_subblock_mark value = rm_buf_begin_subblock(&body, 6);
  rm_buf_append(&body, l->value.data + l->value.offset,
                l->thickness_offset - l->value.offset);
  rm_write_f64(&body, l->thickness);
  rm_write_tag(&body, 4, RM_TAG_BYTE4);
  rm_write_f32(&body, (float)start);

  rm_subblock_mark samples = rm_buf_begin_subblock(&body, 5);
  if (transform)
    status = transform_points(l, points, *transform, scale, &body);
  else
    rm_buf_append(&body, points.data + points.offset,
                  rm_reader_remaining(&points));
  rm_buf_end_subblock(&body, samples);
  if (status == RM_OK) {
    /* A moved piece names in tag 7 the stroke it was moved from, carrying
     * over an earlier origin if the original already had one. A cut shader
     * stroke names its source in tag 9 the same way. Each replaces the last
     * such field in place, or is added after the fields that read. */
    bool want_move = transform != nullptr;
    bool shader_cut = !transform && l->tool == RM_TOOL_SHADER;
    rm_crdt_id new_move =
        rm_crdt_equal(l->move, RM_CRDT_NONE) ? l->item.id : l->move;
    rm_crdt_id new_src =
        (l->source_present && !rm_crdt_equal(l->source, RM_CRDT_NONE))
            ? l->source
            : l->item.id;

    const line_tail_field* f = l->tail_fields.data;
    size_t n = l->tail_fields.count;
    size_t last_move = SIZE_MAX, last_src = SIZE_MAX, opaque = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
      if (f[i].tag == 0 && f[i].type == 0) {
        opaque = i;
        break;
      }
      if (f[i].tag == 7 && f[i].type == RM_TAG_ID) last_move = i;
      if (f[i].tag == 9 && f[i].type == RM_TAG_ID) last_src = i;
    }
    size_t complete = opaque == SIZE_MAX ? n : opaque;

    /* Unreadable bytes, if any, stay last, after any field added here. */
    for (size_t i = 0; i < complete; ++i) {
      if (want_move && i == last_move)
        rm_write_id(&body, 7, new_move);
      else if (shader_cut && i == last_src)
        rm_write_id(&body, 9, new_src);
      else
        rm_buf_append(&body, l->value.data + f[i].start, f[i].end - f[i].start);
    }
    if (want_move && last_move == SIZE_MAX) rm_write_id(&body, 7, new_move);
    if (shader_cut && last_src == SIZE_MAX) rm_write_id(&body, 9, new_src);
    if (opaque != SIZE_MAX)
      rm_buf_append(&body, l->value.data + f[opaque].start,
                    f[opaque].end - f[opaque].start);
    rm_buf_end_subblock(&body, value);
    rm_buf_append(&body, l->item_tail.data + l->item_tail.offset,
                  rm_reader_remaining(&l->item_tail));
    rm_write_top_block(file, RM_BLOCK_SCENE_LINE_ITEM, h.min_version,
                       h.current_version, body.bytes, body.count);
    rmi_buf_propagate(file, &body);
    status = rmi_buf_status(file);
  }
  rm_buf_free(&body);
  return status;
}

/* A selected stroke: its decoded value and the block it was read from. */
typedef struct replacement {
  line original;
  rm_block_header block;
} replacement;

/* Replace selected blocks with tombstones where they stood. Copy the
 * intervening bytes straight through so unrelated items keep their on-disk
 * spelling.
 *
 * Only the block recorded in `first_block_starts` is replaced for each ID;
 * `targets` receives its decoded value. `r` is left at the end of the last
 * whole block and `output` holds the page up to there. */
static rm_status collect_lines(rm_reader* r, const rm_idmap* selected,
                               const size_t* first_block_starts,
                               replacement* targets, rm_buf* output) {
  size_t copied = 0, found = 0;
  size_t limit = rmi_block_limit();
  while (rm_reader_remaining(r) >= 8) {
    rm_block_header block;
    RM_TRY(rm_read_block_header(r, &block));
    rm_reader body = *r;
    body.size = block.end;
    r->offset = block.end;
    if (block.type != RM_BLOCK_SCENE_LINE_ITEM) continue;
    rm_item_preamble item;
    RM_TRY(rm_parse_item_preamble(&body, &item));
    uint64_t index;
    if (!rm_idmap_get(selected, item.id, &index)) continue;
    if (block.start != first_block_starts[index]) continue;
    replacement* target = &targets[index];
    if (target->block.end) return RM_ERR_UNVERIFIED;

    /* The deletion count is the preamble's last four bytes. The tombstone
     * keeps the preamble, sets the count to one and drops the value, the
     * shape the tablet gives an erased stroke. */
    size_t deleted_at = body.offset - 4;
    body.offset = block.body;
    RM_TRY(decode_line(body, block, &target->original));
    target->block = block;
    ++found;
    size_t new_body_len = deleted_at - block.body + 4u;
    if (new_body_len > limit) return RM_ERR_TOO_MANY_TO_NUMBER;
    rm_buf_append(output, r->data + copied, block.start - copied);
    rm_write_u32(output, (uint32_t)new_body_len);
    rm_buf_append(output, r->data + block.start + 4,
                  deleted_at - block.start - 4);
    rm_write_u32(output, 1);
    copied = block.end;
  }
  if (found != selected->count) return RM_ERR_ITEMS_NOT_FOUND;
  rm_buf_append(output, r->data + copied, r->offset - copied);
  return rmi_buf_status(output);
}

/* Walk the result to verify that replacement pieces occupy the original
 * strokes' places relative to the other ink and pictures.
 *
 * The expected stroke order is the original with each selected stroke
 * replaced by its pieces, in ID order. A picture drawn after N strokes must
 * now be drawn after N plus the number of strokes the replacements added
 * before it. */
static rm_status verify_reworked_order(
    const rmi_scene* orig_scene, const rm_vec* orig_strokes,
    const rm_vec* orig_images, const uint8_t* cand_bytes, size_t cand_size,
    const rm_idmap* selected, const rm_crdt_id* piece_first,
    const size_t* piece_count, rm_error* error) {
  rmi_scene cand_scene = {0};
  rm_vec cand_strokes, cand_images, cand_glyphs, cand_events;
  RM_TRY(collect_scene_snapshot(cand_bytes, cand_size, &cand_scene,
                                &cand_strokes, &cand_images, &cand_glyphs,
                                &cand_events, error));

  rm_status status = RM_ERR_OUT_OF_MEMORY;
  rm_vec expected_strokes;
  rm_vec_init(&expected_strokes, sizeof(rmi_scene_event));
  ptrdiff_t* delta_before =
      calloc(orig_strokes->count + 1u, sizeof *delta_before);
  if (!delta_before) goto done;

  const rmi_ordered_stroke* orig_st = orig_strokes->data;
  const rmi_scene_item* orig_items = orig_scene->items.data;
  for (size_t s = 0; s < orig_strokes->count; ++s) {
    rm_crdt_id orig_id = orig_items[orig_st[s].item].id;
    uint64_t idx = 0;
    ptrdiff_t d = 0;
    if (rm_idmap_get(selected, orig_id, &idx)) {
      size_t pc = piece_count[idx];
      d = (ptrdiff_t)pc - 1;

      for (size_t k = 0; k < pc; ++k) {
        rmi_scene_event ev = {
            .parent = orig_st[s].parent,
            .id = rm_crdt_make(piece_first[idx].author,
                               piece_first[idx].counter + (uint64_t)k),
            .kind = ITEM_STROKE,
        };
        if (!rm_vec_push(&expected_strokes, &ev)) goto done;
      }
    } else {
      rmi_scene_event ev = {
          .parent = orig_st[s].parent, .id = orig_id, .kind = ITEM_STROKE};
      if (!rm_vec_push(&expected_strokes, &ev)) goto done;
    }
    delta_before[s + 1u] = delta_before[s] + d;
  }

  const rmi_ordered_stroke* cand_st = cand_strokes.data;
  const rmi_scene_item* cand_items = cand_scene.items.data;
  const rmi_scene_event* exp_st = expected_strokes.data;
  bool ok = cand_strokes.count == expected_strokes.count &&
            cand_images.count == orig_images->count;
  for (size_t s = 0; ok && s < expected_strokes.count; ++s) {
    if (!rm_crdt_equal(cand_st[s].parent, exp_st[s].parent) ||
        !rm_crdt_equal(cand_items[cand_st[s].item].id, exp_st[s].id))
      ok = false;
  }
  const rmi_placed_image* orig_im = orig_images->data;
  const rmi_placed_image* cand_im = cand_images.data;

  for (size_t m = 0; ok && m < orig_images->count; ++m) {
    if (orig_im[m].strokes_before > orig_strokes->count) {
      ok = false;
      break;
    }
    ptrdiff_t adj = (ptrdiff_t)orig_im[m].strokes_before +
                    delta_before[orig_im[m].strokes_before];
    if (adj < 0 || (size_t)adj != cand_im[m].strokes_before ||
        !rm_crdt_equal(cand_im[m].parent, orig_im[m].parent) ||
        !rm_crdt_equal(cand_items[cand_im[m].item].id,
                       orig_items[orig_im[m].item].id)) {
      ok = false;
    }
  }
  status = ok ? RM_OK : RM_ERR_UNVERIFIED;

done:
  free(delta_before);
  rm_vec_free(&expected_strokes);
  destroy_scene_snapshot(&cand_scene, &cand_strokes, &cand_images, &cand_glyphs,
                         &cand_events);
  return status;
}

/* A transform must be finite and invertible: a singular one would flatten
 * the ink onto a line, and its scale factor for widths would be zero. */
bool rmi_edit_transform_valid(rm_edit_transform transform) {
  if (!isfinite(transform.a) || !isfinite(transform.b) ||
      !isfinite(transform.c) || !isfinite(transform.d) ||
      !isfinite(transform.tx) || !isfinite(transform.ty))
    return false;

  double det = transform.a * transform.d - transform.b * transform.c;
  return isfinite(det) && det != 0.0;
}

/* Nothing moved or cut. The candidate owns a copy of the session's already
 * verified page, including any bytes after its last block. */
static rm_status copy_unchanged_page(const uint8_t* data, size_t size,
                                     rmi_edit_candidate* out) {
  uint8_t* copy = malloc(size);
  if (!copy) return RM_ERR_OUT_OF_MEMORY;

  memcpy(copy, data, size);
  *out = (rmi_edit_candidate){.bytes = copy, .size = size, .verified = true};
  return RM_OK;
}

/* Resolve all selected strokes before making any pieces. Otherwise replacing
 * one could change the successor seen by the next.
 *
 * With a transform each selected stroke becomes one moved piece, taking
 * `count` IDs from `fresh`. Without one, the single stroke loses samples
 * [first, first + removed) and keeps up to two pieces, one ID each. The
 * candidate is owned by the caller. */
static rm_status rework(const uint8_t* data, size_t size, const rm_item_id* ids,
                        size_t count, rm_item_id fresh, size_t first,
                        size_t removed, const rm_edit_transform* transform,
                        rmi_edit_candidate* out_cand, rm_error* error) {
  *out_cand = (rmi_edit_candidate){0};
  if (!data || (count && !ids) ||
      (transform && !rmi_edit_transform_valid(*transform)) ||
      (count && count - 1 > UINT64_MAX - fresh.counter))
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (size < RM_HEADER_LENGTH)
    return rm_edit_result(error, RM_ERR_UNEXPECTED_EOF);
  if (!count) return copy_unchanged_page(data, size, out_cand);

  rm_idmap selected = {0};
  rm_buf out = {0};
  replacement* targets = calloc(count, sizeof *targets);
  rm_crdt_id* successors = calloc(count, sizeof *successors);
  size_t* first_blocks = calloc(count, sizeof *first_blocks);
  rm_crdt_id* piece_first = calloc(count, sizeof *piece_first);
  size_t* piece_count = calloc(count, sizeof *piece_count);

  rmi_scene orig_scene = {0};
  rm_vec orig_strokes = {0}, orig_images = {0}, orig_glyphs = {0},
         orig_events = {0};
  bool have_orig_snap = false;
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  if (!targets || !successors || !first_blocks || !piece_first || !piece_count)
    goto done;
  /* The map's value is the ID's index in the request. */
  for (size_t i = 0; i < count; ++i) {
    rm_crdt_id id = rm_crdt_of(ids[i]);
    if (rm_idmap_contains(&selected, id)) {
      status = RM_ERR_INVALID_ARGUMENT;
      goto done;
    }
    if (!rm_idmap_set(&selected, id, i)) goto done;
  }

  /* Resolve every requested item on the immutable starting revision. */
  status = rmi_scene_resolve_stroke_targets(data, size, &selected, true,
                                            first_blocks, successors, error);
  if (status != RM_OK) goto done;

  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  status = collect_lines(&r, &selected, first_blocks, targets, &out);
  if (status != RM_OK) goto done;

  size_t needed = count;
  if (!transform) {
    const line* l = &targets[0].original;
    size_t points = rm_reader_remaining(&l->points) / l->stride;
    if (first > points || removed > points - first) {
      status = RM_ERR_INVALID_ARGUMENT;
      goto done;
    }
    /* Cutting nothing returns the page unchanged, which the session treats
     * as a no-op. */
    if (!removed) {
      status = copy_unchanged_page(data, size, out_cand);
      goto done;
    }
    /* A piece before the cut and a piece after it, when there is any. */
    needed = (size_t)(first != 0) + (first + removed < points);
  }
  status = rmi_scene_check_ids(data, size, fresh, needed, error);
  if (status != RM_OK) goto done;

  status =
      collect_scene_snapshot(data, size, &orig_scene, &orig_strokes,
                             &orig_images, &orig_glyphs, &orig_events, error);
  if (status != RM_OK) goto done;
  have_orig_snap = true;

  rm_crdt_id next = rm_crdt_of(fresh);
  for (size_t i = 0; i < count; ++i) {
    line* l = &targets[i].original;
    rm_block_header block = targets[i].block;

    /* Anchor every surviving piece between the original tombstone and the
     * next scene item, including erasers and non-stroke children. */
    l->item.right = successors[i];
    size_t points = rm_reader_remaining(&l->points) / l->stride;
    rm_crdt_id left = l->item.id;
    piece_first[i] = next;
    if (transform) {
      status = piece(&out, block, l, next, left, 0, points, transform);
      piece_count[i] = 1u;
      ++next.counter;
    } else {
      /* The second piece follows the first, so their order is fixed. */
      if (first) {
        status = piece(&out, block, l, next, left, 0, first, nullptr);
        left = next;
        piece_count[i] += 1u;
        ++next.counter;
      }

      if (status == RM_OK && first + removed < points) {
        status =
            piece(&out, block, l, next, left, first + removed, points, nullptr);
        piece_count[i] += 1u;
        ++next.counter;
      }
    }
    if (status != RM_OK) goto done;
  }
  /* The pieces were appended after the last block; trailing bytes follow. */
  rm_buf_append(&out, data + r.offset, size - r.offset);
  status = rmi_buf_status(&out);
  if (status != RM_OK) goto done;

  size_t out_size = 0;
  uint8_t* bytes = rm_buf_take(&out, &out_size);
  if (!bytes) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  status = verify_reworked_order(&orig_scene, &orig_strokes, &orig_images,
                                 bytes, out_size, &selected, piece_first,
                                 piece_count, error);
  if (status != RM_OK) {
    free(bytes);
    goto done;
  }
  out_cand->bytes = bytes;
  out_cand->size = out_size;
  out_cand->verified = false;
  status = RM_OK;

done:
  if (have_orig_snap) {
    destroy_scene_snapshot(&orig_scene, &orig_strokes, &orig_images,
                           &orig_glyphs, &orig_events);
  }
  if (targets) {
    for (size_t i = 0; i < count; ++i) line_free(&targets[i].original);
  }
  rm_idmap_free(&selected);
  rm_buf_free(&out);
  free(targets);
  free(successors);
  free(first_blocks);
  free(piece_first);
  free(piece_count);
  return status;
}

rm_status rmi_edit_transform_strokes(const uint8_t* data, size_t size,
                                     const rm_item_id* ids, size_t count,
                                     rm_item_id fresh,
                                     rm_edit_transform transform,
                                     rmi_edit_candidate* out, rm_error* error) {
  if (error) *error = (rm_error){0};
  return rm_edit_result(error, rework(data, size, ids, count, fresh, 0, 0,
                                      &transform, out, error));
}

rm_status rmi_edit_cut_stroke(const uint8_t* data, size_t size,
                              rm_item_id stroke, rm_item_id fresh, size_t first,
                              size_t count, rmi_edit_candidate* out,
                              rm_error* error) {
  if (error) *error = (rm_error){0};
  return rm_edit_result(error, rework(data, size, &stroke, 1, fresh, first,
                                      count, nullptr, out, error));
}
