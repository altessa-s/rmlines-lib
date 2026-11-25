/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The editor against bytes nobody wrote: malformed pages, truncated trees,
 * overlapping CRDT ranges, and highlighter queries must never crash, read out
 * of bounds, or leak memory under ASan/UBSan. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rmlines/rmlines.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

typedef struct {
  const uint8_t* bytes;
  size_t left;
} commands;

static uint8_t take(commands* input) {
  if (input->left == 0) return 0;

  input->left--;
  return *input->bytes++;
}

static void edit_stroke(rm_edit_session* session, unsigned op, uint8_t target,
                        uint8_t a, uint8_t b, rm_item_id fresh) {
  size_t size;
  const uint8_t* bytes = rm_edit_bytes(session, &size);
  rm_page* page = nullptr;
  if (rm_parse_page(bytes, size, &page, nullptr) != RM_OK) return;

  if (page->stroke_count != 0) {
    const rm_stroke* stroke = &page->strokes[target % page->stroke_count];
    if (op == 1) {
      rm_edit_transform transform = {
          1, 0, 0, 1, (double)a - 128, (double)b - 128};
      (void)rm_edit_transform_stroke(session, stroke->id, fresh, transform,
                                     nullptr);
    } else if (op == 2) {
      (void)rm_edit_cut_stroke(session, stroke->id, fresh, a, b, nullptr);
    } else {
      (void)rm_edit_erase(session, &stroke->id, 1, nullptr);
    }
  }
  rm_page_free(page);
}

static void edit_layer(rm_edit_session* session, unsigned op, uint8_t target,
                       uint8_t arg, const char* name, rm_item_id fresh) {
  rm_edit_layer* layers = nullptr;
  size_t count = 0;
  if (rm_edit_layers(session, &layers, &count, nullptr) != RM_OK) return;

  if (op == 4) {
    (void)rm_edit_add_layer(session, target % (count + 1u), name, fresh,
                            nullptr);
  } else if (count != 0) {
    rm_item_id node = layers[target % count].node;
    switch (op) {
      case 5:
        (void)rm_edit_move_layer(session, node, arg % count, fresh, nullptr);
        break;
      case 6:
        (void)rm_edit_delete_layer(session, node, fresh, nullptr);
        break;
      case 7:
        (void)rm_edit_set_node_visible(session, node, fresh, (arg & 1) != 0,
                                       nullptr);
        break;
      case 8:
        (void)rm_edit_set_node_name(session, node, fresh, name, nullptr);
        break;
    }
  }
  rm_edit_layers_free(layers);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  /* Bytes before the page header are commands. A raw tablet fixture borrows
   * its first few block bytes as commands too, so an unchanged seed exercises
   * edits and inserting a prefix can change them without damaging the page. */
  constexpr size_t max_prefix = 96;
  size_t prefix = 0;
  while (prefix < size && prefix < max_prefix &&
         (size - prefix < RM_HEADER_LENGTH ||
          memcmp(data + prefix, RM_HEADER, RM_HEADER_LENGTH) != 0)) {
    prefix++;
  }

  const uint8_t* bytes = data + prefix;
  size_t page_size = size - prefix;
  commands input = {data, prefix};
  if (prefix == 0 && page_size >= RM_HEADER_LENGTH) {
    size_t available = page_size - RM_HEADER_LENGTH;
    input = (commands){bytes + RM_HEADER_LENGTH,
                       available < max_prefix ? available : max_prefix};
  }

  rm_item_id* ids = nullptr;
  size_t count = 0;
  (void)rm_highlighter_items(bytes, page_size, &ids, &count, nullptr);
  rm_item_ids_free(ids);

  rm_edit_session* session = nullptr;
  if (rm_edit_open(bytes, page_size, &session, nullptr) != RM_OK) return 0;

  for (unsigned i = 0; input.left != 0 && i < 16; i++) {
    unsigned op = take(&input) % 11u;
    uint8_t a = take(&input), b = take(&input), c = take(&input);
    rm_item_id fresh = {1, 10000000u + 32u * i};
    char text[9] = {0};
    if (op == 0 || op == 4 || op == 8) {
      size_t length = c % sizeof text;
      for (size_t j = 0; j < length; j++) text[j] = (char)take(&input);
    }

    if (op == 0) {
      (void)rm_edit_replace_text(session, a, b, text, fresh, nullptr);
    } else if (op <= 3) {
      edit_stroke(session, op, a, b, c, fresh);
    } else if (op <= 8) {
      edit_layer(session, op, a, b, text, fresh);
    } else if (op == 9) {
      (void)rm_edit_undo(session);
    } else {
      (void)rm_edit_redo(session);
    }
  }

  rm_edit_close(session);
  return 0;
}
