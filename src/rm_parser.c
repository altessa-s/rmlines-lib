/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Public page parser and page assembly from the decoded scene. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_log_internal.h"
#include "rm_scene.h"
#include "rmlines/parser.h"

constexpr uint8_t FACE_BOLD = 1u;
constexpr uint8_t FACE_ITALIC = 2u;

/* Neighbouring characters of one face gathered into a stretch. Plain text
 * yields nothing: most of a page is plain, and an entry per line saying so
 * would be an entry for nothing. */
static rm_status build_faces(rm_arena* arena, const rmi_line_build* line,
                             const rm_face_run** out_runs, size_t* out_count) {
  *out_runs = nullptr;
  *out_count = 0;
  size_t scalars = line->faces.count;
  if (scalars == 0) return RM_OK;
  if (scalars > (size_t)INT32_MAX) return RM_ERR_MALFORMED_PAGE;
  const uint8_t* marks = line->faces.data;

  size_t runs = 0;
  for (size_t start = 0; start < scalars;) {
    uint8_t face = marks[start];
    size_t end = start + 1;
    while (end < scalars && marks[end] == face) end += 1;
    if (face != 0) runs += 1;
    start = end;
  }
  if (runs == 0) return RM_OK;

  rm_face_run* out = rm_arena_alloc(arena, runs * sizeof *out);
  if (out == nullptr) return RM_ERR_OUT_OF_MEMORY;
  size_t written = 0;
  for (size_t start = 0; start < scalars;) {
    uint8_t face = marks[start];
    size_t end = start + 1;
    while (end < scalars && marks[end] == face) end += 1;
    if (face != 0) {
      out[written++] = (rm_face_run){
          .start = (int32_t)start,
          .end = (int32_t)end,
          .bold = (face & FACE_BOLD) != 0,
          .italic = (face & FACE_ITALIC) != 0,
      };
    }
    start = end;
  }
  *out_runs = out;
  *out_count = runs;
  return RM_OK;
}

/* Which line each paragraph style belongs to.
 *
 * A style is keyed by the character that **ends the paragraph before** it —
 * the newline — and so belongs to the line after that one. The first
 * paragraph has no newline before it and is keyed by the format's
 * start-of-text sentinel. */
static rm_status apply_styles(const rmi_scene* ctx, rm_paragraph* paragraphs,
                              size_t count) {
  if (!ctx->text.styles.count || count == 0) return RM_OK;
  rm_crdt_id* stamps = calloc(count, sizeof *stamps);
  if (!stamps) return RM_ERR_OUT_OF_MEMORY;
  for (size_t i = 0; i < ctx->text.styles.count; i += 1) {
    const rmi_style_entry* entry = rm_vec_at(&ctx->text.styles, i);
    size_t line;
    if (rm_crdt_equal(entry->key, RM_CRDT_NONE)) {
      line = 0;
    } else {
      uint64_t line_u64;
      if (!rm_idmap_get(&ctx->text.style_line_of_key, entry->key, &line_u64))
        continue;
      line = (size_t)line_u64;
    }
    if (line >= count || rm_stamp_newer(stamps[line], entry->stamp)) continue;
    stamps[line] = entry->stamp;
    paragraphs[line].has_format = true;
    paragraphs[line].format = entry->format;
  }
  free(stamps);
  return RM_OK;
}

/* The page's `rm_text_block`: one `rm_paragraph` per laid-out line, its text
 * copied into the page's arena and its style and faces attached. */
static rm_status build_text_block(rmi_scene* ctx, rm_text_block* out) {
  size_t count = ctx->text.lines.count;
  rm_paragraph* paragraphs =
      rm_arena_calloc(ctx->arena, count == 0 ? 1 : count, sizeof *paragraphs);
  if (paragraphs == nullptr) return RM_ERR_OUT_OF_MEMORY;

  for (size_t i = 0; i < count; i += 1) {
    const rmi_line_build* line = rm_vec_at(&ctx->text.lines, i);
    char* text = rm_arena_strndup(ctx->arena, (const char*)line->text.bytes,
                                  line->text.count);
    if (text == nullptr) return RM_ERR_OUT_OF_MEMORY;
    paragraphs[i].text = (rm_str){.bytes = text, .length = line->text.count};
    RM_TRY(build_faces(ctx->arena, line, &paragraphs[i].faces,
                       &paragraphs[i].face_count));
  }
  RM_TRY(apply_styles(ctx, paragraphs, count));

  *out = (rm_text_block){
      .origin_x = ctx->text.origin_x,
      .origin_y = ctx->text.origin_y,
      .paragraphs = paragraphs,
      .paragraph_count = count,
      .has_column_width = ctx->text.has_column_width,
      .column_width = ctx->text.column_width,
  };
  return RM_OK;
}

/* The gathered blocks as one page. */
static rm_status assemble(rmi_scene* ctx, rm_page* page,
                          rm_idmap* out_edit_removed) {
  rm_vec strokes, images, glyphs;
  rm_vec_init(&glyphs, sizeof(rm_highlight));
  rm_vec_init(&strokes, sizeof(rmi_ordered_stroke));
  rm_vec_init(&images, sizeof(rmi_placed_image));
  rm_status status = RM_OK;

  /* The drawing order is the scene's, not the file's: layers in the chain
   * order of 0x04, strokes in their own chain within a layer. A hidden or
   * deleted layer is dropped here rather than at drawing time, so that it
   * widens no sheet and shifts no anchor either. */
  if (!rmi_scene_order_items_ex(ctx, &strokes, &images, &glyphs, false, nullptr,
                                out_edit_removed)) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }

  const rmi_scene_item* items = ctx->items.data;
  size_t drawable = 0;
  for (size_t i = 0; i < ctx->items.count; i += 1) {
    if (items[i].kind == ITEM_STROKE) drawable += 1;
  }
  if (strokes.count != drawable) {
    RM_LOG_DEBUGF("page: %zu strokes on hidden or deleted layers are not drawn",
                  drawable - strokes.count);
  }

  /* Mark the handwriting that is counted from the text with the paragraph it
   * belongs to. Where that paragraph lands is not decided here: a paragraph
   * too long for one line takes more than one step, and working out how much
   * more takes a font and a column width. Both belong to drawing, so the
   * number travels to the renderer. */
  rm_stroke* out_strokes = nullptr;
  if (strokes.count != 0) {
    out_strokes =
        rm_arena_alloc(ctx->arena, strokes.count * sizeof *out_strokes);
    if (out_strokes == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }

  size_t anchored = 0;
  for (size_t i = 0; i < strokes.count; i += 1) {
    const rmi_ordered_stroke* entry = rm_vec_at(&strokes, i);
    out_strokes[i] = items[entry->item].stroke;
    rm_text_anchor place;
    if (rmi_scene_anchor_under(ctx, entry->parent, &place)) {
      out_strokes[i].has_anchor = true;
      out_strokes[i].anchor = place;
      anchored += 1;
    }
  }

  /* The scene's images, their keys resolved to file names. A key the
   * attachment map does not know is content we cannot fetch — said out loud
   * rather than dropped in silence, because a page that quietly loses a
   * picture looks like a page that never had one. */
  rm_page_image* out_images = nullptr;
  if (images.count != 0) {
    out_images = rm_arena_calloc(ctx->arena, images.count, sizeof *out_images);
    if (out_images == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
  }
  size_t image_count = 0;

  for (size_t i = 0; i < images.count; i += 1) {
    const rmi_placed_image* entry = rm_vec_at(&images, i);
    const rm_str* name =
        rmi_scene_attachment_name(ctx, items[entry->item].image_key);
    if (name == nullptr) {
      RM_LOG_ERRORF(
          "page: an image without a name in the attachment map is lost");
      continue;
    }
    rm_page_image* image = &out_images[image_count++];
    image->file = *name;
    memcpy(image->vertices, items[entry->item].vertices,
           sizeof image->vertices);
    image->strokes_before = entry->strokes_before;

    /* An image inserted into the text flow hangs on an anchored group the way
     * handwriting does, and takes the same anchor: its vertices are counted
     * from the pen, and where the pen stands is the layout's to say. */
    image->has_anchor =
        rmi_scene_anchor_under(ctx, entry->parent, &image->anchor);
  }

  rm_highlight* out_highlights = nullptr;
  if (glyphs.count != 0) {
    out_highlights =
        rm_arena_alloc(ctx->arena, glyphs.count * sizeof *out_highlights);
    if (out_highlights == nullptr) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
    memcpy(out_highlights, glyphs.data, glyphs.count * sizeof *out_highlights);
  }

  *page = (rm_page){
      .strokes = out_strokes,
      .stroke_count = strokes.count,
      .highlights = out_highlights,
      .highlight_count = glyphs.count,
      .images = out_images,
      .image_count = image_count,
      .anchored_strokes = anchored,
      /* Deleted items do not count as skipped: the user erased them, and a
       * "loss" here would be a false alarm. */
      .skipped_blocks = ctx->unreadable_strokes + ctx->other_blocks,
      .has_paper_size = ctx->has_paper_size,
      .paper_size = ctx->paper_size,
      .background_visible = ctx->background_visible,
      .root_document_visible = ctx->root_document_visible,
      .arena = ctx->arena,
  };
  if (ctx->has_text) {
    status = build_text_block(ctx, &page->text);
    page->has_text = status == RM_OK;
  }

done:
  rm_vec_free(&strokes);
  rm_vec_free(&images);
  rm_vec_free(&glyphs);
  return status;
}

/* A paragraph containing bidirectional text needs a more conservative check
 * before reusing a highlight's rectangles. */
static bool is_bidi_scalar(uint32_t cp) {
  return (cp >= 0x0590u && cp <= 0x08FFu) || (cp >= 0xFB1Du && cp <= 0xFDFFu) ||
         (cp >= 0xFE70u && cp <= 0xFEFFu) ||
         (cp >= 0x10800u && cp <= 0x10FFFu) ||
         (cp >= 0x1E800u && cp <= 0x1EFFFu) || cp == 0x061Cu || cp == 0x200Eu ||
         cp == 0x200Fu || (cp >= 0x202Au && cp <= 0x202Eu) ||
         (cp >= 0x2066u && cp <= 0x2069u);
}

void rmi_text_flat_map_free(rmi_text_flat_map* map) {
  if (map == nullptr) return;
  free(map->ids);
  free(map->scalars);
  free(map->faces);
  free(map->para_idx);
  free(map->is_inline_image);
  free(map->byte_at);
  free(map->utf8);
  free(map->paras);
  rm_idmap_free(&map->pos_of);
  rm_idmap_free(&map->ambiguous_ids);
  rm_idmap_free(&map->newline_ambiguous_ids);
  *map = (rmi_text_flat_map){0};
}

/* Keep the text, byte offsets and character IDs in the same order as the
 * public paragraphs. Range edits use all three to verify which characters
 * survived. */
static rm_status build_text_flat_map(const rmi_scene* ctx, const rm_page* page,
                                     rmi_text_flat_map* out) {
  *out = (rmi_text_flat_map){0};
  rm_idmap_init(&out->pos_of);
  rm_idmap_init(&out->ambiguous_ids);
  rm_idmap_init(&out->newline_ambiguous_ids);
  if (!page->has_text) return RM_OK;

  rm_idmap seen_nl = {0};
  rm_idmap_init(&seen_nl);
  rm_status status = RM_ERR_OUT_OF_MEMORY;

  size_t para_count = page->text.paragraph_count;
  size_t total_scalars = ctx->text.flat_ids.count;
  out->para_count = para_count;
  out->count = total_scalars;
  out->paras = calloc(para_count == 0 ? 1u : para_count, sizeof *out->paras);
  out->ids = calloc(total_scalars == 0 ? 1u : total_scalars, sizeof *out->ids);
  out->scalars =
      calloc(total_scalars == 0 ? 1u : total_scalars, sizeof *out->scalars);
  out->faces =
      calloc(total_scalars == 0 ? 1u : total_scalars, sizeof *out->faces);
  out->para_idx =
      calloc(total_scalars == 0 ? 1u : total_scalars, sizeof *out->para_idx);
  out->is_inline_image = calloc(total_scalars == 0 ? 1u : total_scalars,
                                sizeof *out->is_inline_image);
  out->byte_at = calloc(total_scalars + 1u, sizeof *out->byte_at);
  if (!out->paras || !out->ids || !out->scalars || !out->faces ||
      !out->para_idx || !out->is_inline_image || !out->byte_at)
    goto fail;

  size_t total_bytes = 0;
  for (size_t p = 0; p < para_count; p += 1) {
    out->paras[p].has_format = page->text.paragraphs[p].has_format;
    out->paras[p].format = page->text.paragraphs[p].format;
    if (page->text.paragraphs[p].text.length > SIZE_MAX - total_bytes) {
      status = RM_ERR_TOO_MANY_TO_NUMBER;
      goto fail;
    }
    total_bytes += page->text.paragraphs[p].text.length;
    if (p + 1u < para_count) {
      if (total_bytes == SIZE_MAX) {
        status = RM_ERR_TOO_MANY_TO_NUMBER;
        goto fail;
      }
      total_bytes += 1u;
    }
  }
  if (total_bytes == SIZE_MAX) {
    status = RM_ERR_TOO_MANY_TO_NUMBER;
    goto fail;
  }
  out->utf8 = malloc(total_bytes + 1u);
  if (!out->utf8) goto fail;

  const rm_crdt_id* flat_ids = ctx->text.flat_ids.data;
  const uint8_t* flat_inline = ctx->text.flat_inline_image.data;
  size_t k = 0;
  size_t byte_pos = 0;
  for (size_t p = 0; p < para_count; p += 1) {
    const rm_paragraph* para = &page->text.paragraphs[p];
    const rmi_line_build* line = rm_vec_at(&ctx->text.lines, p);
    const uint8_t* faces = line->faces.data;
    size_t scalar_in_para = 0;
    size_t at = 0;
    while (at < para->text.length) {
      uint32_t scalar = 0;
      size_t next =
          rm_utf8_next(para->text.bytes, para->text.length, at, &scalar);
      if (k >= total_scalars) {
        status = RM_ERR_UNVERIFIED;
        goto fail;
      }

      rm_crdt_id id = flat_ids[k];
      out->ids[k] = id;
      out->scalars[k] = scalar;
      out->faces[k] =
          scalar_in_para < line->faces.count ? faces[scalar_in_para] : 0u;
      out->para_idx[k] = p;
      out->is_inline_image[k] = flat_inline[k] != 0;
      if (is_bidi_scalar(scalar)) out->paras[p].has_bidi = true;
      out->byte_at[k] = byte_pos;
      memcpy(out->utf8 + byte_pos, para->text.bytes + at, next - at);
      byte_pos += next - at;
      if (!rm_crdt_equal(id, RM_CRDT_NONE)) {
        if (rm_idmap_contains(&out->pos_of, id)) {
          if (!rm_idmap_set(&out->ambiguous_ids, id, 1)) goto fail;
        } else if (!rm_idmap_set(&out->pos_of, id, k)) {
          goto fail;
        }
      }

      scalar_in_para += 1u;
      k += 1u;
      at = next;
    }
    if (p + 1u < para_count) {
      if (k >= total_scalars) {
        status = RM_ERR_UNVERIFIED;
        goto fail;
      }

      rm_crdt_id id = flat_ids[k];
      out->ids[k] = id;
      out->scalars[k] = '\n';
      out->faces[k] = 0u;
      out->para_idx[k] = p;
      out->is_inline_image[k] = false;
      out->byte_at[k] = byte_pos;
      out->utf8[byte_pos++] = '\n';
      if (!rm_crdt_equal(id, RM_CRDT_NONE)) {
        if (rm_idmap_contains(&out->pos_of, id)) {
          if (!rm_idmap_set(&out->ambiguous_ids, id, 1)) goto fail;
        } else if (!rm_idmap_set(&out->pos_of, id, k)) {
          goto fail;
        }
        if (rm_idmap_contains(&seen_nl, id)) {
          if (!rm_idmap_set(&out->newline_ambiguous_ids, id, 1)) goto fail;
        } else if (!rm_idmap_set(&seen_nl, id, 1)) {
          goto fail;
        }
      }
      k += 1u;
    }
  }

  out->byte_at[k] = byte_pos;
  out->utf8[byte_pos] = '\0';
  out->count = k;
  size_t pcursor = 0;
  rm_crdt_id pid;
  uint64_t pval;
  while (rm_idmap_next(&ctx->text.place_ambiguous, &pcursor, &pid, &pval)) {
    if (!rm_idmap_set(&out->ambiguous_ids, pid, 1)) goto fail;
  }
  rm_idmap_free(&seen_nl);
  return RM_OK;

fail:
  rm_idmap_free(&seen_nl);
  rmi_text_flat_map_free(out);
  return status;
}

/* The edit map and removed nodes must describe the same revision as the page.
 * Gather them during the parse that supplies its paragraphs. */
rm_status rmi_parse_page_with_edit_state(const uint8_t* data, size_t size,
                                         rm_page** out_page,
                                         rmi_text_flat_map* out_map,
                                         rm_idmap* out_edit_removed,
                                         rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (out_map != nullptr) *out_map = (rmi_text_flat_map){0};
  if (out_edit_removed != nullptr) rm_idmap_init(out_edit_removed);
  if (data == nullptr || out_page == nullptr) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }

  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks_ex(data, size, out_map != nullptr, &ctx,
                                  out_error));

  /* Only the unread must not be passed over in silence: that is a missing
   * piece of what was written. Deleted items alone do not call for a line —
   * there are many of them and they are normal — though their count rides
   * along when one is written. The other blocks are counted in
   * `skipped_blocks` on the same terms as a stroke, so they are said on the
   * same terms too: a lost image or an unhooked layer that reached the count
   * and not the log would be found months later, in somebody's page. */
  if (ctx.unreadable_strokes + ctx.other_blocks > 0) {
    RM_LOG_ERRORF(
        "page: %zu scene items, %zu unreadable, %zu deleted, %zu other blocks; "
        "first failure: %s",
        ctx.items.count, ctx.unreadable_strokes, ctx.deleted_items,
        ctx.other_blocks,
        ctx.first_failure[0] == '\0' ? "-" : ctx.first_failure);
  }

  rm_status status;
  rm_page* page = rm_arena_alloc(ctx.arena, sizeof *page);
  if (page == nullptr) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto fail;
  }
  memset(page, 0, sizeof *page);
  status = assemble(&ctx, page, out_edit_removed);
  if (status != RM_OK) goto fail;
  if (out_map != nullptr) {
    status = build_text_flat_map(&ctx, page, out_map);
    if (status != RM_OK) goto fail;
  }

  rmi_scene_free_scratch(&ctx);
  *out_page = page;
  return RM_OK;

fail:
  if (out_edit_removed != nullptr) rm_idmap_free(out_edit_removed);
  if (out_map != nullptr) rmi_text_flat_map_free(out_map);
  rmi_scene_destroy(&ctx);
  rmi_error_set(out_error, status, 0, 0, nullptr);
  return status;
}

rm_status rm_parse_page(const uint8_t* data, size_t size, rm_page** out_page,
                        rm_error* out_error) {
  return rmi_parse_page_with_edit_state(data, size, out_page, nullptr, nullptr,
                                        out_error);
}

void rm_page_free(rm_page* page) {
  if (page == nullptr) return;

  /* The page is itself inside the arena, so this is the last read of it. */
  rm_arena_destroy(page->arena);
}

rm_status rm_page_set_template(rm_page* page, const char* name) {
  if (page == nullptr) return RM_ERR_INVALID_ARGUMENT;
  if (name == nullptr) {
    page->has_template_name = false;
    page->template_name = (rm_str){.bytes = "", .length = 0};
    return RM_OK;
  }
  size_t raw_length = strlen(name);
  size_t bound = rm_utf8_repair_bound(raw_length);
  if (!bound) return RM_ERR_OUT_OF_MEMORY;
  char* copy = rm_arena_alloc(page->arena, bound);
  if (copy == nullptr) return RM_ERR_OUT_OF_MEMORY;
  size_t repaired = rm_utf8_repair(name, raw_length, copy);
  page->has_template_name = true;
  page->template_name = (rm_str){.bytes = copy, .length = repaired};
  return RM_OK;
}
