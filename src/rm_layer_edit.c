/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Layer enumeration, addition, reordering, and deletion. */

#include <stdlib.h>
#include <string.h>

#include "rm_edit_ops.h"
#include "rm_scene.h"
#include "rmlines/parser.h"

static bool zero_id(rm_item_id id) { return !id.author && !id.counter; }

typedef enum layer_action { ADD_LAYER, MOVE_LAYER, DELETE_LAYER } layer_action;

/* A move is a new tree event about an existing node. Its operation ID and the
 * node it moves occupy different fields. With `node` set to none the record
 * creates the node named by `event` instead, the shape the tablet writes for
 * a layer's first placement. `attach` puts the node under the page root;
 * false, with a none parent, takes it out of the tree, which is how the
 * tablet deletes a layer. */
static void tree_move(rm_buf* out, rm_crdt_id event, rm_crdt_id node,
                      bool attach) {
  rm_buf body = {0};
  rm_write_id(&body, 1, event);
  rm_write_id(&body, 2, node);
  rm_write_tag(&body, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&body, attach ? 1u : 0u);
  rm_subblock_mark parent = rm_buf_begin_subblock(&body, 4);
  rm_write_id(&body, 1, attach ? RM_CRDT_ROOT : RM_CRDT_NONE);
  rm_buf_end_subblock(&body, parent);
  rm_write_top_block(out, RM_BLOCK_SCENE_TREE, 1, 1, body.bytes, body.count);
  rmi_buf_propagate(out, &body);
  rm_buf_free(&body);
}

/* Set the name on the node, where a subsequent rename can replace it
 * independently of the layer's position. Visibility is written as the
 * tablet's default: shown, under a zero stamp, so any real write wins over
 * it. Failure is recorded in `out`. */
static void layer_node(rm_buf* out, rm_crdt_id node, rm_crdt_id stamp,
                       const char* name) {
  char* repaired = rmi_utf8_repair_alloc(name);
  if (!repaired) {
    rmi_buf_fail(out, RM_BUF_ERR_OOM);
    return;
  }
  rm_buf body = {0};
  rm_write_id(&body, 1, node);
  rm_subblock_mark name_reg = rm_buf_begin_subblock(&body, 2);
  rm_write_id(&body, 1, stamp);
  rm_subblock_mark name_val = rm_buf_begin_subblock(&body, 2);
  rm_write_string(&body, repaired);
  rm_buf_end_subblock(&body, name_val);
  rm_buf_end_subblock(&body, name_reg);
  rm_write_stamped_u8(&body, 3, RM_CRDT_NONE, 1);
  rm_write_top_block(out, RM_BLOCK_TREE_NODE, 1, 2, body.bytes, body.count);
  rmi_buf_propagate(out, &body);
  rm_buf_free(&body);
  free(repaired);
}

/* The layer's position belongs to a group item under the root. Other layers
 * name that item's ID as a neighbour. The item is written live (deletion
 * count zero) and its value names the layer's node. */
static void layer_link(rm_buf* out, rm_crdt_id node, rm_crdt_id link,
                       rm_crdt_id left, rm_crdt_id right) {
  rm_buf body = {0};
  rm_write_id(&body, 1, RM_CRDT_ROOT);
  rm_write_id(&body, 2, link);
  rm_write_id(&body, 3, left);
  rm_write_id(&body, 4, right);
  rm_write_tag(&body, 5, RM_TAG_BYTE4);
  rm_write_u32(&body, 0);
  rm_subblock_mark value = rm_buf_begin_subblock(&body, 6);
  rm_buf_append_byte(&body, RM_VALUE_GROUP_ITEM);
  rm_write_id(&body, 2, node);
  rm_buf_end_subblock(&body, value);
  rm_write_top_block(out, RM_BLOCK_SCENE_GROUP_ITEM, 1, 1, body.bytes,
                     body.count);
  rmi_buf_propagate(out, &body);
  rm_buf_free(&body);
}

/* The current layer is a stamped reference to a node. The stamp dates the
 * selection and the value names the layer. */
static void write_current(rm_buf* field, rm_crdt_id stamp, rm_item_id active) {
  rm_subblock_mark v = rm_buf_begin_subblock(field, 1);
  rm_write_id(field, 1, stamp);
  rm_write_id(field, 2, rm_crdt_of(active));
  rm_buf_end_subblock(field, v);
}

/* One current-layer register: the block it is in, its stamp, and the layer
 * it names. */
typedef struct layer_winner {
  bool present;
  size_t block;
  rm_crdt_id stamp, layer;
} layer_winner;

/* The current layer as two readers see it. `logical` is the newest tag-1
 * register anywhere in a SceneInfo block; `leading` the newest among those
 * that open their block, the only place the tablet looks. `newest_stamp`
 * also counts registers whose layer would not read. */
typedef struct current_layer_state {
  size_t first_block;
  bool has_any_stamp;
  rm_crdt_id newest_stamp;
  layer_winner logical;
  layer_winner leading;
} current_layer_state;

/* What the tablet takes from SceneInfo reading fields in its fixed order:
 * which fields it reaches, and the winning template and PDF visibility. */
typedef struct ordered_scene_state {
  rm_crdt_id bg_stamp, doc_stamp;
  bool bg_visible, doc_visible;
  uint32_t field_mask;
} ordered_scene_state;

/* Follow the fields in tablet order to check which values remain readable
 * after a current-layer edit. The firmware reads tags 1, 2, 3, 5, 6, 7, 8, 9
 * in that order and steps past a tag that is not next, so a field out of
 * place is never reached; this mirrors that walk. */
static void read_ordered_block(rm_reader body, ordered_scene_state* out) {
  static constexpr uint32_t kOrder[8] = {1, 2, 3, 5, 6, 7, 8, 9};
  for (size_t i = 0; i < sizeof kOrder / sizeof *kOrder; i += 1) {
    uint32_t expected = kOrder[i];
    if (!rm_match_tag(&body, expected, RM_TAG_LENGTH4)) continue;
    rm_reader field = {0};
    if (rm_read_subblock(&body, &field) != RM_OK) break;
    out->field_mask |= (1u << expected);
    if (expected == 2 || expected == 3) {
      rm_crdt_id stamp = RM_CRDT_NONE;
      (void)rm_read_crdt(&field, 1, &stamp);
      uint8_t flag = 0;
      if (rm_match_tag(&field, 2, RM_TAG_BYTE1) &&
          rm_read_u8(&field, &flag) == RM_OK) {
        if (expected == 2 && !rm_stamp_newer(out->bg_stamp, stamp)) {
          out->bg_stamp = stamp;
          out->bg_visible = flag != 0;
        } else if (expected == 3 && !rm_stamp_newer(out->doc_stamp, stamp)) {
          out->doc_stamp = stamp;
          out->doc_visible = flag != 0;
        }
      }
    }
  }
}

/* The ordered reading of every SceneInfo block on the page, with the newest
 * visibility of each kind winning across blocks. Absent flags read as
 * shown. */
static ordered_scene_state read_ordered_scene_info(rm_reader r) {
  ordered_scene_state st = {.bg_visible = true, .doc_visible = true};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    if (rm_read_block_header(&r, &b) != RM_OK) break;
    if (b.type == RM_BLOCK_SCENE_INFO) {
      rm_reader body = r;
      body.size = b.end;
      read_ordered_block(body, &st);
    }
    r.offset = b.end;
  }
  return st;
}

/* The tablet keeps the current layer in SceneInfo's leading register. Also
 * track later tag-1 fields: an edit needs a stamp newer than all of them and
 * must know which block supplies each winning value. */
static rm_status find_current_layer(rm_reader r, current_layer_state* out) {
  *out = (current_layer_state){0};
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&r, &b));
    if (b.type == RM_BLOCK_SCENE_INFO) {
      rm_reader body = r;
      body.size = b.end;
      if (!out->first_block) out->first_block = b.start;
      while (rm_reader_remaining(&body) > 0) {
        bool is_leading = body.offset == b.body;
        uint32_t tag;
        uint8_t type;
        if (rm_read_tag(&body, &tag, &type) != RM_OK) break;
        rm_reader field = {0};
        if (rmi_skip_tagged_value(&body, type, &field) != RM_OK) break;
        if (tag != 1 || type != RM_TAG_LENGTH4) continue;

        rm_crdt_id stamp, layer;
        if (rm_read_crdt(&field, 1, &stamp) != RM_OK) continue;
        if (!out->has_any_stamp || !rm_stamp_newer(out->newest_stamp, stamp)) {
          out->has_any_stamp = true;
          out->newest_stamp = stamp;
        }
        if (rm_read_crdt(&field, 2, &layer) != RM_OK) continue;
        if (!out->logical.present ||
            !rm_stamp_newer(out->logical.stamp, stamp)) {
          out->logical = (layer_winner){.present = true,
                                        .block = b.start,
                                        .stamp = stamp,
                                        .layer = layer};
        }

        if (is_leading && (!out->leading.present ||
                           !rm_stamp_newer(out->leading.stamp, stamp))) {
          out->leading = (layer_winner){.present = true,
                                        .block = b.start,
                                        .stamp = stamp,
                                        .layer = layer};
        }
      }
    }
    r.offset = b.end;
  }
  return RM_OK;
}

/* Whether `target` is the node of one of the live layers. */
static bool has_layer_node(const rmi_layer_info* layers, size_t count,
                           rm_crdt_id target) {
  for (size_t i = 0; i < count; i += 1) {
    if (rm_crdt_equal(rm_crdt_of(layers[i].node), target)) return true;
  }
  return false;
}

/* A node's creation and a later move have different operation IDs. Keep those
 * IDs occupied when making placements explicit. */
static rm_status collect_scene_tree_op_ids(rm_reader r, rm_idmap* out_ops) {
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    RM_TRY(rm_read_block_header(&r, &b));
    if (b.type == RM_BLOCK_SCENE_TREE) {
      rm_reader body = r;
      body.size = b.end;
      rm_crdt_id op_id;
      if (rm_read_crdt(&body, 1, &op_id) == RM_OK &&
          !rm_crdt_equal(op_id, RM_CRDT_NONE)) {
        if (!rm_idmap_set(out_ops, op_id, 1)) return RM_ERR_OUT_OF_MEMORY;
      }
    }
    r.offset = b.end;
  }
  return RM_OK;
}

/* Preserve the rest of SceneInfo, including fields following the selected
 * layer. A new selection must remain readable in the leading register.
 *
 * The block is rebuilt with one fresh tag-1 register first and every other
 * field after it in its original order. If that would change what the
 * tablet's ordered read sees, or an old tag-1 register held more than a
 * stamp and a layer, the block is copied unchanged and `out_rewritten` stays
 * false; the caller then appends a SceneInfo block of its own instead. */
static rm_status rewrite_scene_info_block(rm_buf* out, rm_reader body,
                                          rm_block_header b, rm_crdt_id stamp,
                                          rm_item_id active,
                                          bool* out_rewritten) {
  *out_rewritten = false;
  rm_buf normalized = {0};
  write_current(&normalized, stamp, active);
  bool before_has_tag1 = false;
  bool has_extra_tag1 = false;
  rm_reader scan = body;
  while (rm_reader_remaining(&scan) > 0) {
    size_t start = scan.offset;
    uint32_t tag;
    uint8_t type;
    /* Bytes past the last field that reads are kept as they are. */
    if (rm_read_tag(&scan, &tag, &type) != RM_OK ||
        rmi_skip_tagged_value(&scan, type, nullptr) != RM_OK) {
      rm_buf_append(&normalized, scan.data + start, scan.size - start);
      break;
    }

    /* Old current-layer registers are dropped. One that carries anything
     * besides a stamp and a layer would lose that content, so it is noted. */
    if (tag == 1 && type == RM_TAG_LENGTH4) {
      before_has_tag1 = true;
      rm_reader f = {.data = scan.data, .size = scan.offset, .offset = start};
      uint32_t ft;
      uint8_t ftp;
      rm_reader val = {0};
      if (rm_read_tag(&f, &ft, &ftp) == RM_OK &&
          rmi_skip_tagged_value(&f, ftp, &val) == RM_OK) {
        rm_crdt_id s, l;
        if (rm_read_crdt(&val, 1, &s) != RM_OK ||
            rm_read_crdt(&val, 2, &l) != RM_OK ||
            rm_reader_remaining(&val) > 0) {
          has_extra_tag1 = true;
        }
      } else {
        has_extra_tag1 = true;
      }
      continue;
    }

    rm_buf_append(&normalized, scan.data + start, scan.offset - start);
  }
  if (normalized.error != RM_BUF_OK) {
    rm_status st = rmi_buf_status(&normalized);
    rm_buf_free(&normalized);
    return st;
  }

  /* The rebuilt block must reach the same fields as before, plus tag 1 if it
   * had none, and give the same template and PDF visibility. */
  ordered_scene_state before = {.bg_visible = true, .doc_visible = true};
  read_ordered_block(body, &before);
  ordered_scene_state after = {.bg_visible = true, .doc_visible = true};
  read_ordered_block(
      (rm_reader){.data = normalized.bytes, .size = normalized.count}, &after);
  uint32_t expected_mask =
      before_has_tag1 ? before.field_mask : (before.field_mask | (1u << 1));
  bool order_changed = has_extra_tag1 || expected_mask != after.field_mask ||
                       before.bg_visible != after.bg_visible ||
                       before.doc_visible != after.doc_visible ||
                       !rm_crdt_equal(before.bg_stamp, after.bg_stamp) ||
                       !rm_crdt_equal(before.doc_stamp, after.doc_stamp);
  if (order_changed) {
    rm_buf_free(&normalized);
    rm_buf_append(out, body.data + b.start, b.end - b.start);
    return rmi_buf_status(out);
  }

  if (normalized.count > rmi_block_limit()) {
    rm_buf_free(&normalized);
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  /* New body length, then the original four header bytes after it. */
  rm_write_u32(out, (uint32_t)normalized.count);
  rm_buf_append(out, body.data + b.start + 4, 4);
  rm_buf_append(out, normalized.bytes, normalized.count);
  rm_buf_free(&normalized);
  *out_rewritten = true;
  return rmi_buf_status(out);
}

/* The public view of the live layers. The array is the caller's; it is
 * allocated even for no layers so a successful call never returns null. */
rm_status rmi_edit_layers(const uint8_t* data, size_t size, rm_edit_layer** out,
                          size_t* count, rm_error* error) {
  if (!data || !out || !count) {
    rmi_error_set(error, RM_ERR_INVALID_ARGUMENT, 0, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }
  rmi_layer_info* raw = nullptr;
  size_t n = 0;
  RM_TRY(rmi_scene_layers(data, size, &raw, &n, error));
  rm_edit_layer* layers = calloc(n == 0 ? 1 : n, sizeof *layers);
  if (!layers) {
    free(raw);
    rmi_error_set(error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
    return RM_ERR_OUT_OF_MEMORY;
  }
  for (size_t i = 0; i < n; ++i) {
    layers[i] = (rm_edit_layer){
        .node = raw[i].node, .link = raw[i].link, .visible = raw[i].visible};
  }
  free(raw);
  *out = layers;
  *count = n;
  return RM_OK;
}

/* Edit placement and ordering together, then read the layers back. Changing a
 * link alone can leave a layer under its old parent.
 *
 * IDs are taken from `first` in this order. Add: the new node (its creation
 * record), its name stamp, its ordering link, the current-layer stamp. Move:
 * the new ordering link. Delete: the detaching tree record, the
 * current-layer stamp. Add and delete also make the new or the surviving
 * layer current, as the tablet does. The candidate is owned by the caller
 * and left unverified, except for a move to the same place, which returns
 * an unchanged copy. */
static rm_status change_layer(const uint8_t* data, size_t size,
                              layer_action action, rm_item_id node,
                              size_t position, const char* name,
                              rm_item_id first, rmi_edit_candidate* out_cand,
                              rm_error* error) {
  *out_cand = (rmi_edit_candidate){0};
  if (error) *error = (rm_error){0};
  size_t needed = action == ADD_LAYER ? 4 : action == MOVE_LAYER ? 1 : 2;
  if (!data || !first.author || !first.counter ||
      needed - 1 > UINT64_MAX - first.counter || (action == ADD_LAYER && !name))
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (size < RM_HEADER_LENGTH)
    return rm_edit_result(error, RM_ERR_UNEXPECTED_EOF);

  /* Every reserved ID must be unused anywhere on the page. */
  RM_TRY(rm_edit_result(error,
                        rmi_scene_check_ids(data, size, first, needed, error)));
  rmi_layer_info* layers = nullptr;
  size_t count = 0;
  RM_TRY(rmi_scene_layers(data, size, &layers, &count, error));

  rmi_tree_state pre_tree = {0};
  bool have_pre_tree = false;
  rm_idmap scene_tree_ops = {0};
  bool have_tree_ops = false;
  rm_buf out = {0};
  rm_status status = RM_ERR_INVALID_ARGUMENT;

  /* Add needs to know whether the page already has explicit placements.
   * Delete builds the tree too, so a tree that would not build refuses
   * it. */
  if (action != MOVE_LAYER) {
    status = rmi_scene_live_removed_nodes(data, size, &pre_tree, error);
    if (status != RM_OK) goto done;
    have_pre_tree = true;
    status = RM_ERR_INVALID_ARGUMENT;
  }

  /* Five layers is the tablet's limit. A page always keeps one layer. */
  size_t index = 0;
  while (index < count && !rm_item_id_equal(layers[index].node, node)) ++index;
  if (action == ADD_LAYER && (index < count || count >= 5 || position > count))
    goto done;
  if (action != ADD_LAYER && index == count) {
    status = RM_ERR_ITEMS_NOT_FOUND;
    goto done;
  }
  if (action == DELETE_LAYER && count < 2) goto done;
  if (action == MOVE_LAYER && position >= count) goto done;
  /* Moving a layer to where it already is changes nothing. The copy is
   * byte-identical, so the session treats it as a no-op. */
  if (action == MOVE_LAYER && position == index) {
    uint8_t* copy = malloc(size ? size : 1);
    if (!copy) {
      status = RM_ERR_OUT_OF_MEMORY;
      goto done;
    }
    if (size) memcpy(copy, data, size);
    out_cand->bytes = copy;
    out_cand->size = size;
    out_cand->verified = true;
    status = RM_OK;
    goto done;
  }

  rm_item_id active = node, old_link = {0};
  size_t old_link_block = 0;
  if (action != ADD_LAYER) {
    old_link = layers[index].link;
    old_link_block = layers[index].link_block;
  }
  /* After a delete the layer above becomes current, or the one below when
   * the top layer goes. */
  if (action == DELETE_LAYER)
    active = layers[index + 1 < count ? index + 1 : index - 1].node;
  size_t expected_count = action == ADD_LAYER      ? count + 1
                          : action == DELETE_LAYER ? count - 1
                                                   : count;
  /* Take the moved layer out of the list so that `position` indexes the
   * layers it will sit between. */
  if (action == MOVE_LAYER) {
    memmove(layers + index, layers + index + 1,
            (count - index - 1) * sizeof *layers);
    --count;
  }
  /* A layer without a live ordering link has no ID a new link could name as
   * its left neighbour, so nothing can be placed after it reliably. */
  if (action != DELETE_LAYER && position > 0 &&
      zero_id(layers[position - 1].link)) {
    status = RM_ERR_UNVERIFIED;
    goto done;
  }

  /* The current-layer stamp is the last reserved ID, newer than the rest. */
  rm_crdt_id next = rm_crdt_of(first), stamp = next;
  stamp.counter += needed - 1;
  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  rm_buf_append(&out, r.data, RM_HEADER_LENGTH);
  current_layer_state pre_state = {0};
  ordered_scene_state pre_ordered = {.bg_visible = true, .doc_visible = true};
  size_t target_block = 0;
  bool update_scene_info = false;
  bool preserved_scene_info = false;
  bool need_new_scene_info = false;
  rm_crdt_id target_stamp = RM_CRDT_NONE;
  rm_item_id target_active = {0};

  if (action != MOVE_LAYER) {
    status = find_current_layer(r, &pre_state);
    if (status != RM_OK) goto done;
    pre_ordered = read_ordered_scene_info(r);

    /* Deleting a layer that is not current leaves the selection alone. If
     * the selection names a layer that is not live, the page is in a state
     * this code does not try to repair. */
    if (action == DELETE_LAYER &&
        (!pre_state.logical.present ||
         !rm_crdt_equal(pre_state.logical.layer, rm_crdt_of(node)))) {
      if (pre_state.logical.present &&
          !rm_crdt_equal(pre_state.logical.layer, RM_CRDT_NONE) &&
          !has_layer_node(layers, count, pre_state.logical.layer)) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }
      /* The tablet reads only the leading register. If that one names the
       * deleted layer while a later register has the newer selection, copy
       * the newer selection, stamp and all, into the leading position. */
      if (pre_state.leading.present &&
          rm_crdt_equal(pre_state.leading.layer, rm_crdt_of(node))) {
        if (!pre_state.logical.present ||
            rm_crdt_equal(pre_state.logical.layer, RM_CRDT_NONE) ||
            !has_layer_node(layers, count, pre_state.logical.layer)) {
          status = RM_ERR_UNVERIFIED;
          goto done;
        }

        update_scene_info = true;
        target_block = pre_state.leading.block;
        target_stamp = pre_state.logical.stamp;
        target_active = rm_item_id_of(pre_state.logical.layer);
      } else {
        preserved_scene_info = true;
      }
    } else {
      /* A new selection must beat every current-layer register on the page,
       * readable or not, or it would lose the LWW comparison. */
      if ((pre_state.logical.present &&
           !rm_stamp_newer(stamp, pre_state.logical.stamp)) ||
          (pre_state.has_any_stamp &&
           !rm_stamp_newer(stamp, pre_state.newest_stamp))) {
        status = RM_ERR_INVALID_ARGUMENT;
        goto done;
      }
      update_scene_info = true;
      target_stamp = stamp;
      target_active = active;
      /* Write into the block holding the current winner, or the first
       * SceneInfo block; a page without one gets a new block on add. */
      target_block = pre_state.logical.present ? pre_state.logical.block
                                               : pre_state.first_block;
      if (target_block == 0 && action == ADD_LAYER) need_new_scene_info = true;
    }
  }

  /* Copy the page block by block, rewriting the SceneInfo block chosen above
   * and, for a move, the layer's old ordering link. */
  bool removed = false;
  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    status = rm_read_block_header(&r, &b);
    if (status != RM_OK) goto done;
    rm_reader body = r;
    body.size = b.end;
    bool copied = false;
    if (update_scene_info && target_block != 0 && b.start == target_block) {
      bool block_rewritten = false;
      status = rewrite_scene_info_block(&out, body, b, target_stamp,
                                        target_active, &block_rewritten);
      if (status != RM_OK) goto done;
      if (!block_rewritten) need_new_scene_info = true;
      copied = true;
    } else if (action == MOVE_LAYER && !zero_id(old_link) &&
               b.start == old_link_block) {
      if (b.type != RM_BLOCK_SCENE_GROUP_ITEM) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }

      /* The tablet moves a layer by tombstoning its old link and writing a
       * new one. Keep the item's preamble, drop its value, and set the
       * deletion count, the preamble's last four bytes, to one. */
      rm_item_preamble it;
      status = rm_parse_item_preamble(&body, &it);
      if (status != RM_OK) goto done;
      if (removed || it.deleted ||
          !rm_crdt_equal(it.id, rm_crdt_of(old_link))) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }
      rm_write_u32(&out, (uint32_t)(body.offset - b.body));
      rm_buf_append(&out, r.data + b.start + 4, body.offset - b.start - 8);
      rm_write_u32(&out, 1);
      copied = removed = true;
    }
    if (!copied) rm_buf_append(&out, r.data + b.start, b.end - b.start);
    r.offset = b.end;
  }
  if (action == MOVE_LAYER && !zero_id(old_link) && !removed) {
    status = RM_ERR_UNVERIFIED;
    goto done;
  }

  /* A page whose layers have no placement records is read from its group
   * links alone. Once the new layer brings the first placement, the reader
   * keeps only nodes reachable through placements, so every existing layer
   * gets its creation record now. Each record takes the layer's node ID as
   * its own, which must not already be in use. */
  if (action == ADD_LAYER && !pre_tree.has_root_placements) {
    rm_idmap_init(&scene_tree_ops);
    have_tree_ops = true;
    status = collect_scene_tree_op_ids(
        (rm_reader){.data = data, .size = size, .offset = RM_HEADER_LENGTH},
        &scene_tree_ops);
    if (status != RM_OK) goto done;
    for (size_t k = 0; k < count; ++k) {
      if (layers[k].node.author == 0 || layers[k].node.counter == 0 ||
          rm_idmap_contains(&scene_tree_ops, rm_crdt_of(layers[k].node))) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }

      rm_status id_st =
          rmi_scene_check_layer_materialization_id(data, size, layers[k].node);
      if (id_st != RM_OK) {
        status = (id_st == RM_ERR_OUT_OF_MEMORY) ? id_st : RM_ERR_UNVERIFIED;
        goto done;
      }
    }
    for (size_t k = 0; k < count; ++k) {
      tree_move(&out, rm_crdt_of(layers[k].node), RM_CRDT_NONE, true);
    }
  }

  if (action == DELETE_LAYER)
    tree_move(&out, next, rm_crdt_of(node), false);
  else {
    if (action == ADD_LAYER) {
      tree_move(&out, next, RM_CRDT_NONE, true);
      ++next.counter;
      layer_node(&out, rm_crdt_of(node), next, name);
      ++next.counter;
    }
    rm_crdt_id left =
        position ? rm_crdt_of(layers[position - 1].link) : RM_CRDT_NONE;
    rm_crdt_id right =
        position < count ? rm_crdt_of(layers[position].link) : RM_CRDT_NONE;
    layer_link(&out, rm_crdt_of(node), next, left, right);
  }
  /* A SceneInfo block holding only the current layer. Its size is known only
   * once written, so the length is filled in afterwards. */
  if (need_new_scene_info) {
    size_t len_at = out.count;
    rm_write_u32(&out, 0);

    const uint8_t hdr[4] = {0, 0, 1, RM_BLOCK_SCENE_INFO};
    rm_buf_append(&out, hdr, sizeof hdr);
    size_t body_start = out.count;
    write_current(&out, target_stamp, target_active);
    size_t body_len = out.count - body_start;
    if (body_len > rmi_block_limit()) {
      status = RM_ERR_TOO_MANY_TO_NUMBER;
      goto done;
    }
    rm_write_u32_at(&out, len_at, (uint32_t)body_len);
  }
  /* Bytes too short to be a block stay at the end of the page. */
  rm_buf_append(&out, r.data + r.offset, rm_reader_remaining(&r));
  status = rmi_buf_status(&out);
  if (status != RM_OK) goto done;

  /* Read the layers back from the new bytes: the count must be the one
   * intended and the layer must sit at `position`; a deleted layer must be
   * gone and the layer made current must still be there. */
  rmi_layer_info* result = nullptr;
  size_t actual = 0;
  status = rmi_scene_layers(out.bytes, out.count, &result, &actual, error);
  if (status != RM_OK) goto done;
  if (actual != expected_count)
    status = RM_ERR_UNVERIFIED;
  else if (action != DELETE_LAYER &&
           !rm_item_id_equal(result[position].node, node))
    status = RM_ERR_UNVERIFIED;
  else if (action == DELETE_LAYER) {
    bool survivor = false;
    for (size_t i = 0; i < actual; i++) {
      if (rm_item_id_equal(result[i].node, node)) status = RM_ERR_UNVERIFIED;
      if (rm_item_id_equal(result[i].node, active)) survivor = true;
    }
    if (!survivor) status = RM_ERR_UNVERIFIED;
  }
  free(result);
  if (status != RM_OK) goto done;

  /* Read the selection back. A preserved selection must be exactly as it
   * was; an updated one must name the new layer, with the new stamp, for
   * both readers. Template and PDF visibility must be unchanged. */
  if (action != MOVE_LAYER) {
    rm_reader cand_r = {
        .data = out.bytes, .size = out.count, .offset = RM_HEADER_LENGTH};
    current_layer_state post_state = {0};
    status = find_current_layer(cand_r, &post_state);
    if (status != RM_OK) goto done;
    ordered_scene_state post_ordered = read_ordered_scene_info(cand_r);

    bool ordered_ok = pre_ordered.bg_visible == post_ordered.bg_visible &&
                      pre_ordered.doc_visible == post_ordered.doc_visible;
    if (preserved_scene_info) {
      if (!ordered_ok ||
          post_state.logical.present != pre_state.logical.present ||
          post_state.leading.present != pre_state.leading.present ||
          (post_state.logical.present &&
           (!rm_crdt_equal(post_state.logical.stamp, pre_state.logical.stamp) ||
            !rm_crdt_equal(post_state.logical.layer,
                           pre_state.logical.layer))) ||
          (post_state.leading.present &&
           (!rm_crdt_equal(post_state.leading.stamp, pre_state.leading.stamp) ||
            !rm_crdt_equal(post_state.leading.layer,
                           pre_state.leading.layer)))) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }
    } else if (update_scene_info) {
      if (!ordered_ok || !post_state.logical.present ||
          !post_state.leading.present ||
          !rm_crdt_equal(post_state.logical.layer, rm_crdt_of(target_active)) ||
          !rm_crdt_equal(post_state.leading.layer, rm_crdt_of(target_active)) ||
          !rm_crdt_equal(post_state.logical.stamp, target_stamp) ||
          !rm_crdt_equal(post_state.leading.stamp, target_stamp)) {
        status = RM_ERR_UNVERIFIED;
        goto done;
      }
    }
  }

  size_t out_size = 0;
  uint8_t* bytes = rm_buf_take(&out, &out_size);
  if (!bytes) {
    status = RM_ERR_OUT_OF_MEMORY;
    goto done;
  }
  out_cand->bytes = bytes;
  out_cand->size = out_size;
  out_cand->verified = false;
  status = RM_OK;
done:
  if (have_tree_ops) rm_idmap_free(&scene_tree_ops);
  if (have_pre_tree) rmi_tree_state_free(&pre_tree);
  rm_buf_free(&out);
  free(layers);
  return status;
}

/* The first reserved ID becomes the new layer's node. */
rm_status rmi_edit_add_layer(const uint8_t* data, size_t size, size_t position,
                             const char* name, rm_item_id first,
                             rmi_edit_candidate* out, rm_error* error) {
  return rm_edit_result(error, change_layer(data, size, ADD_LAYER, first,
                                            position, name, first, out, error));
}

rm_status rmi_edit_move_layer(const uint8_t* data, size_t size, rm_item_id node,
                              size_t position, rm_item_id first,
                              rmi_edit_candidate* out, rm_error* error) {
  return rm_edit_result(
      error, change_layer(data, size, MOVE_LAYER, node, position, nullptr,
                          first, out, error));
}

rm_status rmi_edit_delete_layer(const uint8_t* data, size_t size,
                                rm_item_id node, rm_item_id first,
                                rmi_edit_candidate* out, rm_error* error) {
  return rm_edit_result(error, change_layer(data, size, DELETE_LAYER, node, 0,
                                            nullptr, first, out, error));
}
