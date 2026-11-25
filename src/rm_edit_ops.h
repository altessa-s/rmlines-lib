/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Pure byte-transform editing operations returning immutable candidates
 * that the session publishes. */

#ifndef RM_EDIT_OPS_H
#define RM_EDIT_OPS_H

#include "rmlines/editor.h"

/* Preserve an existing diagnostic when an inner operation already supplied it
 * for this status. */
static inline rm_status rm_edit_result(rm_error* error, rm_status status) {
  if (error != nullptr && error->status != status)
    *error = (rm_error){.status = status};
  return status;
}

/* A candidate owns its bytes. The session can skip a second completeness
 * check when the edit has already verified them. */
typedef struct rmi_edit_candidate {
  uint8_t* bytes;
  size_t size;
  bool verified;
} rmi_edit_candidate;

[[nodiscard]] rm_status rmi_edit_erase_bytes(const uint8_t* data, size_t size,
                                             const rm_item_id* ids,
                                             size_t count,
                                             rmi_edit_candidate* out,
                                             rm_error* error);

/* Name and visibility are separate stamped fields on the node. Changing one
 * must leave the other's bytes in place. */
[[nodiscard]] rm_status rmi_edit_set_node_name(
    const uint8_t* data, size_t size, rm_item_id node, rm_item_id stamp,
    const char* name, rmi_edit_candidate* out, rm_error* error);

[[nodiscard]] rm_status rmi_edit_set_node_visible(
    const uint8_t* data, size_t size, rm_item_id node, rm_item_id stamp,
    bool visible, rmi_edit_candidate* out, rm_error* error);

/* Neighbours name items in the parent's sequence, including positions that no
 * longer draw. */
[[nodiscard]] rm_status rmi_edit_insert_stroke(
    const uint8_t* data, size_t size, rm_item_id parent, rm_item_id id,
    rm_item_id left, rm_item_id right, const rm_draft_stroke* stroke,
    rmi_edit_candidate* out, rm_error* error);

/* Rows past an extension can still belong to the declared list. Preserve
 * their count when an edit rewrites the rows before the extension. */
static inline rm_status rmi_text_row_surplus(size_t declared, size_t decoded,
                                             size_t* surplus) {
  if (decoded > declared) return RM_ERR_UNVERIFIED;

  *surplus = declared - decoded;
  return RM_OK;
}

/* The range is in visible scalars. The replacement also has to account for
 * newline style keys and handwriting anchored to those scalars. */
[[nodiscard]] rm_status rmi_edit_replace_text(const uint8_t* data, size_t size,
                                              size_t start, size_t remove,
                                              const char* utf8, rm_item_id id,
                                              rmi_edit_candidate* out,
                                              rm_error* error);

/* A layer's placement and its ordering link can have different IDs. These
 * operations keep the two in step. */
[[nodiscard]] rm_status rmi_edit_layers(const uint8_t* data, size_t size,
                                        rm_edit_layer** out, size_t* count,
                                        rm_error* error);

[[nodiscard]] rm_status rmi_edit_add_layer(const uint8_t* data, size_t size,
                                           size_t position, const char* name,
                                           rm_item_id first,
                                           rmi_edit_candidate* out,
                                           rm_error* error);

[[nodiscard]] rm_status rmi_edit_move_layer(const uint8_t* data, size_t size,
                                            rm_item_id node, size_t position,
                                            rm_item_id first,
                                            rmi_edit_candidate* out,
                                            rm_error* error);

[[nodiscard]] rm_status rmi_edit_delete_layer(const uint8_t* data, size_t size,
                                              rm_item_id node, rm_item_id first,
                                              rmi_edit_candidate* out,
                                              rm_error* error);

/* Replacing ink leaves its original ID in the sequence as a tombstone and
 * uses fresh IDs for the pieces. */
[[nodiscard]] bool rmi_edit_transform_valid(rm_edit_transform transform);

[[nodiscard]] rm_status rmi_edit_transform_strokes(
    const uint8_t* data, size_t size, const rm_item_id* ids, size_t count,
    rm_item_id fresh, rm_edit_transform transform, rmi_edit_candidate* out,
    rm_error* error);

[[nodiscard]] rm_status rmi_edit_cut_stroke(const uint8_t* data, size_t size,
                                            rm_item_id stroke, rm_item_id fresh,
                                            size_t first, size_t count,
                                            rmi_edit_candidate* out,
                                            rm_error* error);

/* Count comparisons shared by highlights on the same page. */
#ifdef RMLINES_TESTING
void rm_test_reset_glyph_checks(void);
size_t rm_test_glyph_checks(void);
#endif

#endif /* RM_EDIT_OPS_H */
