/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* RootText replacement, style remapping, and live anchor remapping. */

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rm_edit_ops.h"
#include "rm_scene.h"
#include "rm_wire_write.h"
#include "rmlines/parser.h"

#ifdef RMLINES_TESTING
static _Thread_local size_t rm_test_glyph_check_count = 0;

void rm_test_reset_glyph_checks(void) { rm_test_glyph_check_count = 0; }

size_t rm_test_glyph_checks(void) { return rm_test_glyph_check_count; }
#endif

/* The text rows of one RootText block, read for rewriting. The readers and
 * `*_start` offsets locate each nested container (the lists, the first items
 * container, its row list) so each can be rebuilt with a new length while
 * its siblings are copied. `rows_suffix_start` is where rows this code did not
 * decode begin; `surplus` is how many of the declared rows they still hold.
 * `raw` and `has_value_suffix` are per decoded row; `records` are the text
 * runs split from them, each naming its row in `source`, and `order` is their
 * reading order. */
typedef struct text_rows {
  rm_reader root, lists, items, rows;
  bool has_lists, has_items, has_rows, stopped_early;
  size_t lists_start, items_start, rows_start, rows_suffix_start;
  size_t declared_count, decoded_count, surplus;
  rm_arena* arena;
  rm_reader* raw;
  bool* has_value_suffix;
  text_item* records;
  uint32_t* order;
  size_t count;
} text_rows;

/* Read the length-prefixed field `index`, which must come next. */
static rm_status sub(rm_reader* r, uint32_t index, rm_reader* child) {
  RM_TRY(rm_expect_tag(r, index, RM_TAG_LENGTH4));
  return rm_read_subblock(r, child);
}

/* Read the row as the page reader would, and remember any bytes beyond the
 * known value. The editor must preserve those with their row. */
static rm_status row_value(rm_reader row, text_item* item, rm_arena* arena,
                           bool* out_has_value_suffix) {
  *item = (text_item){0};
  *out_has_value_suffix = false;
  RM_TRY(rm_read_crdt(&row, 2, &item->id));
  RM_TRY(rm_read_crdt(&row, 3, &item->left));
  RM_TRY(rm_read_crdt(&row, 4, &item->right));
  RM_TRY(rm_expect_tag(&row, 5, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&row, &item->deleted));
  item->text = (rm_str){.bytes = "", .length = 0};
  if (item->deleted != 0) {
    *out_has_value_suffix = rm_reader_remaining(&row) > 0;
    return RM_OK;
  }
  return rmi_decode_text_row_tag6(arena, &row, item, out_has_value_suffix);
}

/* An extension can end the visible list while bounded rows still follow it.
 * Those rows make the unread part unsafe to renumber. */
static bool suffix_contains_bounded_rows(rm_reader list, size_t list_end) {
  while (list.offset < list_end) {
    size_t row_start = list.offset;
    rm_reader row = {0};
    rmi_row_envelope_status env =
        rmi_next_text_row_envelope(&list, list_end, &row_start, &row);
    if (env == RM_ROW_EOF || env == RM_ROW_SHORT_PREFIX) break;
    if (env == RM_ROW_OK) return true;
    list.offset = row_start;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&list, &tag, &type) != RM_OK) break;
    if (tag == 0 && type == RM_TAG_LENGTH4) break;
    if (rmi_skip_tagged_value(&list, type, nullptr) != RM_OK) break;
  }
  return false;
}

/* Keep each raw row alongside its decoded text. An edit outside that row can
 * then copy its original bytes, including fields this library has never seen.
 *
 * A root without lists, items or rows is not an error: the flags say which
 * containers exist and the rewrite creates the rest. A row list that will not
 * read where the page reader expects one is RM_ERR_MALFORMED_PAGE. */
static rm_status load_rows(text_rows* t, rm_reader root) {
  t->root = root;
  RM_TRY(rm_skip_crdt(&root, 1));
  /* Find the lists (tag 2) among the root's fields. Four-byte fields are
   * stepped over; anything else ends the walk where the reader stops. */
  t->lists_start = root.offset;
  while (root.offset < root.size) {
    size_t saved = root.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&root, &tag, &type) != RM_OK) {
      root.offset = saved;
      break;
    }
    if (type == RM_TAG_BYTE4) {
      const uint8_t* ignored;
      if (rm_read_bytes(&root, 4, &ignored) != RM_OK) {
        root.offset = saved;
        break;
      }
      continue;
    }
    if (type != RM_TAG_LENGTH4) {
      root.offset = saved;
      break;
    }

    uint32_t len;
    size_t sub_end;
    if (rm_read_u32(&root, &len) != RM_OK ||
        !rm_fits_within(&root, len, root.size, &sub_end)) {
      if (tag == 2) return RM_ERR_MALFORMED_PAGE;
      root.offset = saved;
      break;
    }
    if (tag == 2) {
      t->has_lists = true;
      t->lists_start = saved;
      t->lists = root;
      t->lists.size = sub_end;
    }
    root.offset = sub_end;
  }
  if (!t->has_lists) {
    t->lists_start = root.offset;
    return RM_OK;
  }

  rm_reader lists = t->lists;
  t->items_start = lists.offset;
  if (!rm_match_tag(&lists, 1, RM_TAG_LENGTH4)) return RM_OK;
  if (rm_read_subblock(&lists, &t->items) != RM_OK)
    return RM_ERR_MALFORMED_PAGE;
  t->has_items = true;

  rm_reader items = t->items;
  t->rows_start = items.offset;
  if (!rm_match_tag(&items, 1, RM_TAG_LENGTH4)) return RM_OK;
  if (rm_read_subblock(&items, &t->rows) != RM_OK) return RM_ERR_MALFORMED_PAGE;
  t->has_rows = true;

  rm_reader rows = t->rows;
  if (rm_read_count(&rows, &t->declared_count) != RM_OK)
    return RM_ERR_MALFORMED_PAGE;

  rm_vec raw_vec, rec_vec, suf_vec;
  rm_vec_init(&raw_vec, sizeof(rm_reader));
  rm_vec_init(&rec_vec, sizeof(text_item));
  rm_vec_init(&suf_vec, sizeof(uint8_t));
  t->arena = rm_arena_create();
  if (!t->arena) return RM_ERR_OUT_OF_MEMORY;

  rm_status status = RM_OK;
  for (size_t i = 0; i < t->declared_count; ++i) {
    size_t row_start = rows.offset;
    rm_reader raw_row = {0};
    rmi_row_envelope_status env =
        rmi_next_text_row_envelope(&rows, rows.size, &row_start, &raw_row);
    if (env == RM_ROW_EOF) {
      t->rows_suffix_start = rows.size;
      break;
    }
    if (env == RM_ROW_SHORT_PREFIX) {
      status = RM_ERR_MALFORMED_PAGE;
      goto done_vecs;
    }

    if (env == RM_ROW_STOP) {
      rows.offset = row_start;
      t->rows_suffix_start = row_start;
      t->stopped_early = true;
      break;
    }
    text_item item;
    bool has_suf = false;
    status = row_value(raw_row, &item, t->arena, &has_suf);
    if (status != RM_OK) {
      if (status != RM_ERR_OUT_OF_MEMORY) status = RM_ERR_MALFORMED_PAGE;
      goto done_vecs;
    }

    uint8_t suf_u8 = has_suf ? 1u : 0u;
    rm_reader raw_wire = {
        .data = rows.data, .offset = row_start, .size = rows.offset};
    if (!rm_vec_push(&raw_vec, &raw_wire) || !rm_vec_push(&rec_vec, &item) ||
        !rm_vec_push(&suf_vec, &suf_u8)) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done_vecs;
    }
    t->decoded_count += 1u;
  }
  /* When readable rows follow an extension, they still count towards the
   * declared total: the rewrite keeps them, and their number, after the
   * rows it writes. */
  if (!t->stopped_early) t->rows_suffix_start = rows.offset;
  if (t->stopped_early && t->rows_suffix_start < rows.size &&
      suffix_contains_bounded_rows(rows, rows.size)) {
    status =
        rmi_text_row_surplus(t->declared_count, t->decoded_count, &t->surplus);
    if (status != RM_OK) goto done_vecs;
  }

  /* Split rows into the runs the page reader orders, so an edit can place
   * itself between any two characters. */
  rm_vec parts;
  rm_vec_init(&parts, sizeof(text_item));
  status = rm_split_text_runs_checked(rec_vec.data, rec_vec.count, &parts);
  if (status != RM_OK) {
    rm_vec_free(&parts);
    goto done_vecs;
  }

  t->raw = raw_vec.data;
  raw_vec.data = nullptr;
  t->has_value_suffix =
      calloc(suf_vec.count ? suf_vec.count : 1u, sizeof(bool));
  if (!t->has_value_suffix) {
    rm_vec_free(&parts);
    status = RM_ERR_OUT_OF_MEMORY;
    goto done_vecs;
  }
  const uint8_t* sufs = suf_vec.data;
  for (size_t i = 0; i < suf_vec.count; ++i)
    t->has_value_suffix[i] = sufs[i] != 0;

  t->records = parts.data;
  t->count = parts.count;
  if (!rm_text_record_order(t->records, t->count, &t->order)) {
    status = RM_ERR_OUT_OF_MEMORY;
  }

done_vecs:
  rm_vec_free(&raw_vec);
  rm_vec_free(&rec_vec);
  rm_vec_free(&suf_vec);
  return status;
}

/* The ID `distance` characters into a run that starts at `id`: a run of text
 * takes consecutive counters, one per scalar. */
static rm_crdt_id advanced(rm_crdt_id id, size_t distance) {
  id.counter += distance;
  return id;
}

/* The new row list. `style_offsets` maps a removed newline's ID to the
 * offset of the inserted newline paired with it; `removed_newlines` holds every
 * newline the edit removed. */
typedef struct rewritten {
  rm_buf bytes;
  size_t count;
  rm_idmap style_offsets;
  rm_idmap removed_newlines;
} rewritten;

/* A deleted row retains its ID and neighbours. Its deleted length takes the
 * place of the text value. A live row's value is the text as a string with
 * the format's one-byte encoding marker. */
static void emit_row(rewritten* out, rm_crdt_id id, rm_crdt_id left,
                     rm_crdt_id right, uint32_t deleted, rm_str text) {
  rm_subblock_mark row = rm_buf_begin_subblock(&out->bytes, 0);
  rm_write_id(&out->bytes, 2, id);
  rm_write_id(&out->bytes, 3, left);
  rm_write_id(&out->bytes, 4, right);
  rm_write_tag(&out->bytes, 5, RM_TAG_BYTE4);
  rm_write_u32(&out->bytes, deleted);
  if (!deleted) {
    rm_subblock_mark value = rm_buf_begin_subblock(&out->bytes, 6);
    rm_write_varuint(&out->bytes, text.length);
    rm_buf_append_byte(&out->bytes, 1);
    rm_buf_append(&out->bytes, text.bytes, text.length);
    rm_buf_end_subblock(&out->bytes, value);
  }
  rm_buf_end_subblock(&out->bytes, row);
  ++out->count;
}

/* Several fragments can refer to one extended row. Copy the raw row once so
 * its suffix stays with its original owner. */
static void copy_row(rewritten* out, const text_item* r, rm_reader raw,
                     bool is_ext_row, bool* emitted_raw) {
  if (is_ext_row) {
    if (emitted_raw[r->source]) return;
    emitted_raw[r->source] = true;
  } else if (r->fragment) {
    emit_row(out, r->id, r->left, r->right, r->deleted, r->text);
    return;
  }
  rm_buf_append(&out->bytes, raw.data + raw.offset, rm_reader_remaining(&raw));
  ++out->count;
}

/* The byte offset of scalar number `scalar`, or the length if past the end. */
static size_t byte_at(rm_str s, size_t scalar) {
  size_t n = 0;
  while (scalar-- && n < s.length)
    n = rm_utf8_next(s.bytes, s.length, n, nullptr);
  return n;
}

/* A marker reserves an ID. Only an inline image also occupies a visible
 * character in the replacement range. */
static size_t text_row_vis_len(const text_item* r) {
  if (r->deleted || r->opaque_span) return 0;
  if (r->has_marker) return r->marker == UINT32_MAX ? 1u : 0u;
  return rm_utf8_scalar_count(r->text.bytes, r->text.length);
}

/* Locate the edit in reading order. Leave tombstones for removed IDs and give
 * the inserted scalars a new run so existing links still have somewhere to
 * land.
 *
 * `start` and `end` are visible scalars. `removed` receives each removed ID
 * with its offset from `start`, so a later pass can move a reference to the
 * inserted character at the same offset. */
static rm_status rewrite_rows(const text_rows* t, size_t start, size_t end,
                              rm_str insertion, rm_crdt_id fresh,
                              rm_idmap* removed, rewritten* out) {
  size_t src_cap = t->decoded_count ? t->decoded_count : 1u;
  bool* emitted_raw = calloc(src_cap, sizeof *emitted_raw);
  size_t* src_total_vis = calloc(src_cap, sizeof *src_total_vis);
  size_t* src_del_vis = calloc(src_cap, sizeof *src_del_vis);
  if (!emitted_raw || !src_total_vis || !src_del_vis) {
    free(emitted_raw);
    free(src_total_vis);
    free(src_del_vis);
    return RM_ERR_OUT_OF_MEMORY;
  }

  rm_status status = RM_OK;
  uint64_t insert_count =
      rm_utf8_scalar_count(insertion.bytes, insertion.length);

  /* First pass: a row with fields this code does not know can only be kept
   * whole or removed whole. Inserting anywhere but at its very start would
   * split it. */
  size_t scan_at = 0;
  bool scan_inserted = false;
  for (size_t i = 0; i < t->count; ++i) {
    const text_item* r = &t->records[t->order[i]];
    size_t length = text_row_vis_len(r);
    if (length > SIZE_MAX - scan_at) {
      status = RM_ERR_MALFORMED_PAGE;
      goto done;
    }

    size_t stop = scan_at + length;
    size_t lo = start <= scan_at ? 0 : start < stop ? start - scan_at : length;
    size_t hi = end <= scan_at ? 0 : end < stop ? end - scan_at : length;
    bool insert_here = insertion.length && !scan_inserted && length &&
                       start >= scan_at && start < stop;
    if (insert_here) scan_inserted = true;
    if (!r->deleted && t->has_value_suffix[r->source]) {
      if (insert_here && (src_total_vis[r->source] > 0 || lo > 0)) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }
      src_total_vis[r->source] += length;
      src_del_vis[r->source] += (hi - lo);
    }
    scan_at = stop;
  }

  /* Splitting an extended record would give its unknown fields new owners. */
  for (size_t s = 0; s < t->decoded_count; ++s) {
    if (t->has_value_suffix[s] && src_del_vis[s] > 0 &&
        src_del_vis[s] < src_total_vis[s]) {
      status = RM_ERR_UNVERIFIED;
      goto done;
    }
  }

  /* Second pass: emit the rows. `previous` is the last visible ID so far,
   * the left neighbour of an insertion that starts a run. */
  size_t at = 0;
  bool inserted = false;
  size_t replacement_byte = 0, replacement_scalar = 0;
  rm_crdt_id previous = RM_CRDT_NONE;

  for (size_t i = 0; i < t->count; ++i) {
    size_t n = t->order[i];
    const text_item* r = &t->records[n];
    rm_reader raw = t->raw[r->source];
    bool is_ext_row = t->has_value_suffix[r->source];
    size_t length = text_row_vis_len(r);
    uint64_t span = rmi_text_item_span(r);
    if (span == 0) span = 1;
    uint64_t last_id = rmi_span_last(r->id.counter, span);
    if (!r->deleted && span - 1 > UINT64_MAX - r->id.counter) {
      status = RM_ERR_MALFORMED_PAGE;
      goto done;
    }
    /* The inserted run must not overlap any ID a row already holds. */
    if (insert_count && fresh.author == r->id.author &&
        fresh.counter <= last_id &&
        r->id.counter <= fresh.counter + (insert_count - 1)) {
      status = RM_ERR_INVALID_ARGUMENT;
      goto done;
    }

    size_t stop = at + length;
    size_t lo = start <= at ? 0 : start < stop ? start - at : length;
    size_t hi = end <= at ? 0 : end < stop ? end - at : length;
    bool insert_here =
        insertion.length && !inserted && length && start >= at && start < stop;

    /* Untouched rows go out as they were. An insertion in front of an
     * extended row goes before it, leaving the row whole. Otherwise the row
     * is split: the kept head, the insertion, a tombstone for the removed
     * part, then the kept tail, each linked to its neighbours by ID. */
    if (!insert_here && hi == lo) {
      copy_row(out, r, raw, is_ext_row, emitted_raw);
    } else if (is_ext_row && !r->deleted && lo == 0 && hi == 0 && insert_here) {
      emit_row(out, fresh, previous, r->id, 0, insertion);
      inserted = true;
      copy_row(out, r, raw, is_ext_row, emitted_raw);
    } else {
      size_t b0 = byte_at(r->text, lo), b1 = byte_at(r->text, hi);
      rm_crdt_id left = lo ? advanced(r->id, lo - 1) : r->left;
      if (lo)
        emit_row(out, r->id, r->left, r->right, 0, (rm_str){r->text.bytes, b0});
      if (insert_here) {
        emit_row(out, fresh, lo ? left : previous, advanced(r->id, lo), 0,
                 insertion);
        inserted = true;
      }
      /* Remember the removed newlines: each keys a paragraph style. */
      if (hi > lo && !r->has_marker) {
        size_t old_scalar = lo;

        for (size_t b = b0; b < b1; old_scalar++) {
          uint32_t ch;
          b = rm_utf8_next(r->text.bytes, r->text.length, b, &ch);
          if (ch != '\n') continue;
          rm_crdt_id nl_id = advanced(r->id, old_scalar);
          if (!rm_crdt_equal(nl_id, RM_CRDT_NONE) &&
              !rm_idmap_set(&out->removed_newlines, nl_id, 1)) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }

          /* Pair removed and inserted paragraph breaks in reading order. */
          while (replacement_byte < insertion.length) {
            uint32_t next;
            replacement_byte = rm_utf8_next(insertion.bytes, insertion.length,
                                            replacement_byte, &next);
            size_t offset = replacement_scalar++;
            if (next == '\n') {
              if (!rm_idmap_set(&out->style_offsets, nl_id, offset)) {
                status = RM_ERR_OUT_OF_MEMORY;
                goto done;
              }
              break;
            }
          }
        }
      }

      if (hi > lo) {
        for (size_t k = lo; k < hi; ++k) {
          if (!rm_idmap_set(removed, advanced(r->id, k), (at + k) - start)) {
            status = RM_ERR_OUT_OF_MEMORY;
            goto done;
          }
        }
        if (hi - lo > UINT32_MAX) {
          status = RM_ERR_TOO_MANY_TO_NUMBER;
          goto done;
        }
        emit_row(out, advanced(r->id, lo), left, r->right, (uint32_t)(hi - lo),
                 (rm_str){0});
      }
      if (hi < length) {
        if (!hi) {
          copy_row(out, r, raw, is_ext_row, emitted_raw);
        } else {
          emit_row(out, advanced(r->id, hi), advanced(r->id, hi - 1), r->right,
                   0, (rm_str){r->text.bytes + b1, r->text.length - b1});
        }
      }
    }

    if (length) previous = advanced(r->id, (size_t)(span - 1u));
    at = stop;
  }
  if (end > at) {
    status = RM_ERR_INVALID_ARGUMENT;
    goto done;
  }
  /* An insertion at the end of the text follows the last visible ID. */
  if (insertion.length && !inserted)
    emit_row(out, fresh, previous, RM_CRDT_NONE, 0, insertion);
  if (out->count > (size_t)PTRDIFF_MAX - t->surplus) {
    status = RM_ERR_TOO_MANY_TO_NUMBER;
    goto done;
  }
  status = rmi_buf_status(&out->bytes);

done:
  free(emitted_raw);
  free(src_total_vis);
  free(src_del_vis);
  return status;
}

/* A style belongs to the paragraph after its newline. Move that key when the
 * edit pairs an old newline with a replacement.
 *
 * `lists` starts after the first items container. Everything up to the style
 * list and after it is copied; within it, only a remapped key changes, and
 * its stamp and value are kept. A key whose newline ID is not unique on the
 * page cannot be matched safely and refuses the edit if it would move. */
static rm_status remap_styles(rm_reader lists, rm_crdt_id fresh,
                              const rm_idmap* offsets,
                              const rm_idmap* removed_newlines,
                              const rm_idmap* newline_ambiguous_ids,
                              rm_buf* result) {
  while (rm_reader_remaining(&lists) > 0) {
    size_t start = lists.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&lists, &tag, &type) != RM_OK) {
      rm_buf_append(result, lists.data + start, lists.size - start);
      return rmi_buf_status(result);
    }
    if (tag == 2 && type == RM_TAG_LENGTH4) {
      lists.offset = start;
      break;
    }
    if (rmi_skip_tagged_value(&lists, type, nullptr) != RM_OK) {
      rm_buf_append(result, lists.data + start, lists.size - start);
      return rmi_buf_status(result);
    }
    rm_buf_append(result, lists.data + start, lists.offset - start);
    RM_TRY(rmi_buf_status(result));
  }
  if (rm_reader_remaining(&lists) == 0) return RM_OK;

  size_t styles_header = lists.offset;
  if (rm_expect_tag(&lists, 2, RM_TAG_LENGTH4) != RM_OK)
    return RM_ERR_MALFORMED_PAGE;
  rm_reader styles, rows;
  if (rm_read_subblock(&lists, &styles) != RM_OK) return RM_ERR_MALFORMED_PAGE;
  rm_reader content = styles;
  size_t rows_header = content.offset;
  if (!rm_match_tag(&content, 1, RM_TAG_LENGTH4)) {
    rm_buf_append(result, lists.data + styles_header,
                  lists.size - styles_header);
    return rmi_buf_status(result);
  }
  if (rm_read_subblock(&content, &rows) != RM_OK) return RM_ERR_MALFORMED_PAGE;
  size_t count;
  rm_reader input = rows;
  if (rm_read_count(&input, &count) != RM_OK) return RM_ERR_MALFORMED_PAGE;

  rm_subblock_mark styles_mark = rm_buf_begin_subblock(result, 2);
  rm_buf_append(result, styles.data + styles.offset,
                rows_header - styles.offset);
  rm_subblock_mark rows_mark = rm_buf_begin_subblock(result, 1);
  rm_buf_append(result, rows.data + rows.offset, input.offset - rows.offset);
  for (size_t i = 0; i < count && rm_reader_remaining(&input) > 0; i++) {
    size_t entry_start = input.offset;
    rm_crdt_id key;
    if (rm_read_bare_crdt(&input, &key) != RM_OK) {
      input.offset = entry_start;
      break;
    }

    size_t after_key = input.offset;
    if (rm_match_tag(&input, 1, RM_TAG_ID)) {
      rm_crdt_id stamp = RM_CRDT_NONE;
      if (rm_read_bare_crdt(&input, &stamp) != RM_OK) {
        input.offset = entry_start;
        break;
      }
    }
    rm_reader value = {0};
    if (!rm_match_tag(&input, 2, RM_TAG_LENGTH4) ||
        rm_read_subblock(&input, &value) != RM_OK) {
      input.offset = entry_start;
      break;
    }

    rm_paragraph_format fmt;
    bool has_fmt = false;
    rm_reader val_probe = value;
    if (rmi_parse_style_value(&val_probe, &fmt, &has_fmt) != RM_OK) {
      input.offset = entry_start;
      break;
    }
    if (!rm_crdt_equal(key, RM_CRDT_NONE) &&
        rm_idmap_contains(newline_ambiguous_ids, key) &&
        (rm_idmap_contains(offsets, key) ||
         rm_idmap_contains(removed_newlines, key))) {
      return RM_ERR_UNVERIFIED;
    }

    rm_crdt_id out_key = key;
    uint64_t offset;
    bool remapped = !rm_crdt_equal(key, RM_CRDT_NONE) &&
                    rm_idmap_get(offsets, key, &offset);
    if (remapped) {
      out_key = advanced(fresh, (size_t)offset);
      rm_write_bare_id(result, out_key);
      rm_buf_append(result, input.data + after_key, input.offset - after_key);
    } else {
      rm_buf_append(result, input.data + entry_start,
                    input.offset - entry_start);
    }
  }
  rm_buf_append(result, input.data + input.offset, rm_reader_remaining(&input));
  rm_buf_end_subblock(result, rows_mark);
  rm_buf_append(result, styles.data + rows.size, styles.size - rows.size);
  rm_buf_end_subblock(result, styles_mark);
  rm_buf_append(result, lists.data + lists.offset, rm_reader_remaining(&lists));
  return rmi_buf_status(result);
}

/* All list headers are five bytes (a one-byte tag followed by a u32).
 * Preserve siblings and tails verbatim as each containing length changes.
 *
 * Writes the new body of the RootText block into `result`. A root that had no
 * lists gets them; one that had some keeps every field around the rewritten
 * rows. */
static rm_status change_root(rm_reader root, size_t start, size_t end,
                             rm_str insertion, rm_crdt_id fresh,
                             rm_idmap* removed,
                             const rm_idmap* newline_ambiguous_ids,
                             rm_buf* result) {
  text_rows t = {0};
  rewritten rewritten_rows = {0};
  rm_idmap_init(&rewritten_rows.style_offsets);
  rm_idmap_init(&rewritten_rows.removed_newlines);
  rm_status status = load_rows(&t, root);
  if (status == RM_OK)
    status = rewrite_rows(&t, start, end, insertion, fresh, removed,
                          &rewritten_rows);
  if (status == RM_OK) {
    rm_buf_append(result, t.root.data + t.root.offset,
                  t.lists_start - t.root.offset);
    rm_subblock_mark lists_m = rm_buf_begin_subblock(result, 2);
    if (!t.has_lists) {
      rm_subblock_mark items_m = rm_buf_begin_subblock(result, 1);

      rm_subblock_mark rows_m = rm_buf_begin_subblock(result, 1);
      rm_write_varuint(result, rewritten_rows.count + t.surplus);
      rm_buf_append(result, rewritten_rows.bytes.bytes,
                    rewritten_rows.bytes.count);
      rm_buf_end_subblock(result, rows_m);
      rm_buf_end_subblock(result, items_m);
      rm_buf_end_subblock(result, lists_m);
      rm_buf_append(result, t.root.data + t.lists_start,
                    t.root.size - t.lists_start);
    } else {
      rm_buf_append(result, t.lists.data + t.lists.offset,
                    t.items_start - t.lists.offset);
      rm_subblock_mark items_m = rm_buf_begin_subblock(result, 1);
      if (t.has_items) {
        rm_buf_append(result, t.items.data + t.items.offset,
                      t.rows_start - t.items.offset);
      }

      /* The count covers the rows written here and the undecoded rows that
       * still follow them. */
      rm_subblock_mark rows_m = rm_buf_begin_subblock(result, 1);
      rm_write_varuint(result, rewritten_rows.count + t.surplus);
      rm_buf_append(result, rewritten_rows.bytes.bytes,
                    rewritten_rows.bytes.count);
      if (t.has_rows && t.rows_suffix_start < t.rows.size) {
        rm_buf_append(result, t.rows.data + t.rows_suffix_start,
                      t.rows.size - t.rows_suffix_start);
      }
      rm_buf_end_subblock(result, rows_m);
      if (t.has_items) {
        size_t after_rows = t.has_rows ? t.rows.size : t.rows_start;
        rm_buf_append(result, t.items.data + after_rows,
                      t.items.size - after_rows);
      }
      rm_buf_end_subblock(result, items_m);

      /* The style list only needs rewriting when a newline went. */
      rm_reader tail = t.lists;
      tail.offset = t.has_items ? t.items.size : t.items_start;
      if (rewritten_rows.style_offsets.count ||
          rewritten_rows.removed_newlines.count) {
        status = remap_styles(tail, fresh, &rewritten_rows.style_offsets,
                              &rewritten_rows.removed_newlines,
                              newline_ambiguous_ids, result);
      } else {
        rm_buf_append(result, tail.data + tail.offset,
                      rm_reader_remaining(&tail));
      }
      if (status == RM_OK) {
        rm_buf_end_subblock(result, lists_m);
        rm_buf_append(result, t.root.data + t.lists.size,
                      t.root.size - t.lists.size);
      }
    }

    if (status == RM_OK) status = rmi_buf_status(result);
  }
  rm_idmap_free(&rewritten_rows.removed_newlines);
  rm_idmap_free(&rewritten_rows.style_offsets);
  rm_buf_free(&rewritten_rows.bytes);
  free(t.has_value_suffix);
  free(t.raw);
  rm_arena_destroy(t.arena);
  free(t.records);
  free(t.order);
  return status;
}

/* Join paragraphs with LF to put their public text in the same scalar space
 * as the edit range. */
static rm_status extract_text(const rm_page* page, rm_buf* flat) {
  for (size_t i = 0; page->has_text && i < page->text.paragraph_count; ++i) {
    if (i) rm_buf_append_byte(flat, '\n');
    rm_str s = page->text.paragraphs[i].text;
    rm_buf_append(flat, s.bytes, s.length);
  }
  return rmi_buf_status(flat);
}

/* Adjust only the existing PageStats fields; counts include the implicit
 * final character and the empty final paragraph. Tag 3 is the character
 * count and tag 4 the line count; every other byte is copied. */
static rm_status update_stats(rm_reader body, rm_text_counts totals,
                              rm_buf* out) {
  if (totals.chars > UINT32_MAX || totals.lines > UINT32_MAX)
    return RM_ERR_TOO_MANY_TO_NUMBER;
  while (rm_reader_remaining(&body)) {
    size_t start = body.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&body, &tag, &type) != RM_OK ||
        rmi_skip_tagged_value(&body, type, nullptr) != RM_OK) {
      rm_buf_append(out, body.data + start, body.size - start);
      break;
    }
    if (type == RM_TAG_BYTE4 && (tag == 3 || tag == 4)) {
      rm_buf_append(out, body.data + start, body.offset - start - 4);
      rm_write_u32(out, (uint32_t)(tag == 3 ? totals.chars : totals.lines));
    } else
      rm_buf_append(out, body.data + start, body.offset - start);
  }
  return rmi_buf_status(out);
}

/* The newest preferred column (SceneInfo tag 9) and the declared screen
 * size, which together give the column width. */
typedef struct column_setting {
  rm_crdt_id stamp;
  uint64_t mode;
  uint32_t screen_width;
  uint32_t screen_height;
} column_setting;

/* A page without RootText still has a preferred column. Read it from
 * SceneInfo before creating its first text box. The fields are walked in the
 * firmware's fixed order, as in the layer editor. */
static void read_column_setting(rm_reader body, column_setting* column) {
  static constexpr uint32_t kOrder[8] = {1, 2, 3, 5, 6, 7, 8, 9};
  for (size_t i = 0; i < sizeof kOrder / sizeof *kOrder; ++i) {
    uint32_t index = kOrder[i];
    if (!rm_match_tag(&body, index, RM_TAG_LENGTH4)) continue;

    rm_reader field = {0};
    if (rm_read_subblock(&body, &field) != RM_OK) break;
    if (index == 5) {
      uint32_t width, height;
      if (rm_read_u32(&field, &width) == RM_OK &&
          rm_read_u32(&field, &height) == RM_OK && width && height) {
        column->screen_width = width;
        column->screen_height = height;
      }
    } else if (index == 9) {
      rm_reader value;
      uint64_t mode;
      rm_crdt_id stamp;
      if (rm_read_crdt(&field, 1, &stamp) == RM_OK &&
          sub(&field, 2, &value) == RM_OK &&
          rm_read_varuint(&value, &mode) == RM_OK &&
          !rm_stamp_newer(column->stamp, stamp)) {
        column->stamp = stamp;
        column->mode = mode;
      }
    }
  }
}

/* A column never set (zero stamp) is read as setting 1, the medium width. */
static float preferred_width(const column_setting* column) {
  uint64_t setting =
      rm_crdt_equal(column->stamp, RM_CRDT_NONE) ? 1u : column->mode;
  return rmi_resolved_column_width(setting, column->screen_width,
                                   column->screen_height);
}

/* Give the first text on an ink-only page the same origin and column settings
 * as text written on that sheet. */
static void empty_root(rm_buf* root, const column_setting* column) {
  /* Under Auto (setting 4) the stamped settings width is written as zero. */
  float width = preferred_width(column);
  float settings_width =
      (!rm_crdt_equal(column->stamp, RM_CRDT_NONE) && column->mode == 4u)
          ? 0.0f
          : width;
  rm_write_id(root, 1, RM_CRDT_NONE);
  rm_subblock_mark lists = rm_buf_begin_subblock(root, 2);
  rm_subblock_mark items1 = rm_buf_begin_subblock(root, 1);
  rm_subblock_mark rows1 = rm_buf_begin_subblock(root, 1);
  rm_write_varuint(root, 0);
  rm_buf_end_subblock(root, rows1);
  rm_buf_end_subblock(root, items1);

  rm_subblock_mark items2 = rm_buf_begin_subblock(root, 2);
  rm_subblock_mark rows2 = rm_buf_begin_subblock(root, 1);
  rm_write_varuint(root, 0);
  rm_buf_end_subblock(root, rows2);
  rm_buf_end_subblock(root, items2);
  rm_buf_end_subblock(root, lists);
  rm_subblock_mark box = rm_buf_begin_subblock(root, 3);
  rm_write_f64(root, -width / 2.0);
  rm_write_f64(root, 234);
  rm_buf_end_subblock(root, box);
  rm_write_tag(root, 4, RM_TAG_BYTE4);
  rm_write_f32(root, width);
  rmi_write_text_settings_width(root, RM_CRDT_NONE, settings_width);
}

/* Choose the last root that can supply the visible text. A later root with
 * broken list boundaries or no finite origin leaves the earlier text on the
 * page. */
static bool root_text_publishes(rm_reader body) {
  rm_crdt_id ignored;
  if (rm_read_crdt(&body, 1, &ignored) != RM_OK) return false;
  bool has_origin = false;
  while (body.offset < body.size) {
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(&body, &index, &type) != RM_OK) break;
    if (type == RM_TAG_BYTE4) {
      float raw;
      if (rm_read_f32(&body, &raw) != RM_OK) break;
      continue;
    }
    if (type != RM_TAG_LENGTH4) break;

    uint32_t length;
    size_t sub_end;
    if (rm_read_u32(&body, &length) != RM_OK ||
        !rm_fits_within(&body, length, body.size, &sub_end)) {
      if (index == 2) return false;
      break;
    }
    if (index == 2) {
      rm_reader inner = body;
      inner.size = sub_end;
      if (rm_match_tag(&inner, 1, RM_TAG_LENGTH4)) {
        rm_reader container;
        if (rm_read_subblock(&inner, &container) != RM_OK) return false;
        if (rm_match_tag(&container, 1, RM_TAG_LENGTH4)) {
          rm_reader list;
          if (rm_read_subblock(&container, &list) != RM_OK) return false;
        }
      }

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

      if (found_styles && rm_match_tag(&inner, 2, RM_TAG_LENGTH4)) {
        rm_reader sc;
        if (rm_read_subblock(&inner, &sc) != RM_OK) return false;
        if (rm_match_tag(&sc, 1, RM_TAG_LENGTH4)) {
          rm_reader sl;
          if (rm_read_subblock(&sc, &sl) != RM_OK) return false;
        }
      }
    } else if (index == 3 && length >= 16) {
      double x, y;
      if (rm_read_f64(&body, &x) == RM_OK && rm_read_f64(&body, &y) == RM_OK &&
          isfinite(x) && isfinite(y))
        has_origin = true;
    }
    body.offset = sub_end;
  }
  return has_origin;
}

/* Replace the effective root and update the page counters. Older text blocks
 * and unrelated scene items go out as they came in.
 *
 * `expected` is the whole text after the edit, used for the counters. A page
 * with no RootText that would publish gets a new one, appended after the
 * last block. */
static rm_status rewrite_file(rm_reader r, size_t start, size_t end,
                              rm_str insertion, rm_crdt_id fresh,
                              const rm_buf* expected, rm_idmap* removed,
                              const rm_idmap* newline_ambiguous_ids,
                              rm_buf* file) {
  if (r.size < RM_HEADER_LENGTH) return RM_ERR_UNEXPECTED_EOF;
  size_t effective_root_start = SIZE_MAX;
  rm_reader scan = {.data = r.data, .size = r.size, .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&scan) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&scan, &b));
    if (b.type == RM_BLOCK_ROOT_TEXT) {
      rm_reader body = scan;
      body.size = b.end;
      if (root_text_publishes(body)) effective_root_start = b.start;
    }
    scan.offset = b.end;
  }

  rm_text_counts totals = {1, 1};
  if (expected != nullptr) {
    rm_text_counts_add(&totals, (const char*)expected->bytes, expected->count);
  }
  bool found = false;
  column_setting column = {0};
  size_t limit = rmi_block_limit();
  rm_buf_append(file, r.data, RM_HEADER_LENGTH);
  r.offset = RM_HEADER_LENGTH;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&r, &b));

    rm_reader body = r;
    body.size = b.end;
    if (b.type == RM_BLOCK_SCENE_INFO) read_column_setting(body, &column);
    if ((b.type == RM_BLOCK_ROOT_TEXT && b.start == effective_root_start) ||
        b.type == RM_BLOCK_PAGE_INFO) {
      rm_buf changed = {0};
      rm_status status =
          b.type == RM_BLOCK_ROOT_TEXT
              ? change_root(body, start, end, insertion, fresh, removed,
                            newline_ambiguous_ids, &changed)
              : update_stats(body, totals, &changed);
      if (b.type == RM_BLOCK_ROOT_TEXT) found = true;
      if (status == RM_OK && changed.count > limit)
        status = RM_ERR_TOO_MANY_TO_NUMBER;
      if (status == RM_OK) {
        rm_write_u32(file, (uint32_t)changed.count);
        rm_buf_append(file, r.data + b.start + 4, 4);
        rm_buf_append(file, changed.bytes, changed.count);
      }

      rm_buf_free(&changed);
      if (status != RM_OK) return status;
    } else
      rm_buf_append(file, r.data + b.start, b.end - b.start);
    r.offset = b.end;
  }
  if (!found) {
    rm_buf root = {0}, changed = {0};
    empty_root(&root, &column);

    rm_status status = rmi_buf_status(&root);
    if (status == RM_OK) {
      status = change_root((rm_reader){.data = root.bytes, .size = root.count},
                           start, end, insertion, fresh, removed,
                           newline_ambiguous_ids, &changed);
    }
    if (status == RM_OK)
      rm_write_top_block(file, RM_BLOCK_ROOT_TEXT, 1, 1, changed.bytes,
                         changed.count);
    rm_buf_free(&root);
    rm_buf_free(&changed);
    if (status != RM_OK) return status;
  }
  rm_buf_append(file, r.data + r.offset, rm_reader_remaining(&r));
  return rmi_buf_status(file);
}

/* The two IDs under author zero that stand for the start and the end of the
 * text rather than a character; an anchor on one never needs remapping. */
static bool is_boundary_sentinel(rm_crdt_id id) {
  return id.author == 0 &&
         (id.counter == 0xFFFF'FFFF'FFFEu || id.counter == 0xFFFF'FFFF'FFFFu);
}

/* Nodes can precede the root text on disk. Once the removed IDs are known,
 * patch their registers in the candidate, copying untouched blocks verbatim.
 * A record is one anchor register and the block it sits in. */
typedef struct anchor_record {
  rm_anchor_field field;
  rm_block_header block;
} anchor_record;

/* A node can carry several anchor writes. Keep each location but remember the
 * winning stamp before changing any bytes. */
static rm_status collect_anchors(rm_reader r, rm_vec* records,
                                 rm_idmap* winners) {
  while (rm_reader_remaining(&r) >= 8) {
    anchor_record record;
    RM_TRY(rm_read_block_header(&r, &record.block));
    rm_reader body = r;
    body.size = r.offset = record.block.end;
    if (record.block.type != RM_BLOCK_TREE_NODE) continue;
    RM_TRY(rm_read_node_anchor(body, &record.field));
    if (!record.field.field_end) continue;
    uint64_t index;
    if (rm_idmap_get(winners, record.field.node, &index)) {
      const anchor_record* old = rm_vec_at(records, index);
      if (rm_stamp_newer(old->field.stamp, record.field.stamp)) continue;
    }
    if (!rm_idmap_set(winners, record.field.node, records->count) ||
        !rm_vec_push(records, &record))
      return RM_ERR_OUT_OF_MEMORY;
  }
  return RM_OK;
}

/* Rebuild just the selected anchor register and its enclosing lengths. The
 * remainder of the node may hold fields the editor does not understand.
 *
 * Only a node's winning register is moved, and only when it names a removed
 * character. It moves to the inserted character at the same offset, under
 * the stamp just past the inserted run. With no such character the edit is
 * refused, unless the node is out of the tree anyway. On a change `file` is
 * replaced by the rebuilt bytes and `out` is left empty. */
static rm_status rewrite_anchors(rm_buf* file,
                                 const rm_idmap* orig_edit_removed,
                                 const rm_idmap* ambiguous_ids,
                                 const rm_idmap* removed, rm_crdt_id fresh,
                                 size_t inserted, rm_buf* out,
                                 const rm_vec* records, const rm_idmap* winners,
                                 bool* out_changed) {
  *out_changed = false;
  rm_reader r = {.data = file->bytes, .size = file->count};
  size_t copied = 0;
  size_t limit = rmi_block_limit();
  for (size_t i = 0; i < records->count; ++i) {
    const anchor_record* record = rm_vec_at(records, i);
    rm_anchor_field anchor = record->field;
    rm_block_header b = record->block;
    uint64_t winning;
    if (!rm_idmap_get(winners, anchor.node, &winning) || i != winning) continue;
    uint64_t offset;
    if (!anchor.field_end || is_boundary_sentinel(anchor.target) ||
        !rm_idmap_get(removed, anchor.target, &offset))
      continue;
    if (rm_idmap_contains(ambiguous_ids, anchor.target))
      return RM_ERR_UNVERIFIED;
    if (offset >= inserted) {
      if (rm_idmap_contains(orig_edit_removed, anchor.node)) continue;
      return RM_ERR_UNVERIFIED;
    }

    /* The new anchor must win the LWW comparison against the old one. */
    rm_crdt_id stamp = advanced(fresh, inserted);
    if (!rm_stamp_newer(stamp, anchor.stamp)) return RM_ERR_INVALID_ARGUMENT;
    rm_buf_append(out, r.data + copied, b.start - copied);
    size_t body_len_at = out->count;
    rm_write_u32(out, 0);
    size_t header = b.start + 4;
    rm_buf_append(out, r.data + header, anchor.length_at - header);
    size_t sub_len_at = out->count;
    rm_write_u32(out, 0);
    size_t id_start = out->count;
    rm_write_id(out, 1, stamp);
    rm_write_id(out, 2, advanced(fresh, (size_t)offset));
    RM_TRY(rmi_buf_status(out));

    /* Both the block and the register change length by the difference
     * between the old and the new stamp-and-target bytes. */
    size_t new_id_len = out->count - id_start;
    size_t payload = anchor.length_at + 4;
    size_t old = anchor.target_end - payload;
    uint64_t body_size = b.end - b.body - old + new_id_len;
    uint64_t sub_size = anchor.field_end - payload - old + new_id_len;
    if (body_size > limit || sub_size > limit) return RM_ERR_TOO_MANY_TO_NUMBER;
    rm_write_u32_at(out, body_len_at, (uint32_t)body_size);
    rm_write_u32_at(out, sub_len_at, (uint32_t)sub_size);
    copied = anchor.target_end;
  }
  if (copied) {
    rm_buf_append(out, r.data + copied, r.size - copied);
    rm_status o_st = rmi_buf_status(out);
    if (o_st != RM_OK) return o_st;
    rm_buf_free(file);
    *file = *out;
    *out = (rm_buf){0};
    *out_changed = true;
  }
  return RM_OK;
}

/* Remap anchors only after the replacement has identified which original
 * characters disappeared. */
static rm_status remap_anchors(rm_buf* file, const rm_idmap* orig_edit_removed,
                               const rm_idmap* ambiguous_ids,
                               const rm_idmap* removed, rm_crdt_id fresh,
                               size_t inserted, rm_buf* out,
                               bool* out_changed) {
  rm_vec records;
  rm_vec_init(&records, sizeof(anchor_record));
  rm_idmap winners = {0};
  if (file->count < RM_HEADER_LENGTH) return RM_ERR_UNEXPECTED_EOF;
  rm_reader r = {
      .data = file->bytes, .size = file->count, .offset = RM_HEADER_LENGTH};
  rm_status status = collect_anchors(r, &records, &winners);
  if (status == RM_OK)
    status =
        rewrite_anchors(file, orig_edit_removed, ambiguous_ids, removed, fresh,
                        inserted, out, &records, &winners, out_changed);
  rm_vec_free(&records);
  rm_idmap_free(&winners);
  return status;
}

/* How a highlight's character-range field read: not there, read, or there
 * but unreadable. */
typedef enum range_field_state : uint8_t {
  RANGE_ABSENT = 0,
  RANGE_DECODED,
  RANGE_MALFORMED,
} range_field_state;

static bool is_ascii_alnum(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
         (cp >= 'a' && cp <= 'z');
}

/* Shared by every highlight on the page. `first_layout_diff` is the first
 * position where the old and new text lay out differently; `bad_prefix[k]`
 * counts positions before k whose character changed identity other than by
 * the edit's own one-for-one replacement; the two `next_*` arrays give the
 * next unchanged line break at or after each position. */
typedef struct glyph_verify_prep {
  size_t common_min;
  size_t first_layout_diff;
  bool trailing_changed;
  size_t* bad_prefix;
  size_t* next_safe_ltr_break;
  size_t* next_unchanged_nl;
} glyph_verify_prep;

static void glyph_verify_prep_free(glyph_verify_prep* p) {
  free(p->bad_prefix);
  free(p->next_safe_ltr_break);
  free(p->next_unchanged_nl);
  *p = (glyph_verify_prep){0};
}

/* Existing highlight rectangles depend on paragraph layout, including face
 * changes and inline images that contribute the same replacement character to
 * plain text. */
static bool layouts_equal_at(const rmi_text_flat_map* orig,
                             const rmi_text_flat_map* cand, size_t k) {
  if (orig->scalars[k] != cand->scalars[k] ||
      orig->is_inline_image[k] != cand->is_inline_image[k] ||
      orig->faces[k] != cand->faces[k] ||
      orig->para_idx[k] != cand->para_idx[k])
    return false;
  if (orig->is_inline_image[k] && !rm_crdt_equal(orig->ids[k], cand->ids[k]))
    return false;
  size_t p = orig->para_idx[k];
  const rmi_text_para_meta* op = &orig->paras[p];
  const rmi_text_para_meta* cp = &cand->paras[p];
  if (op->has_format != cp->has_format) return false;
  if (op->has_format && memcmp(&op->format, &cp->format, sizeof op->format))
    return false;
  return true;
}

/* Check the common text once. Prefix counts and the next unchanged break let
 * many highlights share the same layout comparison. */
static rm_status glyph_verify_prep_build(const rmi_text_flat_map* orig,
                                         const rmi_text_flat_map* cand,
                                         const rm_idmap* removed,
                                         rm_crdt_id fresh, size_t inserted,
                                         glyph_verify_prep* out) {
  *out = (glyph_verify_prep){0};
  size_t common_min = orig->count < cand->count ? orig->count : cand->count;
  out->common_min = common_min;
  out->bad_prefix = calloc(common_min + 1u, sizeof *out->bad_prefix);
  out->next_safe_ltr_break =
      calloc(common_min + 1u, sizeof *out->next_safe_ltr_break);
  out->next_unchanged_nl =
      calloc(common_min + 1u, sizeof *out->next_unchanged_nl);
  if (!out->bad_prefix || !out->next_safe_ltr_break ||
      !out->next_unchanged_nl) {
    glyph_verify_prep_free(out);
    return RM_ERR_OUT_OF_MEMORY;
  }

  size_t first_layout_diff = 0;
  while (first_layout_diff < common_min &&
         layouts_equal_at(orig, cand, first_layout_diff)) {
#ifdef RMLINES_TESTING
    rm_test_glyph_check_count += 1u;
#endif
    first_layout_diff += 1u;
  }
  out->first_layout_diff = first_layout_diff;
  out->trailing_changed =
      first_layout_diff < orig->count || first_layout_diff < cand->count;

  out->bad_prefix[0] = 0;
  for (size_t k = 0; k < common_min; ++k) {
#ifdef RMLINES_TESTING
    rm_test_glyph_check_count += 1u;
#endif
    bool ok = k < first_layout_diff;
    if (ok) {
      if (rm_crdt_equal(orig->ids[k], cand->ids[k])) {
        ok = true;
      } else {
        uint64_t off;
        ok = rm_idmap_get(removed, orig->ids[k], &off) && off < inserted &&
             rm_crdt_equal(cand->ids[k], advanced(fresh, (size_t)off));
      }
    }
    out->bad_prefix[k + 1u] = out->bad_prefix[k] + (ok ? 0u : 1u);
  }

  /* Walk backwards to record, for each position, the next newline and the
   * next space between two ASCII letters or digits before the first layout
   * difference. */
  size_t next_nl = SIZE_MAX;
  size_t next_ltr = SIZE_MAX;
  for (size_t i = common_min + 1u; i-- > 0;) {
    if (i < first_layout_diff) {
      uint32_t sc = orig->scalars[i];
      if (sc == '\n') {
        next_nl = i;
        next_ltr = i;
      } else if (sc == ' ' && i > 0 && is_ascii_alnum(orig->scalars[i - 1u]) &&
                 i + 1u < first_layout_diff &&
                 is_ascii_alnum(cand->scalars[i + 1u])) {
        next_ltr = i;
      }
    }
    out->next_unchanged_nl[i] = next_nl;
    out->next_safe_ltr_break[i] = next_ltr;
  }
  return RM_OK;
}

/* Rectangles are already in page coordinates. Rewrite character references
 * only where the old rectangles can still cover the same text.
 *
 * The block is one highlight. Tag 7 names its first character, tag 8 the
 * character at its end, and tag 9 whether that end character is included.
 * When either ID must change, `rebuilt_block` receives the whole new block
 * and `out_modified` is set. Older highlights use start and length. Read
 * through the rectangles before deciding that no character IDs follow. */
static rm_status verify_and_rebuild_glyph_block(
    const rmi_text_flat_map* orig, const rmi_text_flat_map* cand,
    const glyph_verify_prep* prep, rm_block_header b, rm_reader body,
    rm_buf* rebuilt_block, bool* out_modified) {
  *out_modified = false;
  rm_arena* scratch = nullptr;
  rm_str text = {.bytes = "", .length = 0};
  rm_item_preamble pre;
  RM_TRY(rm_parse_item_preamble(&body, &pre));
  if (pre.deleted != 0) return RM_OK;

  if (!rm_match_tag(&body, 6, RM_TAG_LENGTH4)) return RM_ERR_UNVERIFIED;
  rm_reader val;
  RM_TRY(rm_read_subblock(&body, &val));
  size_t val_head_in_body = val.offset - 4u; /* relative to data */

  uint8_t ignored_type;
  RM_TRY(rm_read_u8(&val, &ignored_type));

  if (rm_match_tag(&val, 2, RM_TAG_BYTE4)) {
    uint32_t ignored;
    RM_TRY(rm_read_u32(&val, &ignored));
  }
  if (rm_match_tag(&val, 3, RM_TAG_BYTE4)) {
    uint32_t ignored;
    RM_TRY(rm_read_u32(&val, &ignored));
  }

  uint32_t color_id;
  if (!rm_match_tag(&val, 4, RM_TAG_BYTE4) ||
      rm_read_u32(&val, &color_id) != RM_OK)
    return RM_ERR_UNVERIFIED;

  if (rm_match_tag(&val, 5, RM_TAG_LENGTH4)) {
    uint32_t text_length;
    if (rm_read_u32(&val, &text_length) != RM_OK) return RM_ERR_UNVERIFIED;
    size_t text_end;
    if (!rm_fits_within(&val, text_length, val.size, &text_end))
      text_end = val.size;
    scratch = rm_arena_create();
    if (!scratch) return RM_ERR_OUT_OF_MEMORY;
    rm_status ts = rm_read_string(&val, text_end, scratch, &text);
    if (ts != RM_OK) {
      rm_arena_destroy(scratch);
      return ts;
    }
    val.offset = text_end;
  }

  rm_reader rect_data;
  if (!rm_match_tag(&val, 6, RM_TAG_LENGTH4) ||
      rm_read_subblock(&val, &rect_data) != RM_OK) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }

  range_field_state tag7_st = RANGE_ABSENT;
  range_field_state tag8_st = RANGE_ABSENT;
  range_field_state tag9_st = RANGE_ABSENT;
  rm_crdt_id orig_first = RM_CRDT_NONE, orig_last = RM_CRDT_NONE;
  uint8_t include_flag = 0;
  size_t tag7_pos = 0, tag7_end = 0, tag8_pos = 0, tag8_end = 0;
  bool saw_post_range = false;
  bool tail_truncated_in_range = false;

  while (rm_reader_remaining(&val) > 0) {
    size_t fstart = val.offset;
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&val, &tag, &type) != RM_OK) {
      if (!saw_post_range) tail_truncated_in_range = true;
      break;
    }
    if (tag == 7) {
      if (tag7_st != RANGE_ABSENT || type != RM_TAG_ID ||
          rm_read_bare_crdt(&val, &orig_first) != RM_OK) {
        tag7_st = RANGE_MALFORMED;
        break;
      }

      tag7_st = RANGE_DECODED;
      tag7_pos = fstart;
      tag7_end = val.offset;
    } else if (tag == 8) {
      if (tag8_st != RANGE_ABSENT || type != RM_TAG_ID ||
          rm_read_bare_crdt(&val, &orig_last) != RM_OK) {
        tag8_st = RANGE_MALFORMED;
        break;
      }

      tag8_st = RANGE_DECODED;
      tag8_pos = fstart;
      tag8_end = val.offset;
    } else if (tag == 9) {
      if (tag9_st != RANGE_ABSENT || type != RM_TAG_BYTE1 ||
          rm_read_u8(&val, &include_flag) != RM_OK) {
        tag9_st = RANGE_MALFORMED;
        break;
      }
      tag9_st = RANGE_DECODED;
    } else {
      if (tag > 9) saw_post_range = true;
      if (rmi_skip_tagged_value(&val, type, nullptr) != RM_OK) {
        if (tag <= 9) tail_truncated_in_range = true;
        break;
      }
    }
  }

  if (tag7_st == RANGE_ABSENT && tag8_st == RANGE_ABSENT &&
      tag9_st == RANGE_ABSENT) {
    rm_arena_destroy(scratch);
    return RM_OK;
  }

#ifdef RMLINES_TESTING
  rm_test_glyph_check_count += 1u;
#endif

  /* A range field that is unreadable, or bytes that stop before the range
   * ends, leave the highlight's coverage unknown. */
  if (tail_truncated_in_range || tag7_st != RANGE_DECODED ||
      tag8_st == RANGE_MALFORMED || tag9_st == RANGE_MALFORMED) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }
  bool include_last = tag9_st == RANGE_DECODED && include_flag != 0;
  if (rm_crdt_equal(orig_first, RM_CRDT_NONE) ||
      rm_idmap_contains(&orig->ambiguous_ids, orig_first)) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }
  uint64_t p_first;
  if (!rm_idmap_get(&orig->pos_of, orig_first, &p_first) ||
      p_first >= orig->count) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }

  size_t sel_lo = (size_t)p_first;
  size_t sel_hi = sel_lo + 1u;
  if (tag8_st == RANGE_DECODED) {
    if (rm_crdt_equal(orig_last, RM_CRDT_NONE) ||
        rm_idmap_contains(&orig->ambiguous_ids, orig_last)) {
      rm_arena_destroy(scratch);
      return RM_ERR_UNVERIFIED;
    }
    uint64_t p_last;
    if (!rm_idmap_get(&orig->pos_of, orig_last, &p_last) ||
        p_last >= orig->count) {
      rm_arena_destroy(scratch);
      return RM_ERR_UNVERIFIED;
    }

    if (include_last ? (p_last < sel_lo) : (p_last <= sel_lo)) {
      rm_arena_destroy(scratch);
      return RM_ERR_UNVERIFIED;
    }
    sel_hi = (size_t)p_last + (include_last ? 1u : 0u);
  }
  /* Every highlighted character must keep its layout and either keep its ID
   * or be replaced one for one by the edit. */
  if (sel_hi > cand->count || sel_lo > prep->first_layout_diff ||
      prep->bad_prefix[sel_hi] - prep->bad_prefix[sel_lo] != 0) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }

  size_t orig_blo = orig->byte_at[sel_lo];
  size_t orig_bhi = orig->byte_at[sel_hi];
  size_t cand_blo = cand->byte_at[sel_lo];
  size_t cand_bhi = cand->byte_at[sel_hi];
  size_t orig_len = orig_bhi - orig_blo;
  size_t cand_len = cand_bhi - cand_blo;
  if (orig_len != cand_len) {
    rm_arena_destroy(scratch);
    return RM_ERR_UNVERIFIED;
  }

  /* A highlight that stores the text it covers must still cover that text. */
  if (text.length != 0) {
#ifdef RMLINES_TESTING
    rm_test_glyph_check_count += text.length;
#endif
    bool orig_matched =
        text.length == orig_len &&
        memcmp(text.bytes, orig->utf8 + orig_blo, orig_len) == 0;
    if (orig_matched &&
        memcmp(text.bytes, cand->utf8 + cand_blo, cand_len) != 0) {
      rm_arena_destroy(scratch);
      return RM_ERR_UNVERIFIED;
    }
  }
  rm_arena_destroy(scratch);
  scratch = nullptr;

  /* Text after the highlight lays out differently. Its rectangles still hold
   * if a break the change cannot move ends it: the highlight ends with a
   * newline, an unchanged break follows it before the first difference, or
   * the character just after it is now a newline where there was a newline
   * or a space between two ASCII letters or digits. Text with
   * bidirectional runs only trusts newlines. */
  if (prep->trailing_changed) {
    size_t end_para = orig->para_idx[sel_hi - 1u];
    bool simple_ltr =
        !orig->paras[end_para].has_bidi && !cand->paras[end_para].has_bidi;
    bool c1 = orig->scalars[sel_hi - 1u] == '\n';
    bool c2 = sel_hi <= prep->first_layout_diff &&
              (simple_ltr
                   ? prep->next_safe_ltr_break[sel_hi]
                   : prep->next_unchanged_nl[sel_hi]) < prep->first_layout_diff;
    bool c3 = simple_ltr && sel_hi < orig->count && sel_hi < cand->count &&
              cand->scalars[sel_hi] == '\n' &&
              (orig->scalars[sel_hi] == '\n' ||
               (orig->scalars[sel_hi] == ' ' && sel_hi > 0 &&
                is_ascii_alnum(orig->scalars[sel_hi - 1u]) &&
                sel_hi + 1u < orig->count &&
                is_ascii_alnum(orig->scalars[sel_hi + 1u])));
    if (!c1 && !c2 && !c3) return RM_ERR_UNVERIFIED;
  }

  rm_crdt_id new_first = cand->ids[sel_lo];
  if (rm_crdt_equal(new_first, RM_CRDT_NONE) ||
      rm_idmap_contains(&cand->ambiguous_ids, new_first))
    return RM_ERR_UNVERIFIED;

  rm_crdt_id new_last = orig_last;
  if (tag8_st == RANGE_DECODED) {
    if (!include_last && sel_hi >= cand->count) return RM_ERR_UNVERIFIED;
    new_last = include_last ? cand->ids[sel_hi - 1u] : cand->ids[sel_hi];
    if (rm_crdt_equal(new_last, RM_CRDT_NONE) ||
        rm_idmap_contains(&cand->ambiguous_ids, new_last))
      return RM_ERR_UNVERIFIED;
  }

  bool ch7 = !rm_crdt_equal(new_first, orig_first);
  bool ch8 = (tag8_st == RANGE_DECODED) && !rm_crdt_equal(new_last, orig_last);
  if (!ch7 && !ch8) return RM_OK;

  /* Rewrite the value with the changed IDs in place, whichever comes first,
   * and copy every other byte; then the block and the value get new
   * lengths. */
  rm_buf new_val = {0};
  size_t vcur = val_head_in_body + 4u;
  if (ch7 && ch8) {
    if (tag7_pos < tag8_pos) {
      rm_buf_append(&new_val, body.data + vcur, tag7_pos - vcur);
      rm_write_id(&new_val, 7, new_first);
      rm_buf_append(&new_val, body.data + tag7_end, tag8_pos - tag7_end);
      rm_write_id(&new_val, 8, new_last);
      vcur = tag8_end;
    } else {
      rm_buf_append(&new_val, body.data + vcur, tag8_pos - vcur);
      rm_write_id(&new_val, 8, new_last);
      rm_buf_append(&new_val, body.data + tag8_end, tag7_pos - tag8_end);
      rm_write_id(&new_val, 7, new_first);
      vcur = tag7_end;
    }
  } else if (ch7) {
    rm_buf_append(&new_val, body.data + vcur, tag7_pos - vcur);

    rm_write_id(&new_val, 7, new_first);
    vcur = tag7_end;
  } else {
    rm_buf_append(&new_val, body.data + vcur, tag8_pos - vcur);
    rm_write_id(&new_val, 8, new_last);
    vcur = tag8_end;
  }
  rm_buf_append(&new_val, body.data + vcur, val.size - vcur);

  size_t limit = rmi_block_limit();
  size_t new_body_len =
      (val_head_in_body - b.body) + 4u + new_val.count + (b.end - val.size);
  if (new_val.count > limit || new_body_len > limit) {
    rm_buf_free(&new_val);
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  rm_status nst = rmi_buf_status(&new_val);
  if (nst != RM_OK) {
    rm_buf_free(&new_val);
    return nst;
  }

  rm_write_u32(rebuilt_block, (uint32_t)new_body_len);
  rm_buf_append(rebuilt_block, body.data + b.start + 4u,
                val_head_in_body - (b.start + 4u));
  rm_write_u32(rebuilt_block, (uint32_t)new_val.count);
  rm_buf_append(rebuilt_block, new_val.bytes, new_val.count);
  rm_buf_append(rebuilt_block, body.data + val.size, b.end - val.size);
  rm_buf_free(&new_val);
  *out_modified = true;
  return rmi_buf_status(rebuilt_block);
}

/* Inspect marks on surviving nodes, including hidden layers. They will use
 * their saved rectangles again when the layer is shown. On a change `file` is
 * replaced by the rebuilt bytes and `out_changed` is set. */
static rm_status remap_glyph_ranges(rm_buf* file,
                                    const rm_idmap* orig_edit_removed,
                                    const rmi_text_flat_map* orig,
                                    const rmi_text_flat_map* cand,
                                    const rm_idmap* removed, rm_crdt_id fresh,
                                    size_t inserted, bool* out_changed) {
  *out_changed = false;
  if (file->count < RM_HEADER_LENGTH) return RM_ERR_UNEXPECTED_EOF;

  /* Most pages have no highlights; skip the shared preparation then. */
  rm_reader probe = {
      .data = file->bytes, .size = file->count, .offset = RM_HEADER_LENGTH};
  bool any_glyph = false;
  while (rm_reader_remaining(&probe) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&probe, &b));
    if (b.type == RM_BLOCK_SCENE_GLYPH_ITEM) {
      rm_reader body = probe;
      body.size = b.end;
      rm_item_preamble pre;
      if (rm_parse_item_preamble(&body, &pre) == RM_OK && pre.deleted == 0 &&
          !rm_idmap_contains(orig_edit_removed, pre.parent)) {
        any_glyph = true;
        break;
      }
    }
    probe.offset = b.end;
  }
  if (!any_glyph) return RM_OK;

  glyph_verify_prep prep = {0};
  RM_TRY(glyph_verify_prep_build(orig, cand, removed, fresh, inserted, &prep));

  rm_reader r = {
      .data = file->bytes, .size = file->count, .offset = RM_HEADER_LENGTH};
  rm_buf out = {0};
  size_t copied = 0;
  rm_status status = RM_OK;

  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    status = rm_read_block_header(&r, &b);
    if (status != RM_OK) break;
    if (b.type == RM_BLOCK_SCENE_GLYPH_ITEM) {
      rm_reader body = r;
      body.size = b.end;
      rm_item_preamble pre;
      if (rm_parse_item_preamble(&body, &pre) == RM_OK && pre.deleted == 0 &&
          !rm_idmap_contains(orig_edit_removed, pre.parent)) {
        rm_reader full_body = r;
        full_body.size = b.end;
        rm_buf rebuilt = {0};
        bool mod = false;
        status = verify_and_rebuild_glyph_block(orig, cand, &prep, b, full_body,
                                                &rebuilt, &mod);
        if (status != RM_OK) {
          rm_buf_free(&rebuilt);
          break;
        }

        if (mod) {
          rm_buf_append(&out, file->bytes + copied, b.start - copied);
          rm_buf_append(&out, rebuilt.bytes, rebuilt.count);
          rmi_buf_propagate(&out, &rebuilt);
          copied = b.end;
          *out_changed = true;
        }
        rm_buf_free(&rebuilt);
      }
    }
    r.offset = b.end;
  }

  glyph_verify_prep_free(&prep);
  if (status == RM_OK && *out_changed) {
    rm_buf_append(&out, file->bytes + copied, file->count - copied);
    status = rmi_buf_status(&out);
    if (status == RM_OK) {
      rm_buf_free(file);
      *file = out;
      out = (rm_buf){0};
    }
  }
  rm_buf_free(&out);
  return status;
}

#ifdef RMLINES_TESTING
/* A page with a broken highlight is refused at open. Exercise the range
 * check itself with the bytes left after deleting the last of the text. */
rm_status rm_test_verify_glyph_deletion(rm_buf* candidate,
                                        const rmi_text_flat_map* original) {
  bool changed;
  return remap_glyph_ranges(candidate, &(rm_idmap){0}, original,
                            &(rmi_text_flat_map){0}, &original->pos_of,
                            RM_CRDT_NONE, 0, &changed);
}
#endif

/* LF is the paragraph separator on disk. A carriage return occupies one
 * scalar inside its paragraph. */
static size_t count_newlines(const char* bytes, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len; ++i) {
    if (bytes[i] == '\n') ++n;
  }
  return n;
}

/* Verify paragraph styles shifted cleanly across the edited range: the
 * paragraph count changed by the newlines inserted less those removed, and
 * the paragraphs before and after the edited ones kept their formats. */
static bool verify_styles(const rm_page* before_page, const rm_page* after_page,
                          size_t start, size_t remove, size_t insert_count,
                          const rm_buf* flat_before, const rm_buf* flat_after) {
  if (!after_page->has_text) return before_page->has_text == false;
  if (!before_page->has_text) return true;
  size_t rem_nl = 0;
  if (remove) {
    rm_str fb = {(const char*)flat_before->bytes, flat_before->count};
    size_t b0 = byte_at(fb, start), b1 = byte_at(fb, start + remove);
    rem_nl = count_newlines(fb.bytes + b0, b1 - b0);
  }
  size_t ins_nl = 0;
  if (insert_count) {
    rm_str fa = {(const char*)flat_after->bytes, flat_after->count};
    size_t b0 = byte_at(fa, start), b1 = byte_at(fa, start + insert_count);
    ins_nl = count_newlines(fa.bytes + b0, b1 - b0);
  }
  size_t before_p = before_page->text.paragraph_count;
  size_t after_p = after_page->text.paragraph_count;
  if (after_p != before_p - rem_nl + ins_nl) return false;

  /* Prefix paragraphs strictly before the edited paragraph retain formats. */
  rm_str fb = {(const char*)flat_before->bytes, flat_before->count};
  size_t prefix_nl = count_newlines(fb.bytes, byte_at(fb, start));
  for (size_t i = 0; i < prefix_nl && i < before_p && i < after_p; ++i) {
    const rm_paragraph* bp = &before_page->text.paragraphs[i];
    const rm_paragraph* ap = &after_page->text.paragraphs[i];
    if (bp->has_format != ap->has_format) return false;
    if (bp->has_format && memcmp(&bp->format, &ap->format, sizeof bp->format))
      return false;
  }
  /* Tail paragraphs strictly after the edited range retain formats. */
  size_t suffix_start_before = prefix_nl + rem_nl + 1u;
  size_t suffix_start_after = prefix_nl + ins_nl + 1u;
  while (suffix_start_before < before_p && suffix_start_after < after_p) {
    const rm_paragraph* bp = &before_page->text.paragraphs[suffix_start_before];
    const rm_paragraph* ap = &after_page->text.paragraphs[suffix_start_after];
    if (bp->has_format != ap->has_format) return false;
    if (bp->has_format && memcmp(&bp->format, &ap->format, sizeof bp->format))
      return false;
    ++suffix_start_before;
    ++suffix_start_after;
  }
  return true;
}

/* Verify the resulting text against the requested scalar splice, then check
 * the styles and the records that refer to those characters.
 *
 * `utf8` is repaired before use. IDs are taken from `id`: one per inserted
 * scalar and, when the edit both removes and inserts, one more for the
 * anchor stamp. The candidate is the caller's and is marked verified: this
 * function parses its final bytes itself and refuses unreadable blocks. */
rm_status rmi_edit_replace_text(const uint8_t* data, size_t size, size_t start,
                                size_t remove, const char* utf8, rm_item_id id,
                                rmi_edit_candidate* out, rm_error* error) {
  *out = (rmi_edit_candidate){0};
  if (error) *error = (rm_error){0};
  rm_status status = RM_ERR_INVALID_ARGUMENT;
  rm_buf original = {0}, expected = {0}, candidate = {0}, actual = {0};
  rm_idmap removed = {0};
  rm_idmap orig_edit_removed = {0};
  rm_buf anchored = {0};
  char* valid = nullptr;
  rm_page* orig_page = nullptr;
  rm_page* cand_page = nullptr;
  rm_page* post_page = nullptr;
  rmi_text_flat_map orig_map = {0}, cand_map = {0};
  bool have_edit_state = false;

  if (!data || !utf8 || remove > SIZE_MAX - start) goto done;
  size_t bytes = strlen(utf8);
  size_t bound = rm_utf8_repair_bound(bytes);
  if (!bound) goto done;
  valid = malloc(bound);
  if (!valid) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  size_t length = rm_utf8_repair(utf8, bytes, valid);
  size_t count = rm_utf8_scalar_count(valid, length);

  /* One operation stamp serves every anchor register this edit remaps. */
  if (count == SIZE_MAX) goto done;
  size_t needed = count + (count && remove ? 1 : 0);

  /* The original page must read whole: the rewrite copies blocks it does
   * not understand, and an unread block could hide a reference. */
  status = rmi_parse_page_with_edit_state(data, size, &orig_page, &orig_map,
                                          &orig_edit_removed, error);
  have_edit_state = true;
  if (status != RM_OK) goto done;
  if (orig_page->skipped_blocks != 0) {
    status = RM_ERR_INCOMPLETE_PAGE;
    if (error) {
      *error = (rm_error){.status = RM_ERR_INCOMPLETE_PAGE,
                          .value = (long)orig_page->skipped_blocks};
    }
    goto done;
  }

  status = rmi_scene_check_ids(data, size, id, needed, error);
  if (status != RM_OK) goto done;

  status = extract_text(orig_page, &original);
  if (status != RM_OK) goto done;

  rm_str flat = {(const char*)original.bytes, original.count};
  if (start + remove > rm_utf8_scalar_count(flat.bytes, flat.length)) {
    status = RM_ERR_INVALID_ARGUMENT;
    goto done;
  }
  /* Nothing to remove or insert: return the page unchanged. */
  if (!count && !remove) {
    uint8_t* copy = malloc(size ? size : 1);
    if (!copy) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
    if (size) memcpy(copy, data, size);
    out->bytes = copy;
    out->size = size;
    out->verified = true;
    status = RM_OK;
    goto done;
  }

  /* The text the page must show afterwards. */
  size_t left = byte_at(flat, start), right = byte_at(flat, start + remove);
  rm_buf_append(&expected, flat.bytes, left);
  rm_buf_append(&expected, valid, length);
  if (flat.length > right)
    rm_buf_append(&expected, flat.bytes + right, flat.length - right);
  status = rmi_buf_status(&expected);
  if (status != RM_OK) goto done;

  status = rewrite_file((rm_reader){.data = data, .size = size}, start,
                        start + remove, (rm_str){valid, length}, rm_crdt_of(id),
                        &expected, &removed, &orig_map.newline_ambiguous_ids,
                        &candidate);
  if (status != RM_OK) goto done;

  /* Read the candidate back as the page reader would and compare its text
   * and styles with what was asked for. */
  status = rmi_parse_page_with_edit_state(
      candidate.bytes, candidate.count, &cand_page, &cand_map, nullptr, error);
  if (status != RM_OK) goto done;
  if (cand_page->skipped_blocks != 0) {
    status = RM_ERR_INCOMPLETE_PAGE;
    if (error)
      *error = (rm_error){.status = RM_ERR_INCOMPLETE_PAGE,
                          .value = (long)cand_page->skipped_blocks};
    goto done;
  }
  status = extract_text(cand_page, &actual);
  if (status != RM_OK) goto done;
  if (actual.count != expected.count ||
      (actual.count && memcmp(actual.bytes, expected.bytes, actual.count)) ||
      !verify_styles(orig_page, cand_page, start, remove, count, &original,
                     &actual)) {
    status = RM_ERR_UNVERIFIED;
    goto done;
  }

  /* Then move handwriting anchors and highlights off removed characters. */
  bool anchors_changed = false;
  if (removed.count) {
    status = remap_anchors(&candidate, &orig_edit_removed,
                           &orig_map.ambiguous_ids, &removed, rm_crdt_of(id),
                           count, &anchored, &anchors_changed);
    if (status != RM_OK) goto done;
  }

  bool glyphs_changed = false;
  if (count || remove) {
    status =
        remap_glyph_ranges(&candidate, &orig_edit_removed, &orig_map, &cand_map,
                           &removed, rm_crdt_of(id), count, &glyphs_changed);
    if (status != RM_OK) goto done;
  }

  /* Those passes rewrote blocks again; read the result once more. */
  if (anchors_changed || glyphs_changed) {
    status = rm_parse_page(candidate.bytes, candidate.count, &post_page, error);
    if (status != RM_OK) goto done;
    if (post_page->skipped_blocks != 0) {
      status = RM_ERR_INCOMPLETE_PAGE;
      if (error)
        *error = (rm_error){.status = RM_ERR_INCOMPLETE_PAGE,
                            .value = (long)post_page->skipped_blocks};
      goto done;
    }
    rm_buf_free(&actual);
    status = extract_text(post_page, &actual);
    if (status != RM_OK) goto done;
    if (actual.count != expected.count ||
        (actual.count && memcmp(actual.bytes, expected.bytes, actual.count)) ||
        !verify_styles(orig_page, post_page, start, remove, count, &original,
                       &actual)) {
      status = RM_ERR_UNVERIFIED;
      goto done;
    }
  }

  size_t out_size = 0;
  uint8_t* output = rm_buf_take(&candidate, &out_size);
  if (!output) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  out->bytes = output;
  out->size = out_size;
  out->verified = true;
  status = RM_OK;

done:
  if (have_edit_state) {
    rmi_text_flat_map_free(&orig_map);
    rmi_text_flat_map_free(&cand_map);
    rm_idmap_free(&orig_edit_removed);
  }
  rm_page_free(post_page);
  rm_page_free(cand_page);
  rm_page_free(orig_page);
  rm_idmap_free(&removed);
  rm_buf_free(&anchored);
  free(valid);
  rm_buf_free(&original);
  rm_buf_free(&expected);
  rm_buf_free(&candidate);
  rm_buf_free(&actual);
  return rm_edit_result(error, status);
}
