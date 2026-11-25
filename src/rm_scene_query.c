/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Unified CRDT ID collision scanner and read-only scene queries for the
 * editors. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_scene.h"
#include "rmlines/parser.h"

constexpr uint32_t TAG_ANCHOR = 7;
constexpr uint32_t TAG_TEXT_ITEMS = 2;
constexpr uint32_t TAG_TEXT_SETTINGS = 5;

/* Every collision is refused the same way: the caller asked for IDs the page
 * already uses, so it is the argument that is wrong. */
static rm_status report_collision(rm_error* error) {
  rmi_error_set(error, RM_ERR_INVALID_ARGUMENT, 0, 0,
                "identifier collides with existing page identifier");
  return RM_ERR_INVALID_ARGUMENT;
}

/* ------------------------------------------------------------------ */
/* Unified Schema-Aware CRDT ID Collision Scanner                      */
/* ------------------------------------------------------------------ */

/* A row can reserve an entire run of consecutive IDs. Check each neighbour as
 * it is read so a damaged tail cannot hide a collision in the preamble. */
static rm_status check_text_row_ids_incremental(rm_arena* arena, rm_reader row,
                                                rm_item_id first, size_t count,
                                                rm_error* error) {
  rm_crdt_id id = RM_CRDT_NONE;
  RM_TRY(rm_read_crdt(&row, 2, &id));
  if (rm_id_overlaps(id, 1, first, count)) return report_collision(error);

  rm_crdt_id left = RM_CRDT_NONE;
  RM_TRY(rm_read_crdt(&row, 3, &left));
  if (rm_id_overlaps(left, 1, first, count)) return report_collision(error);

  rm_crdt_id right = RM_CRDT_NONE;
  RM_TRY(rm_read_crdt(&row, 4, &right));
  if (rm_id_overlaps(right, 1, first, count)) return report_collision(error);

  /* A tombstone row covers as many consecutive IDs as it deleted. */
  uint32_t deleted = 0;
  RM_TRY(rm_expect_tag(&row, 5, RM_TAG_BYTE4));
  RM_TRY(rm_read_u32(&row, &deleted));
  if (deleted != 0) {
    if (rm_id_overlaps(id, (uint64_t)deleted, first, count))
      return report_collision(error);
    return RM_OK;
  }

  /* A live row of several characters holds one ID per character. */
  text_item item = {.id = id, .left = left, .right = right, .deleted = 0};
  rm_status status = rmi_decode_text_row_tag6(arena, &row, &item, nullptr);
  if (status == RM_ERR_OUT_OF_MEMORY) return status;
  if (status == RM_OK) {
    uint64_t span = rmi_text_item_span(&item);
    if (span > 1 && rm_id_overlaps(id, span, first, count))
      return report_collision(error);
  }
  return status;
}

/* Text outside the active count can still contain references. Follow the
 * bounds of recognizable rows and extensions to find the IDs an edit must
 * leave alone. */
static rm_status scan_inactive_text_rows(rm_arena* arena, rm_reader list,
                                         rm_item_id first, size_t count,
                                         rm_error* error) {
  while (list.offset < list.size) {
    size_t row_start = list.offset;
    rm_reader row = {0};
    rmi_row_envelope_status env =
        rmi_next_text_row_envelope(&list, list.size, &row_start, &row);
    if (env == RM_ROW_EOF || env == RM_ROW_SHORT_PREFIX) break;
    /* Something that is not a row: step over it as a tagged value and keep
     * looking, unless it ends the list. */
    if (env == RM_ROW_STOP) {
      list.offset = row_start;
      uint32_t tag;
      uint8_t type;
      if (rm_read_tag(&list, &tag, &type) != RM_OK) break;
      if (tag == 0 && type == RM_TAG_LENGTH4) break;
      if (rmi_skip_tagged_value(&list, type, nullptr) != RM_OK) break;
      continue;
    }
    rm_status status =
        check_text_row_ids_incremental(arena, row, first, count, error);
    if (status == RM_ERR_OUT_OF_MEMORY || status == RM_ERR_INVALID_ARGUMENT)
      return status;
  }
  return RM_OK;
}

/* The declared count delimits the displayed rows. Look beyond it for IDs
 * before handing a fresh range to an edit. */
static rm_status scan_text_row_list(rm_arena* arena, rm_reader* container,
                                    bool strict_envelope, rm_item_id first,
                                    size_t count, bool* out_stopped,
                                    rm_error* error) {
  if (out_stopped != nullptr) *out_stopped = false;
  rm_reader list = {0};
  if (rm_read_subblock(container, &list) != RM_OK) {
    if (strict_envelope) return RM_ERR_MALFORMED_PAGE;
    container->offset = container->size;
    if (out_stopped != nullptr) *out_stopped = true;
    return RM_OK;
  }
  size_t rows = 0;
  if (rm_read_count(&list, &rows) != RM_OK) return RM_OK;

  /* Only a collision or a lack of memory stops the scan with an error. */
  bool hit_stop = false;
  for (size_t i = 0; i < rows; ++i) {
    size_t row_start = list.offset;
    rm_reader row = {0};
    rmi_row_envelope_status env =
        rmi_next_text_row_envelope(&list, list.size, &row_start, &row);
    if (env == RM_ROW_EOF || env == RM_ROW_SHORT_PREFIX) return RM_OK;
    if (env == RM_ROW_STOP) {
      list.offset = row_start;
      hit_stop = true;
      break;
    }
    rm_status status =
        check_text_row_ids_incremental(arena, row, first, count, error);
    if (status == RM_ERR_OUT_OF_MEMORY || status == RM_ERR_INVALID_ARGUMENT)
      return status;
  }
  if (hit_stop || list.offset < list.size) {
    return scan_inactive_text_rows(arena, list, first, count, error);
  }
  return RM_OK;
}

/* A paragraph style names its preceding newline and carries a separate write
 * stamp. An edit may reuse neither. */
static rm_status scan_style_list(rm_reader* sib, bool strict_envelope,
                                 rm_item_id first, size_t count,
                                 rm_error* error) {
  if (!rm_match_tag(sib, 1, RM_TAG_LENGTH4)) return RM_OK;
  rm_reader style_list = {0};
  if (rm_read_subblock(sib, &style_list) != RM_OK) {
    return strict_envelope ? RM_ERR_MALFORMED_PAGE : RM_OK;
  }
  size_t style_count = 0;
  if (rm_read_count(&style_list, &style_count) != RM_OK) return RM_OK;

  for (size_t i = 0; i < style_count; ++i) {
    if (rm_reader_remaining(&style_list) == 0) break;
    rm_crdt_id key, stamp = RM_CRDT_NONE;
    if (rm_read_bare_crdt(&style_list, &key) != RM_OK) break;
    if (rm_id_overlaps(key, 1, first, count)) return report_collision(error);
    if (rm_match_tag(&style_list, 1, RM_TAG_ID)) {
      if (rm_read_bare_crdt(&style_list, &stamp) != RM_OK) break;
      if (rm_id_overlaps(stamp, 1, first, count))
        return report_collision(error);
    }
    if (!rm_match_tag(&style_list, 2, RM_TAG_LENGTH4)) break;
    rm_reader val = {0};
    if (rm_read_subblock(&style_list, &val) != RM_OK) break;
    rm_paragraph_format ignored_fmt;
    bool ignored_has = false;
    if (rmi_parse_style_value(&val, &ignored_fmt, &ignored_has) != RM_OK) break;
  }
  return RM_OK;
}

/* A later items or styles container can still mention an ID. Walk bounded
 * siblings even when only the first list is used to lay out the paragraph. */
static rm_status scan_text_items_container(rm_arena* arena, rm_reader r,
                                           bool strict_envelope,
                                           rm_item_id first, size_t count,
                                           rm_error* error) {
  bool at_items_start = true;
  bool first_styles_child = true;
  while (rm_reader_remaining(&r) > 0) {
    uint32_t sib_tag;
    uint8_t sib_type;
    if (rm_read_tag(&r, &sib_tag, &sib_type) != RM_OK) break;
    rm_reader sib = {0};
    if (rmi_skip_tagged_value(&r, sib_type, &sib) != RM_OK) {
      if (strict_envelope &&
          ((at_items_start && sib_tag == 1 && sib_type == RM_TAG_LENGTH4) ||
           (first_styles_child && sib_tag == 2 && sib_type == RM_TAG_LENGTH4)))
        return RM_ERR_MALFORMED_PAGE;
      break;
    }
    if (sib_tag == 1 && sib_type == RM_TAG_LENGTH4) {
      bool at_rows_start = true;

      while (rm_reader_remaining(&sib) > 0) {
        uint32_t c_tag;
        uint8_t c_type;
        if (rm_read_tag(&sib, &c_tag, &c_type) != RM_OK) break;
        if (c_tag == 1 && c_type == RM_TAG_LENGTH4) {
          bool stopped = false;
          rm_status st = scan_text_row_list(
              arena, &sib, strict_envelope && at_items_start && at_rows_start,
              first, count, &stopped, error);
          if (st != RM_OK) return st;
          if (stopped) break;
        } else if (rmi_skip_tagged_value(&sib, c_type, nullptr) != RM_OK) {
          break;
        }

        at_rows_start = false;
      }
    } else if (sib_tag == 2 && sib_type == RM_TAG_LENGTH4) {
      RM_TRY(scan_style_list(&sib, strict_envelope && first_styles_child, first,
                             count, error));
      first_styles_child = false;
    }
    at_items_start = false;
  }
  return RM_OK;
}

/* A RootText block can mention IDs in its lists and in the stamped column
 * setting as well as in the text itself. */
static rm_status check_root_text_ids(rm_reader root, rm_item_id first,
                                     size_t count, rm_error* error) {
  rm_crdt_id root_id;
  RM_TRY(rm_read_crdt(&root, 1, &root_id));
  if (rm_id_overlaps(root_id, 1, first, count)) return report_collision(error);

  rmi_scene scratch = {.arena = rm_arena_create()};
  if (scratch.arena == nullptr) return RM_ERR_OUT_OF_MEMORY;
  rm_status status = RM_OK;
  bool parser_stopped = false;

  while (rm_reader_remaining(&root) > 0) {
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&root, &tag, &type) != RM_OK) break;
    if (type != RM_TAG_BYTE4 && type != RM_TAG_LENGTH4) parser_stopped = true;
    /* An items list the page reader would still reach must read; past the
     * point where that reader stops, damage only ends the scan. */
    rm_reader sub = {0};
    rm_status skip_st = rmi_skip_tagged_value(&root, type, &sub);
    if (skip_st != RM_OK) {
      if (!parser_stopped && tag == TAG_TEXT_ITEMS && type == RM_TAG_LENGTH4)
        status = skip_st;
      break;
    }
    if (type != RM_TAG_LENGTH4) continue;

    if (tag == TAG_TEXT_ITEMS) {
      status = scan_text_items_container(scratch.arena, sub, !parser_stopped,
                                         first, count, error);
      if (status != RM_OK) break;
    } else if (tag == TAG_TEXT_SETTINGS) {
      rm_reader settings = sub;
      uint32_t stag;
      uint8_t stype;
      rm_reader sfield;
      while (rmi_next_text_settings_field(&settings, &stag, &stype, &sfield)) {
        if (stag == 1 && stype == RM_TAG_LENGTH4 &&
            rm_match_tag(&sfield, 1, RM_TAG_ID)) {
          rm_crdt_id stamp;
          if (rm_read_bare_crdt(&sfield, &stamp) == RM_OK &&
              rm_id_overlaps(stamp, 1, first, count)) {
            status = report_collision(error);
            break;
          }
        }
      }
      if (status != RM_OK) break;
    }
  }

  rm_arena_destroy(scratch.arena);
  return status;
}

/* The attachment key is an opaque string. Only its write stamp and the
 * image's lineage fields name edits. */
static rm_status check_image_item_ids(rm_reader body, rm_item_id first,
                                      size_t count, rm_error* error) {
  while (rm_reader_remaining(&body) > 0) {
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&body, &tag, &type) != RM_OK) break;
    if (type == RM_TAG_ID) {
      rm_crdt_id id;
      if (rm_read_bare_crdt(&body, &id) != RM_OK) break;
      if (rm_id_overlaps(id, 1, first, count)) return report_collision(error);
      continue;
    }

    rm_reader field = {0};
    if (rmi_skip_tagged_value(&body, type, &field) != RM_OK) break;
    if (tag == 1 && type == RM_TAG_LENGTH4) {
      bool has_stamp = false;
      rm_crdt_id stamp = RM_CRDT_NONE;
      const uint8_t* ignored_key = nullptr;
      size_t ignored_len = 0;
      if (rmi_parse_image_key(field, &has_stamp, &stamp, &ignored_key,
                              &ignored_len) == RM_OK &&
          has_stamp && rm_id_overlaps(stamp, 1, first, count)) {
        return report_collision(error);
      }
    }
  }
  return RM_OK;
}

/* Read IDs only in fields whose shape says they hold one. Bits in a
 * coordinate or attachment key can look just like an ID. */
static rm_status check_tagged_id_fields(rm_reader body, rm_item_id first,
                                        size_t count, uint32_t lww_tag_mask,
                                        uint32_t pair_tag, rm_error* error) {
  while (rm_reader_remaining(&body) > 0) {
    uint32_t tag;
    uint8_t type;
    if (rm_read_tag(&body, &tag, &type) != RM_OK) break;
    if (type == RM_TAG_ID) {
      rm_crdt_id id;
      if (rm_read_bare_crdt(&body, &id) != RM_OK) break;
      if (rm_id_overlaps(id, 1, first, count)) return report_collision(error);
      continue;
    }

    rm_reader field = {0};
    if (rmi_skip_tagged_value(&body, type, &field) != RM_OK) break;
    if (type == RM_TAG_LENGTH4 && tag < 32u &&
        ((lww_tag_mask >> tag) & 1u) != 0) {
      if (rm_match_tag(&field, 1, RM_TAG_ID)) {
        rm_crdt_id stamp;
        if (rm_read_bare_crdt(&field, &stamp) == RM_OK &&
            rm_id_overlaps(stamp, 1, first, count))
          return report_collision(error);
      }
      if (pair_tag != 0 && tag == pair_tag &&
          rm_match_tag(&field, 2, RM_TAG_ID)) {
        rm_crdt_id target;
        if (rm_read_bare_crdt(&field, &target) == RM_OK &&
            rm_id_overlaps(target, 1, first, count))
          return report_collision(error);
      }
    }
  }
  return RM_OK;
}

/* A fresh range must be free everywhere the page still names it, including
 * tombstones, neighbour links and earlier writes. */
static rm_status check_ids_impl(const uint8_t* data, size_t size,
                                rm_item_id first, size_t count,
                                bool ignore_layer_refs, rm_error* error) {
  if (!count) return RM_OK;
  if (!data || !first.author || !first.counter ||
      count - 1 > UINT64_MAX - first.counter) {
    rmi_error_set(error, RM_ERR_INVALID_ARGUMENT, 0, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (size < RM_HEADER_LENGTH) {
    rmi_error_set(error, RM_ERR_UNEXPECTED_EOF, 0, 0, nullptr);
    return RM_ERR_UNEXPECTED_EOF;
  }

  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header h;
    rm_status status = rm_read_block_header(&r, &h);
    if (status != RM_OK) {
      rmi_error_set(error, status, 0, 0, nullptr);
      return status;
    }

    rm_reader body = r;
    body.size = h.end;
    r.offset = h.end;

    /* A sequence item names its own ID (or a deleted run of them), its
     * parent and both neighbours. Group items also name the node they hold,
     * and live strokes, highlights and pictures carry IDs in their values. */
    if (rmi_block_is_sequence(h.type)) {
      rm_item_preamble item;
      RM_TRY(rm_parse_item_preamble(&body, &item));
      if (rm_id_overlaps(item.id, item.deleted ? item.deleted : 1, first,
                         count) ||
          (!ignore_layer_refs &&
           rm_id_overlaps(item.parent, 1, first, count)) ||
          rm_id_overlaps(item.left, 1, first, count) ||
          rm_id_overlaps(item.right, 1, first, count))
        return report_collision(error);

      if (h.type == RM_BLOCK_UNDO_TOMBSTONE) continue;

      if (rm_match_tag(&body, 6, RM_TAG_LENGTH4)) {
        rm_reader val;
        RM_TRY(rm_read_subblock(&body, &val));
        uint8_t kind;
        if (h.type == RM_BLOCK_SCENE_GROUP_ITEM) {
          if (rm_read_u8(&val, &kind) == RM_OK && kind == RM_VALUE_GROUP_ITEM) {
            rm_crdt_id child;
            rm_status child_status = rm_read_crdt(&val, 2, &child);

            if (child_status == RM_OK) {
              if (!(ignore_layer_refs &&
                    rm_crdt_equal(item.parent, RM_CRDT_ROOT)) &&
                  rm_id_overlaps(child, 1, first, count))
                return report_collision(error);
            } else if (item.deleted == 0) {
              return child_status;
            }
          } else if (item.deleted == 0) {
            return RM_ERR_MALFORMED_PAGE;
          }
        } else if (item.deleted == 0 && rm_read_u8(&val, &kind) == RM_OK) {
          if (h.type == RM_BLOCK_SCENE_LINE_ITEM ||
              h.type == RM_BLOCK_SCENE_GLYPH_ITEM) {
            RM_TRY(check_tagged_id_fields(val, first, count, 0u, 0, error));
          } else if (h.type == RM_BLOCK_IMAGE_ITEM) {
            RM_TRY(check_image_item_ids(val, first, count, error));
          }
        }
      }
    } else if (h.type == RM_BLOCK_MIGRATION_INFO) {
      rm_crdt_id id;

      if (rm_read_crdt(&body, 1, &id) == RM_OK &&
          rm_id_overlaps(id, 1, first, count))
        return report_collision(error);
    } else if (h.type == RM_BLOCK_SCENE_TREE) {
      /* A tree record: its own ID, the node it is about, and the parent. */
      rm_crdt_id id, target;
      RM_TRY(rm_read_crdt(&body, 1, &id));
      RM_TRY(rm_read_crdt(&body, 2, &target));
      if (rm_id_overlaps(id, 1, first, count) ||
          rm_id_overlaps(target, 1, first, count))
        return report_collision(error);

      uint8_t flag;
      if (rm_match_tag(&body, 3, RM_TAG_BYTE1) &&
          rm_read_u8(&body, &flag) == RM_OK &&
          rm_match_tag(&body, 4, RM_TAG_LENGTH4)) {
        rm_reader place;
        if (rm_read_subblock(&body, &place) == RM_OK) {
          rm_crdt_id parent;
          if (rm_read_crdt(&place, 1, &parent) == RM_OK &&
              rm_id_overlaps(parent, 1, first, count))
            return report_collision(error);
        }
      }
    } else if (h.type == RM_BLOCK_TREE_NODE) {
      rm_crdt_id id;
      RM_TRY(rm_read_crdt(&body, 1, &id));

      if (!ignore_layer_refs && rm_id_overlaps(id, 1, first, count))
        return report_collision(error);
      constexpr uint32_t kTreeNodeLwwTags = (1u << 2) | (1u << 3) | (1u << 7) |
                                            (1u << 8) | (1u << 9) | (1u << 10);
      RM_TRY(check_tagged_id_fields(body, first, count, kTreeNodeLwwTags,
                                    TAG_ANCHOR, error));
    } else if (h.type == RM_BLOCK_SCENE_INFO) {
      constexpr uint32_t kSceneInfoLwwTags =
          (1u << 1) | (1u << 2) | (1u << 3) | (1u << 6) | (1u << 8) | (1u << 9);
      RM_TRY(check_tagged_id_fields(body, first, count, kSceneInfoLwwTags,
                                    ignore_layer_refs ? 0u : 1u, error));
    } else if (h.type == RM_BLOCK_ATTACHMENT_MAP) {
      if (rm_match_tag(&body, 1, RM_TAG_LENGTH4)) {
        rm_reader list;
        size_t entries = 0;
        if (rm_read_subblock(&body, &list) == RM_OK &&
            rm_read_count(&list, &entries) == RM_OK) {
          for (size_t i = 0; i < entries && rm_reader_remaining(&list) > 0;
               ++i) {
            if (!rm_match_tag(&list, 0, RM_TAG_LENGTH4)) break;

            rm_reader entry;
            if (rm_read_subblock(&list, &entry) != RM_OK) break;
            const uint8_t* key;
            if (rm_read_bytes(&entry, 16, &key) == RM_OK) {
              constexpr uint32_t kAttachmentLwwTags = (1u << 1) | (1u << 2);
              RM_TRY(check_tagged_id_fields(entry, first, count,
                                            kAttachmentLwwTags, 0, error));
            }
          }
        }
      }
    } else if (h.type == RM_BLOCK_ROOT_TEXT) {
      RM_TRY(check_root_text_ids(body, first, count, error));
    }
  }
  return RM_OK;
}

/* Refuse the range [first, first + count) with RM_ERR_INVALID_ARGUMENT if the
 * page uses any ID in it. */
rm_status rmi_scene_check_ids(const uint8_t* data, size_t size,
                              rm_item_id first, size_t count, rm_error* error) {
  return check_ids_impl(data, size, first, count, false, error);
}

/* A layer already named by its children may need its first explicit
 * placement. Allow those layer references while checking that the ID has no
 * competing use. */
rm_status rmi_scene_check_layer_materialization_id(const uint8_t* data,
                                                   size_t size,
                                                   rm_item_id layer_node) {
  return check_ids_impl(data, size, layer_node, 1, true, nullptr);
}

/* ------------------------------------------------------------------ */
/* Scene queries for the editors                                       */
/* ------------------------------------------------------------------ */

/* Return the removal tables after dropping the decoded page. A hidden layer
 * still exists for an edit. */
rm_status rmi_scene_live_removed_nodes(const uint8_t* data, size_t size,
                                       rmi_tree_state* out_tree,
                                       rm_error* out_error) {
  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks(data, size, &ctx, out_error));
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  if (!rmi_tree_state_build(ctx.tree_events.data, ctx.tree_events.count,
                            out_tree)) {
    rmi_tree_state_free(out_tree);
    goto done;
  }
  if ((out_tree->removed.count || out_tree->has_root_placements) &&
      !rmi_removed_subtrees(ctx.items.data, ctx.items.count, out_tree)) {
    rmi_tree_state_free(out_tree);
    goto done;
  }
  out_tree->events = nullptr;
  status = RM_OK;
done:
  rmi_scene_destroy(&ctx);
  if (status != RM_OK) rmi_error_set(out_error, status, 0, 0, nullptr);
  return status;
}

/* A node named only by a surviving group link can still be a stroke's parent
 * on a page without full placement records. */
static rm_status target_node_live(const rmi_scene* ctx,
                                  const rmi_tree_state* tree, rm_crdt_id node,
                                  bool has_metadata, bool* out_live) {
  *out_live = false;
  if (!tree->has_root_placements) {
    bool exists = has_metadata || rm_idmap_contains(&tree->placed_by, node);
    const rmi_scene_item* items = ctx->items.data;
    for (size_t i = 0; !exists && i < ctx->items.count; i++) {
      const rmi_scene_item* item = &items[i];
      exists = item->kind == ITEM_GROUP && !item->deleted &&
               rm_crdt_equal(item->group, node) &&
               rmi_tree_links(tree, item->parent, node);
    }
    *out_live = exists;
    return RM_OK;
  }

  rm_idmap reachable;
  if (!rmi_tree_reachable_from_root(ctx->items.data, ctx->items.count, tree,
                                    &reachable))
    return RM_ERR_OUT_OF_MEMORY;
  *out_live = rm_idmap_contains(&reachable, node);
  rm_idmap_free(&reachable);
  return RM_OK;
}

/* Neighbours constrain a position inside one parent's sequence. An ID found
 * elsewhere on the page cannot establish that position. */
rm_status rmi_scene_check_location(const uint8_t* data, size_t size,
                                   rm_item_id parent, rm_item_id left,
                                   rm_item_id right, bool has_metadata,
                                   rm_error* error) {
  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks(data, size, &ctx, error));
  const rmi_scene_item* items = ctx.items.data;
  rm_vec indices;
  rm_vec_init(&indices, sizeof(uint32_t));
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  rmi_tree_state tree = {0};
  if (!rmi_tree_state_build(ctx.tree_events.data, ctx.tree_events.count,
                            &tree) ||
      ((tree.removed.count || tree.has_root_placements) &&
       !rmi_removed_subtrees(ctx.items.data, ctx.items.count, &tree)))
    goto done;
  /* A parent taken out of the tree is treated as missing. */
  if (rm_idmap_contains(&tree.removed, rm_crdt_of(parent))) {
    status = RM_ERR_ITEMS_NOT_FOUND;
    goto done;
  }

  bool head = rm_crdt_equal(rm_crdt_of(left), RM_CRDT_NONE);
  bool tail = rm_crdt_equal(rm_crdt_of(right), RM_CRDT_NONE);
  rm_crdt_id node = rm_crdt_of(parent);
  bool exists = false;
  status = target_node_live(&ctx, &tree, node, has_metadata, &exists);
  if (status != RM_OK) goto done;
  status = RM_ERR_OUT_OF_MEMORY;
  /* With no neighbours named, a live parent is all that is needed. */
  if (!exists || (head && tail)) {
    status = exists ? RM_OK : RM_ERR_ITEMS_NOT_FOUND;
    goto done;
  }
  for (size_t i = 0; i < ctx.items.count; i++) {
    if (!rm_crdt_equal(items[i].parent, rm_crdt_of(parent))) continue;
    uint32_t index = (uint32_t)i;
    if (index != i || !rm_vec_push(&indices, &index)) goto done;
  }

  size_t n;
  uint32_t* ordered = rmi_chain_order(indices.data, indices.count, items, &n);
  if (!ordered) goto done;
  /* Find the neighbours among the parent's visible children, in drawing
   * order; the left one must come before the right one. */
  size_t l = n, r = n;
  for (size_t i = 0; i < n; i++) {
    const rmi_scene_item* item = &items[ordered[i]];
    if (item->kind == ITEM_TOMBSTONE || item->deleted ||
        (item->kind == ITEM_GROUP &&
         (rm_idmap_contains(&tree.removed, item->group) ||
          !rmi_tree_links(&tree, item->parent, item->group))))
      continue;
    if (rm_crdt_equal(items[ordered[i]].id, rm_crdt_of(left))) l = i;
    if (rm_crdt_equal(items[ordered[i]].id, rm_crdt_of(right))) r = i;
  }
  if ((!head && l == n) || (!tail && r == n))
    status = RM_ERR_ITEMS_NOT_FOUND;
  else
    status = head || tail || l < r ? RM_OK : RM_ERR_INVALID_ARGUMENT;
  free(ordered);

done:
  rmi_tree_state_free(&tree);
  rm_vec_free(&indices);
  rmi_scene_destroy(&ctx);
  rmi_error_set(error, status, 0, 0, nullptr);
  return status;
}

/* Resolve the block to change and its successor in the original ordering
 * together. Non-stroke neighbours also determine where a replacement will
 * draw. */
rm_status rmi_scene_resolve_stroke_targets(const uint8_t* data, size_t size,
                                           const rm_idmap* selected,
                                           bool duplicate_first,
                                           size_t* out_first_block_starts,
                                           rm_crdt_id* out_successors,
                                           rm_error* error) {
  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks(data, size, &ctx, error));
  rmi_tree_state tree = {0};
  rm_grouping children = {0};
  rm_idmap parents = {0};
  bool* matched = nullptr;
  if (selected->count > 0) {
    matched = calloc(selected->count, sizeof(*matched));
    if (!matched) {
      rmi_scene_destroy(&ctx);
      rmi_error_set(error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
      return RM_ERR_OUT_OF_MEMORY;
    }
  }
  const rmi_scene_item* items = ctx.items.data;
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  if (!rmi_tree_state_build(ctx.tree_events.data, ctx.tree_events.count,
                            &tree) ||
      ((tree.removed.count || tree.has_root_placements) &&
       !rmi_removed_subtrees(ctx.items.data, ctx.items.count, &tree)) ||
      !rmi_scene_group_by_parent(ctx.items.data, ctx.items.count, &children))
    goto done;

  bool has_duplicate_live = false;
  size_t found = 0;
  for (size_t i = 0; i < ctx.items.count; ++i) {
    const rmi_scene_item* item = &items[i];
    uint64_t index = 0;
    if (item->kind == ITEM_STROKE && !item->deleted &&
        !rm_idmap_contains(&tree.removed, item->parent) &&
        rm_idmap_get(selected, item->id, &index)) {
      if (matched[index]) {
        has_duplicate_live = true;
        continue;
      }
      matched[index] = true;
      found += 1;
      if (out_first_block_starts != nullptr)
        out_first_block_starts[index] = item->block_start;
      if (!rm_idmap_set(&parents, item->parent, 1)) goto done;
    }
  }

  /* Two live blocks with one selected ID make the stroke to replace
   * ambiguous. `duplicate_first` decides which problem is reported when a
   * selection has both. */
  bool has_missing = found != selected->count;
  if (duplicate_first) {
    if (has_duplicate_live) {
      status = RM_ERR_UNVERIFIED;
      goto done;
    }
    if (has_missing) {
      status = RM_ERR_ITEMS_NOT_FOUND;
      goto done;
    }
  } else {
    if (has_missing) {
      status = RM_ERR_ITEMS_NOT_FOUND;
      goto done;
    }
    if (has_duplicate_live) {
      status = RM_ERR_UNVERIFIED;
      goto done;
    }
  }

  size_t cursor = 0;
  rm_crdt_id parent;
  uint64_t unused;
  while (rm_idmap_next(&parents, &cursor, &parent, &unused)) {
    const uint32_t* members;
    size_t n;
    if (!rm_grouping_find(&children, parent, &members, &n)) continue;
    uint32_t* order = rmi_chain_order(members, n, items, &n);
    if (!order) goto done;
    for (size_t j = 0; j < n; ++j) {
      const rmi_scene_item* item = &items[order[j]];
      uint64_t index;
      if (item->kind == ITEM_STROKE && !item->deleted &&
          rm_idmap_get(selected, item->id, &index)) {
        /* The successor is the next item with a different, non-zero ID,
         * live or not. */
        size_t k = j + 1u;

        while (k < n && (rm_crdt_equal(items[order[k]].id, item->id) ||
                         rm_crdt_equal(items[order[k]].id, RM_CRDT_NONE))) {
          k += 1u;
        }
        rm_crdt_id succ = k < n ? items[order[k]].id : RM_CRDT_NONE;
        /* An item whose left neighbour also follows it in the order has
         * links that contradict each other; refuse rather than guess. */
        if (!rm_crdt_equal(item->left, RM_CRDT_NONE) &&
            rm_crdt_equal(item->left, succ)) {
          free(order);
          status = RM_ERR_UNVERIFIED;
          goto done;
        }
        if (out_successors != nullptr) out_successors[index] = succ;
      }
    }
    free(order);
  }
  status = RM_OK;

done:
  free(matched);
  rm_grouping_free(&children);
  rm_idmap_free(&parents);
  rmi_tree_state_free(&tree);
  rmi_scene_destroy(&ctx);
  rmi_error_set(error, status, 0, 0, nullptr);
  return status;
}

/* The successors alone, with a missing stroke reported first. */
rm_status rmi_scene_stroke_successors(const uint8_t* data, size_t size,
                                      const rm_idmap* selected,
                                      rm_crdt_id* successors, rm_error* error) {
  return rmi_scene_resolve_stroke_targets(data, size, selected, false, nullptr,
                                          successors, error);
}

/* A layer's position comes from the root's child sequence. Keep its ordering
 * link separate from the node that holds its name and visibility. */
rm_status rmi_scene_layers(const uint8_t* data, size_t size,
                           rmi_layer_info** out, size_t* count,
                           rm_error* error) {
  if (!data || !out || !count) {
    rmi_error_set(error, RM_ERR_INVALID_ARGUMENT, 0, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }
  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks(data, size, &ctx, error));
  rmi_tree_state tree = {0};
  rm_idmap live;
  rm_idmap_init(&live);
  rm_vec indices, layers;
  rm_vec_init(&indices, sizeof(uint32_t));
  rm_vec_init(&layers, sizeof(rmi_layer_info));

  uint32_t* ordered = nullptr;
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  if (!rmi_tree_state_build(ctx.tree_events.data, ctx.tree_events.count, &tree))
    goto done;
  if ((tree.removed.count || tree.has_root_placements) &&
      !rmi_removed_subtrees(ctx.items.data, ctx.items.count, &tree))
    goto done;
  if (!rmi_append_unlinked_placements(&ctx, &tree)) goto done;
  const rmi_scene_item* items = ctx.items.data;
  rm_crdt_id root = RM_CRDT_ROOT;
  for (size_t i = 0; i < ctx.items.count; ++i) {
    if (!rm_crdt_equal(items[i].parent, root)) continue;
    if (i > UINT32_MAX) {
      status = RM_ERR_TOO_MANY_TO_NUMBER;
      goto done;
    }

    uint32_t index = (uint32_t)i;
    if (!rm_vec_push(&indices, &index)) goto done;
  }
  size_t n;
  ordered = rmi_chain_order(indices.data, indices.count, items, &n);
  if (!ordered) goto done;
  for (size_t i = 0; i < n; ++i) {
    const rmi_scene_item* item = &items[ordered[i]];
    if (item->kind != ITEM_GROUP || item->deleted ||
        rm_idmap_contains(&tree.removed, item->group) ||
        !rmi_tree_links(&tree, root, item->group))
      continue;
    /* One layer linked twice has no single position to report. */
    if (rm_idmap_contains(&live, item->group)) {
      status = RM_ERR_UNVERIFIED;
      goto done;
    }
    if (!rm_idmap_set(&live, item->group, 1)) goto done;

    rmi_layer_info layer = {
        .node = rm_item_id_of(item->group),
        .link = rm_item_id_of(item->id),
        .visible = !rm_idmap_contains(&ctx.hidden_nodes, item->group),
        .link_block = item->block_start,
    };
    if (!rm_vec_push(&layers, &layer)) goto done;
  }
  *out = layers.data;
  *count = layers.count;
  layers.data = nullptr;
  status = RM_OK;
done:
  rm_idmap_free(&live);
  rm_vec_free(&layers);
  rm_vec_free(&indices);
  free(ordered);
  rmi_tree_state_free(&tree);
  rmi_scene_destroy(&ctx);
  rmi_error_set(error, status, 0, 0, nullptr);
  return status;
}

/* Several blocks may describe the same node. Its name and its visibility each
 * have a write stamp of their own.
 *
 * Any output may be null. `*out_name` is the caller's to free, and is null
 * when the node has no name register. A block header that will not read is
 * RM_ERR_UNVERIFIED. */
rm_status rmi_scene_node_state(const uint8_t* data, size_t size,
                               rm_item_id node_id, bool* out_found,
                               bool* out_visible, char** out_name) {
  if (out_found) *out_found = false;
  if (out_visible) *out_visible = true;
  if (out_name) *out_name = nullptr;
  if (!data) return RM_ERR_INVALID_ARGUMENT;
  if (size < RM_HEADER_LENGTH) return RM_ERR_UNEXPECTED_EOF;

  rm_arena* arena = rm_arena_create();
  if (arena == nullptr) return RM_ERR_OUT_OF_MEMORY;

  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  rm_crdt_id target = rm_crdt_of(node_id);
  bool found = false;
  bool visible = true;
  rm_crdt_id vis_stamp = RM_CRDT_NONE;
  rm_crdt_id name_stamp = RM_CRDT_NONE;
  rm_str name = {.bytes = "", .length = 0};
  bool has_name = false;

  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header h;
    if (rm_read_block_header(&r, &h) != RM_OK) {
      rm_arena_destroy(arena);
      return RM_ERR_UNVERIFIED;
    }
    if (h.type == RM_BLOCK_TREE_NODE) {
      rm_reader body = r;
      body.size = h.end;
      rmi_tree_node node;
      rm_status pstatus = rmi_parse_tree_node(
          &body, h.end, out_name ? arena : nullptr, &node, nullptr);
      if (pstatus == RM_ERR_OUT_OF_MEMORY) {
        rm_arena_destroy(arena);
        return RM_ERR_OUT_OF_MEMORY;
      }

      if (pstatus == RM_OK && rm_crdt_equal(node.id, target)) {
        found = true;
        if (node.has_visible &&
            !rm_stamp_newer(vis_stamp, node.visibility_stamp)) {
          vis_stamp = node.visibility_stamp;
          visible = node.visible;
        }
        if (node.has_name && !rm_stamp_newer(name_stamp, node.name_stamp)) {
          name_stamp = node.name_stamp;
          name = node.name;
          has_name = true;
        }
      }
    }
    r.offset = h.end;
  }

  if (out_found) *out_found = found;
  if (out_visible) *out_visible = visible;
  if (out_name && found && has_name) {
    char* copy = malloc(name.length + 1u);
    if (copy == nullptr) {
      rm_arena_destroy(arena);
      return RM_ERR_OUT_OF_MEMORY;
    }
    if (name.length) memcpy(copy, name.bytes, name.length);
    copy[name.length] = '\0';
    *out_name = copy;
  }
  rm_arena_destroy(arena);
  return RM_OK;
}

/* Every living mark of the highlighter: its strokes on every layer the tablet
 * still holds — a hidden one included, since showing it again brings them
 * back — and every highlight over a text layer. Strokes on a layer taken out
 * of the tree are left out: no switch brings them back. */
rm_status rm_scan_highlighter_items(const uint8_t* data, size_t size,
                                    rm_vec* out_ids, rm_error* out_error) {
  rmi_scene ctx;
  RM_TRY(rmi_scene_read_blocks(data, size, &ctx, out_error));

  /* A page with a block that would not read is refused rather than answered
   * in part: the block lost may be a mark, and "every mark" less one is not
   * every mark. */
  size_t lost = ctx.unreadable_strokes + ctx.other_blocks;
  if (lost > 0) {
    rmi_scene_destroy(&ctx);
    char message[80];
    snprintf(message, sizeof message, "%zu blocks of the page would not read",
             lost);
    rmi_error_set(out_error, RM_ERR_INCOMPLETE_PAGE, 0, (long)lost, message);
    return RM_ERR_INCOMPLETE_PAGE;
  }

  rm_status status = RM_ERR_OUT_OF_MEMORY;
  rmi_tree_state tree = {0};
  rm_idmap seen;
  rm_idmap_init(&seen);
  if (!rmi_tree_state_build(ctx.tree_events.data, ctx.tree_events.count, &tree))
    goto done;
  if (!rmi_removed_subtrees(ctx.items.data, ctx.items.count, &tree)) goto done;
  const rm_idmap* removed = &tree.removed;

  /* In file order, strokes first, and each identifier once: an order that
   * does not change between two calls on the same bytes. */
  const rmi_scene_item* items = ctx.items.data;
  for (size_t i = 0; i < ctx.items.count; i += 1) {
    if (items[i].kind != ITEM_STROKE || items[i].deleted != 0) continue;
    if (!rm_pen_tool_is_highlighter(items[i].stroke.tool)) continue;
    if (rm_idmap_contains(removed, items[i].parent)) continue;
    if (rm_idmap_contains(&seen, items[i].id)) continue;
    rm_item_id id = rm_item_id_of(items[i].id);
    if (!rm_idmap_set(&seen, items[i].id, 1) || !rm_vec_push(out_ids, &id))
      goto done;
  }
  for (size_t i = 0; i < ctx.items.count; i += 1) {
    if (items[i].kind != ITEM_GLYPH ||
        rm_idmap_contains(removed, items[i].parent))
      continue;
    const rm_highlight* highlight = &items[i].glyph;
    rm_crdt_id key = rm_crdt_of(highlight->id);
    if (rm_idmap_contains(&seen, key)) continue;
    if (!rm_idmap_set(&seen, key, 1) || !rm_vec_push(out_ids, &highlight->id))
      goto done;
  }
  status = RM_OK;

done:
  rm_idmap_free(&seen);
  rmi_tree_state_free(&tree);
  rmi_scene_destroy(&ctx);
  if (status != RM_OK) rmi_error_set(out_error, status, 0, 0, nullptr);
  return status;
}
