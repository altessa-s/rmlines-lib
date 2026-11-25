/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Session lifecycle, undo/redo revision tree, commit verification,
 * and public RM_API editor entrypoints. */

#include <stdlib.h>
#include <string.h>

#include "rm_edit_ops.h"
#include "rm_wire.h"
#include "rmlines/parser.h"

/* One version of the page. Undo and redo walk a single line of revisions:
 * `prev` is the version this one was made from, `next` the version made from
 * this one that redo returns to. A new edit made after an undo replaces
 * everything past `current`, so there is never more than one `next`. */
typedef struct rm_edit_revision {
  uint8_t* bytes;
  size_t size;
  struct rm_edit_revision* prev;
  struct rm_edit_revision* next;
} rm_edit_revision;

/* The session owns every revision in the line, before and after `current`. */
struct rm_edit_session {
  rm_edit_revision* current;
};

static void reset_error(rm_error* error) {
  if (error != nullptr) *error = (rm_error){0};
}

/* Free a revision and every revision redo could reach from it. */
static void free_revisions_forward(rm_edit_revision* rev) {
  while (rev != nullptr) {
    rm_edit_revision* next = rev->next;
    free(rev->bytes);
    free(rev);
    rev = next;
  }
}

/* A successful parse can still report missing content. Only a complete page
 * is suitable for publishing an unverified edit. */
static rm_status verify_bytes_complete(const uint8_t* bytes, size_t size,
                                       rm_status incomplete_status,
                                       rm_error* error) {
  rm_page* page = nullptr;
  rm_status status = rm_parse_page(bytes, size, &page, error);
  if (status != RM_OK) {
    rm_page_free(page);
    return status;
  }
  size_t skipped = page != nullptr ? page->skipped_blocks : 0;
  rm_page_free(page);
  if (skipped > 0) {
    rmi_error_set(error, incomplete_status, 0, (long)skipped,
                  "page has unreadable blocks");
    return incomplete_status;
  }
  return RM_OK;
}

/* Make the candidate the current revision. Takes ownership of its bytes,
 * including on failure: they become the revision's on success and are freed
 * otherwise. A candidate the edit did not verify itself is parsed once more
 * here and refused with RM_ERR_INCOMPLETE_PAGE if any block of it would not
 * read. */
static rm_status commit_candidate(rm_edit_session* session,
                                  rmi_edit_candidate* cand, rm_error* error) {
  if (session == nullptr || session->current == nullptr || cand == nullptr ||
      cand->bytes == nullptr) {
    if (cand != nullptr) {
      free(cand->bytes);
      cand->bytes = nullptr;
    }
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  }

  /* A byte-identical edit is a no-op and must preserve existing redo
   * history. */
  if (cand->size == session->current->size &&
      (cand->size == 0 ||
       memcmp(cand->bytes, session->current->bytes, cand->size) == 0)) {
    free(cand->bytes);
    cand->bytes = nullptr;
    cand->size = 0;
    reset_error(error);
    return RM_OK;
  }
  if (!cand->verified) {
    rm_status status = verify_bytes_complete(cand->bytes, cand->size,
                                             RM_ERR_INCOMPLETE_PAGE, error);
    if (status != RM_OK) {
      free(cand->bytes);
      cand->bytes = nullptr;
      return status;
    }
  }

  rm_edit_revision* rev = calloc(1, sizeof(*rev));
  if (rev == nullptr) {
    free(cand->bytes);
    cand->bytes = nullptr;
    return rm_edit_result(error, RM_ERR_OUT_OF_MEMORY);
  }
  rev->bytes = cand->bytes;
  rev->size = cand->size;
  cand->bytes = nullptr;
  cand->size = 0;

  /* An edit made after an undo starts a new line: the revisions redo could
   * have returned to are dropped for good. */
  free_revisions_forward(session->current->next);
  session->current->next = rev;
  rev->prev = session->current;
  session->current = rev;
  reset_error(error);
  return RM_OK;
}

/* The session owns the bytes once the edit returns. A failed edit can have
 * allocated them before discovering the failure. */
static rm_status finish_edit(rm_edit_session* session, rm_status status,
                             rmi_edit_candidate* cand, rm_error* error) {
  if (status != RM_OK) {
    free(cand->bytes);
    cand->bytes = nullptr;
    return status;
  }
  return commit_candidate(session, cand, error);
}

/* Keep a session-owned copy so the caller can release the input while undo
 * still needs the original page. */
rm_status rm_edit_open(const uint8_t* data, size_t size, rm_edit_session** out,
                       rm_error* error) {
  reset_error(error);
  if (out != nullptr) *out = nullptr;
  if (out == nullptr || data == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);

  /* Every edit rewrites the page whole. A block the parser could not read
   * would either be carried along unchecked or lost, so such a page is not
   * opened at all. */
  RM_TRY(verify_bytes_complete(data, size, RM_ERR_INCOMPLETE_PAGE, error));

  rm_edit_session* session = calloc(1, sizeof(*session));
  rm_edit_revision* rev = calloc(1, sizeof(*rev));
  uint8_t* copy = malloc(size > 0 ? size : 1);
  if (session == nullptr || rev == nullptr || copy == nullptr) {
    free(copy);
    free(rev);
    free(session);
    return rm_edit_result(error, RM_ERR_OUT_OF_MEMORY);
  }
  if (size > 0) memcpy(copy, data, size);
  rev->bytes = copy;
  rev->size = size;
  session->current = rev;
  *out = session;
  return RM_OK;
}

/* Free the whole line of revisions: walk back to the original page first,
 * since `current` may sit anywhere in it after an undo. */
void rm_edit_close(rm_edit_session* session) {
  if (session == nullptr) return;
  rm_edit_revision* head = session->current;
  while (head != nullptr && head->prev != nullptr) head = head->prev;
  free_revisions_forward(head);
  free(session);
}

/* The current revision's bytes, borrowed. The public contract keeps them
 * valid until the next successful edit or the close; undo and redo free
 * nothing, but an edit after an undo frees the revisions past `current`. */
const uint8_t* rm_edit_bytes(const rm_edit_session* session, size_t* size) {
  if (session == nullptr || session->current == nullptr) {
    if (size != nullptr) *size = 0;
    return nullptr;
  }
  if (size != nullptr) *size = session->current->size;
  return session->current->bytes;
}

#ifdef RMLINES_TESTING
/* A writable view of the current revision, so tests can damage the bytes a
 * session holds and check that later edits notice. */
uint8_t* rm_test_edit_bytes_mut(rm_edit_session* session, size_t* size);
uint8_t* rm_test_edit_bytes_mut(rm_edit_session* session, size_t* size) {
  if (session == nullptr || session->current == nullptr) {
    if (size != nullptr) *size = 0;
    return nullptr;
  }
  if (size != nullptr) *size = session->current->size;
  return session->current->bytes;
}
#endif

/* Undo and redo only move along the line of revisions; nothing is freed, so
 * a redo after an undo returns the very same bytes. Both return false when
 * there is nowhere to go. */
bool rm_edit_undo(rm_edit_session* session) {
  if (session == nullptr || session->current == nullptr ||
      session->current->prev == nullptr)
    return false;
  session->current = session->current->prev;
  return true;
}

bool rm_edit_redo(rm_edit_session* session) {
  if (session == nullptr || session->current == nullptr ||
      session->current->next == nullptr)
    return false;
  session->current = session->current->next;
  return true;
}

/* The public edits below all follow one pattern: build a candidate from the
 * current revision's bytes, then hand it to finish_edit, which frees it on
 * failure and commits it on success. The current revision is never changed
 * in place, so a failed edit leaves the session as it was. */
rm_status rm_edit_erase(rm_edit_session* session, const rm_item_id* ids,
                        size_t count, rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_erase_bytes(session->current->bytes, session->current->size, ids,
                           count, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_set_node_name(rm_edit_session* session, rm_item_id node,
                                rm_item_id stamp, const char* name,
                                rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_set_node_name(session->current->bytes, session->current->size,
                             node, stamp, name, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_set_node_visible(rm_edit_session* session, rm_item_id node,
                                   rm_item_id stamp, bool visible,
                                   rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_set_node_visible(session->current->bytes, session->current->size,
                                node, stamp, visible, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_insert_stroke(rm_edit_session* session, rm_item_id parent,
                                rm_item_id id, rm_item_id left,
                                rm_item_id right, const rm_draft_stroke* stroke,
                                rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_insert_stroke(session->current->bytes, session->current->size,
                             parent, id, left, right, stroke, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_replace_text(rm_edit_session* session, size_t start,
                               size_t remove_count, const char* utf8,
                               rm_item_id first_new, rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_replace_text(session->current->bytes, session->current->size,
                            start, remove_count, utf8, first_new, &cand, error);
  return finish_edit(session, status, &cand, error);
}

/* A read-only query: nothing is committed. The array is the caller's, to be
 * released with rm_edit_layers_free. */
rm_status rm_edit_layers(const rm_edit_session* session, rm_edit_layer** out,
                         size_t* count, rm_error* error) {
  reset_error(error);
  if (out != nullptr) *out = nullptr;
  if (count != nullptr) *count = 0;
  if (session == nullptr || session->current == nullptr || out == nullptr ||
      count == nullptr) {
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  }
  return rmi_edit_layers(session->current->bytes, session->current->size, out,
                         count, error);
}

void rm_edit_layers_free(rm_edit_layer* layers) { free(layers); }

rm_status rm_edit_add_layer(rm_edit_session* session, size_t position,
                            const char* name, rm_item_id first,
                            rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_add_layer(session->current->bytes, session->current->size,
                         position, name, first, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_move_layer(rm_edit_session* session, rm_item_id node,
                             size_t position, rm_item_id first,
                             rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_move_layer(session->current->bytes, session->current->size, node,
                          position, first, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_delete_layer(rm_edit_session* session, rm_item_id node,
                               rm_item_id first, rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status =
      rmi_edit_delete_layer(session->current->bytes, session->current->size,
                            node, first, &cand, error);
  return finish_edit(session, status, &cand, error);
}

/* A single stroke is a selection of one; it takes one fresh ID. */
rm_status rm_edit_transform_stroke(rm_edit_session* session, rm_item_id stroke,
                                   rm_item_id fresh,
                                   rm_edit_transform transform,
                                   rm_error* error) {
  return rm_edit_transform_strokes(session, &stroke, 1, fresh, transform,
                                   error);
}

rm_status rm_edit_transform_strokes(rm_edit_session* session,
                                    const rm_item_id* strokes, size_t count,
                                    rm_item_id first_new,
                                    rm_edit_transform transform,
                                    rm_error* error) {
  reset_error(error);
  /* The transform is checked before the empty-selection shortcut, so a
   * singular or non-finite matrix is refused even with nothing to move. */
  if (session == nullptr || session->current == nullptr ||
      !rmi_edit_transform_valid(transform))
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  if (count == 0) return RM_OK;
  rmi_edit_candidate cand = {};
  rm_status status = rmi_edit_transform_strokes(
      session->current->bytes, session->current->size, strokes, count,
      first_new, transform, &cand, error);
  return finish_edit(session, status, &cand, error);
}

rm_status rm_edit_cut_stroke(rm_edit_session* session, rm_item_id stroke,
                             rm_item_id fresh, size_t first_point,
                             size_t point_count, rm_error* error) {
  reset_error(error);
  if (session == nullptr || session->current == nullptr)
    return rm_edit_result(error, RM_ERR_INVALID_ARGUMENT);
  rmi_edit_candidate cand = {};
  rm_status status = rmi_edit_cut_stroke(
      session->current->bytes, session->current->size, stroke, fresh,
      first_point, point_count, &cand, error);
  return finish_edit(session, status, &cand, error);
}
