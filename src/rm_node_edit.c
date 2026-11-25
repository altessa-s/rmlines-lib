/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* LWW updates on SceneTreeNode (0x02) blocks. */

#include <stdlib.h>
#include <string.h>

#include "rm_edit_ops.h"
#include "rm_scene.h"
#include "rmlines/parser.h"

/* Where one register of a node sits inside its block, as offsets into the
 * page. `replace_begin`..`replace_end` is the stamp and value that a new
 * write replaces; `sub_len_at` is the register's own four-byte length and
 * `sub_end` the end of the register, so the bytes between `replace_end` and
 * `sub_end` are carried over untouched. When `found` is false the node has no
 * such register and `replace_begin == replace_end` is where a new one goes. */
typedef struct node_field_loc {
  bool found;
  size_t replace_begin;
  size_t replace_end;
  size_t sub_len_at;
  size_t sub_end;
  rm_crdt_id stamp;
} node_field_loc;

/* Locate the writable part of a name or visibility register. The bytes past
 * its known value still belong to the node. */
static rm_status inspect_register(rm_reader sub, uint32_t field,
                                  rm_item_id stamp, node_field_loc* loc,
                                  size_t len_at) {
  /* A register this code cannot read is skipped rather than refused: it is
   * not counted as a value and is left in the block as it is. */
  rm_crdt_id candidate = RM_CRDT_NONE;
  if (!rm_match_tag(&sub, 1, RM_TAG_ID) ||
      rm_read_bare_crdt(&sub, &candidate) != RM_OK) {
    return RM_OK;
  }

  uint32_t val_tag;
  uint8_t val_type;
  if (rm_read_tag(&sub, &val_tag, &val_type) != RM_OK || val_tag != 2)
    return RM_OK;
  /* Visibility (field 3) is a single byte; the name is a string. */
  uint8_t expected_type = field == 3 ? RM_TAG_BYTE1 : RM_TAG_LENGTH4;
  if (val_type != expected_type) return RM_OK;

  if (val_type == RM_TAG_BYTE1) {
    if (rmi_skip_tagged_value(&sub, val_type, nullptr) != RM_OK) return RM_OK;
  } else {
    rm_reader str_sub = {0};
    if (rm_read_subblock(&sub, &str_sub) != RM_OK) return RM_OK;
    rm_arena* arena = rm_arena_create();
    if (arena == nullptr) return RM_ERR_OUT_OF_MEMORY;
    rm_str ignored = {0};
    rm_status sstatus = rm_read_string(&str_sub, str_sub.size, arena, &ignored);
    rm_arena_destroy(arena);
    if (sstatus == RM_ERR_OUT_OF_MEMORY) return RM_ERR_OUT_OF_MEMORY;
    if (sstatus != RM_OK) return RM_OK;
  }

  /* Last writer wins: a stamp no newer than a value already on the page would
   * lose to it on every reader, and the edit would silently do nothing. */
  if (!rm_stamp_newer(rm_crdt_of(stamp), candidate))
    return RM_ERR_INVALID_ARGUMENT;

  /* Keep the newest value seen so far; on a tie the later one is taken. */
  if (!loc->found || !rm_stamp_newer(loc->stamp, candidate)) {
    loc->found = true;
    loc->stamp = candidate;
    loc->sub_len_at = len_at;
    loc->replace_begin = len_at + 4u;
    loc->replace_end = sub.offset;
    loc->sub_end = sub.size;
  }
  return RM_OK;
}

/* Find `field` in one TreeNode block body. Returns RM_ERR_ITEMS_NOT_FOUND
 * when the block describes another node and RM_ERR_INVALID_ARGUMENT when the
 * block already holds a value with a stamp at least as new as `stamp`. On
 * RM_OK `out` holds the newest register of the field in this block, or, when
 * it has none, the offset where a new register keeps the fields in order. */
static rm_status node_field(rm_reader r, rm_item_id node, rm_item_id stamp,
                            uint32_t field, node_field_loc* out) {
  *out = (node_field_loc){0};
  rm_crdt_id id;
  if (rm_read_crdt(&r, 1, &id) != RM_OK) return RM_ERR_UNVERIFIED;
  if (!rm_crdt_equal(id, rm_crdt_of(node))) return RM_ERR_ITEMS_NOT_FOUND;

  /* A node stores its numbered LWW fields in ascending order. Keep the suffix
   * opaque, including anchor records and future extensions. */
  size_t insert_at = r.offset;
  bool have_insert = false;
  size_t end_valid = r.offset;

  while (rm_reader_remaining(&r) > 0) {
    size_t start = r.offset;
    uint32_t index;
    uint8_t type;
    if (rm_read_tag(&r, &index, &type) != RM_OK) break;
    if (index == field && type == RM_TAG_LENGTH4) {
      size_t len_at = r.offset;
      rm_reader sub;
      if (rm_read_subblock(&r, &sub) != RM_OK) break;
      RM_TRY(inspect_register(sub, field, stamp, out, len_at));
    } else {
      if (rmi_skip_tagged_value(&r, type, nullptr) != RM_OK) break;
    }
    end_valid = r.offset;
    if (!have_insert && index > field) {
      insert_at = start;
      have_insert = true;
    }
  }

  /* No register yet: insert before the first higher field, or after the
   * last field that would read. */
  if (!out->found) {
    size_t pos = have_insert ? insert_at : end_valid;
    out->replace_begin = pos;
    out->replace_end = pos;
  }
  return RM_OK;
}

/* Update the winning register and re-read the node. A later block can carry
 * an older stamp, so file position alone cannot select the value.
 *
 * `inner_bytes` is the register's new stamp and value, without the register's
 * own tag and length. `out` receives the rewritten page, owned by the caller;
 * it is left unverified so the session checks the whole page again. */
static rm_status set_node(const uint8_t* data, size_t size, rm_item_id node,
                          rm_item_id stamp, uint32_t field,
                          const rm_buf* inner_bytes, bool is_visible,
                          bool expect_visible, const char* expect_name,
                          rmi_edit_candidate* out, rm_error* error) {
  *out = (rmi_edit_candidate){0};
  if (!data || !stamp.author || !stamp.counter)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (size < RM_HEADER_LENGTH)
    return rm_edit_result(error, RM_ERR_UNEXPECTED_EOF);
  rm_status buf_st = rmi_buf_status(inner_bytes);
  if (buf_st != RM_OK) return rm_edit_result(error, buf_st);

  /* The stamp is an ID of its own: reusing one the page already holds would
   * make two writes indistinguishable. */
  RM_TRY(
      rm_edit_result(error, rmi_scene_check_ids(data, size, stamp, 1, error)));

  rm_reader r = {.data = data, .size = size, .offset = RM_HEADER_LENGTH};
  rm_block_header target = {0};
  node_field_loc winning = {0};
  bool have_target = false;

  while (rm_reader_remaining(&r) >= 8) {
    rm_block_header b;
    rm_status status = rm_read_block_header(&r, &b);
    if (status != RM_OK) return rm_edit_result(error, status);
    if (b.type == RM_BLOCK_TREE_NODE) {
      rm_reader body = r;
      body.size = b.end;
      node_field_loc loc;
      status = node_field(body, node, stamp, field, &loc);
      /* Several blocks may describe the node. Write into the one holding the
       * newest value, so the new register replaces the value readers show;
       * with no value anywhere, the first block of the node takes it. */
      if (status == RM_OK) {
        if (!have_target || (loc.found && !winning.found) ||
            (loc.found && winning.found &&
             !rm_stamp_newer(winning.stamp, loc.stamp))) {
          have_target = true;
          target = b;
          winning = loc;
        }
      } else if (status != RM_ERR_ITEMS_NOT_FOUND) {
        return rm_edit_result(error, status);
      }
    }

    r.offset = b.end;
  }
  if (!have_target) return rm_edit_result(error, RM_ERR_ITEMS_NOT_FOUND);
  /* A node whose block survives can still have been taken out of the tree;
   * such a node is refused as not found. */
  RM_TRY(rmi_scene_check_location(data, size, node, (rm_item_id){0},
                                  (rm_item_id){0}, true, error));

  size_t limit = rmi_block_limit();
  rm_buf spliced = {0};
  if (winning.found) {
    /* Replace the stamp and value in place. Both the register's length and
     * the block's length change with them, and both must still fit in the
     * four bytes that hold them. */
    size_t tail_len = winning.sub_end - winning.replace_end;
    if (inner_bytes->count > limit - (limit < tail_len ? limit : tail_len) ||
        tail_len > limit)
      return rm_edit_result(error, RM_ERR_TOO_MANY_TO_NUMBER);
    uint32_t new_sub_len = (uint32_t)(inner_bytes->count + tail_len);

    size_t old_inner = winning.replace_end - winning.replace_begin;
    size_t kept = target.end - target.body - old_inner;
    if (kept > limit || inner_bytes->count > limit - kept)
      return rm_edit_result(error, RM_ERR_TOO_MANY_TO_NUMBER);
    /* Page up to the block, new block length, the block up to the register's
     * length, new register length, new value, then everything after the old
     * value: the register's unread tail and the rest of the page. */
    rm_buf_append(&spliced, data, target.start);
    rm_write_u32(&spliced, (uint32_t)(kept + inner_bytes->count));
    rm_buf_append(&spliced, data + target.start + 4u,
                  winning.sub_len_at - target.start - 4u);
    rm_write_u32(&spliced, new_sub_len);
    rm_buf_append(&spliced, inner_bytes->bytes, inner_bytes->count);
    rm_buf_append(&spliced, data + winning.replace_end,
                  size - winning.replace_end);
  } else {
    /* Insert a whole new register. Its size is known only once written, so
     * the block length is reserved here and filled in afterwards. */
    size_t kept = target.end - target.body;
    rm_buf_append(&spliced, data, target.start);

    size_t len_at = spliced.count;
    rm_write_u32(&spliced, 0);
    rm_buf_append(&spliced, data + target.start + 4u,
                  winning.replace_begin - target.start - 4u);
    size_t field_start = spliced.count;
    rm_write_block(&spliced, field, inner_bytes->bytes, inner_bytes->count);
    rm_status s_st = rmi_buf_status(&spliced);
    if (s_st != RM_OK) {
      rm_buf_free(&spliced);
      return rm_edit_result(error, s_st);
    }
    size_t added = spliced.count - field_start;
    if (kept > limit || added > limit - kept) {
      rm_buf_free(&spliced);
      return rm_edit_result(error, RM_ERR_TOO_MANY_TO_NUMBER);
    }
    rm_write_u32_at(&spliced, len_at, (uint32_t)(kept + added));
    rm_buf_append(&spliced, data + winning.replace_begin,
                  size - winning.replace_begin);
  }

  rm_status s_err = rmi_buf_status(&spliced);
  if (s_err != RM_OK) {
    rm_buf_free(&spliced);
    return rm_edit_result(error, s_err);
  }
  size_t out_size = 0;
  uint8_t* bytes = rm_buf_take(&spliced, &out_size);
  if (!bytes) return rm_edit_result(error, RM_ERR_OUT_OF_MEMORY);

  /* Verify the requested LWW value actually took effect on the candidate. */
  bool found = false, actual_visible = true;
  char* actual_name = nullptr;
  rm_status vstatus = rmi_scene_node_state(
      bytes, out_size, node, &found, is_visible ? &actual_visible : nullptr,
      expect_name ? &actual_name : nullptr);
  bool ok = vstatus == RM_OK && found &&
            (!is_visible || actual_visible == expect_visible) &&
            (!expect_name ||
             (actual_name != nullptr && strcmp(actual_name, expect_name) == 0));
  free(actual_name);
  if (!ok) {
    free(bytes);
    return rm_edit_result(
        error, vstatus == RM_ERR_OUT_OF_MEMORY ? vstatus : RM_ERR_UNVERIFIED);
  }

  out->bytes = bytes;
  out->size = out_size;
  out->verified = false;
  return RM_OK;
}

/* The name is stored as given, after any invalid UTF-8 in it is repaired; the
 * check after the write compares against the repaired string. */
rm_status rmi_edit_set_node_name(const uint8_t* data, size_t size,
                                 rm_item_id node, rm_item_id stamp,
                                 const char* name, rmi_edit_candidate* out,
                                 rm_error* error) {
  if (!name) return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  char* repaired = rmi_utf8_repair_alloc(name);
  if (!repaired) return rm_edit_result(error, RM_ERR_OUT_OF_MEMORY);
  rm_buf inner = {0};
  rm_write_id(&inner, 1, rm_crdt_of(stamp));
  rm_subblock_mark sv = rm_buf_begin_subblock(&inner, 2);
  rm_write_string(&inner, repaired);
  rm_buf_end_subblock(&inner, sv);
  rm_status status = set_node(data, size, node, stamp, 2, &inner, false, false,
                              repaired, out, error);
  free(repaired);
  rm_buf_free(&inner);
  return status;
}

rm_status rmi_edit_set_node_visible(const uint8_t* data, size_t size,
                                    rm_item_id node, rm_item_id stamp,
                                    bool visible, rmi_edit_candidate* out,
                                    rm_error* error) {
  rm_buf inner = {0};
  rm_write_id(&inner, 1, rm_crdt_of(stamp));
  rm_write_tag(&inner, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(&inner, visible ? 1u : 0u);
  rm_status status = set_node(data, size, node, stamp, 3, &inner, true, visible,
                              nullptr, out, error);
  rm_buf_free(&inner);
  return status;
}
