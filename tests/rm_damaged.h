/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Written pages with one field damaged, shared by the recovery suite, which
 * checks what the loss costs, and the log suite, which checks that it is said
 * out loud. The sink is process-wide, so that half has to live in a suite of
 * its own. */

#ifndef RM_DAMAGED_H
#define RM_DAMAGED_H

#include "rm_internal.h"
#include "rmlines/rmlines.h"

/* The payload of the first block of `type` in a written page, found the way
 * the parser finds it: past the header, envelope by envelope. */
[[maybe_unused]] static bool rm_damaged_payload(const uint8_t* bytes,
                                                size_t size, uint8_t type,
                                                size_t* out_start,
                                                size_t* out_end) {
  size_t offset = RM_HEADER_LENGTH;
  while (offset + 8 <= size) {
    size_t length = (size_t)bytes[offset] | (size_t)bytes[offset + 1] << 8 |
                    (size_t)bytes[offset + 2] << 16 |
                    (size_t)bytes[offset + 3] << 24;
    size_t start = offset + 8;
    if (start + length > size) return false;
    if (bytes[offset + 7] == type) {
      *out_start = start;
      *out_end = start + length;
      return true;
    }
    offset = start + length;
  }
  return false;
}

/* A page with one picture and nothing else, as the writer spells it. */
[[maybe_unused]] static uint8_t* rm_damaged_image_page(size_t* out_size) {
  rm_draft_image image = {.file = "0a418e1a-1111-4222-8333-444455556666.png",
                          .x = -60,
                          .y = 200,
                          .width = 120,
                          .height = 160};
  for (uint8_t i = 0; i < 16; i += 1) image.key[i] = i;
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .images = &image,
      .image_count = 1,
  };
  uint8_t* bytes = nullptr;
  if (rm_write_page(&draft, &bytes, out_size, nullptr) != RM_OK) return nullptr;
  return bytes;
}

/* The written page with the vertex count of its image item damaged. The count
 * is the varuint that opens the tag-3 subblock of the value: the writer puts
 * sixteen floats there — four corners of (x, y, u, v) — and this turns it into
 * twelve, three corners, a placement nothing can be drawn on. */
[[maybe_unused]] static uint8_t* rm_damaged_image_with_three_corners(
    size_t* out_size) {
  uint8_t* bytes = rm_damaged_image_page(out_size);
  size_t start, end;
  if (bytes == nullptr ||
      !rm_damaged_payload(bytes, *out_size, 0x0F, &start, &end)) {
    rm_bytes_free(bytes);
    return nullptr;
  }
  /* 3C: tag 3, length-prefixed; 41 00 00 00: one count byte and sixty-four
   * of floats; 10: sixteen floats. */
  static const uint8_t count[] = {0x3C, 0x41, 0x00, 0x00, 0x00, 0x10};
  for (size_t at = start; at + sizeof count <= end; at += 1) {
    if (memcmp(bytes + at, count, sizeof count) == 0) {
      bytes[at + 5] = 0x0C;
      return bytes;
    }
  }
  rm_bytes_free(bytes);
  return nullptr;
}

/* A page with one loose stroke under its layer. */
[[maybe_unused]] static uint8_t* rm_damaged_stroke_page(size_t* out_size) {
  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(10, 0)};
  rm_draft_stroke stroke = {.tool = RM_TOOL_FINELINER_2,
                            .color = RM_COLOR_BLACK,
                            .thickness = 2,
                            .points = points,
                            .point_count = 2};
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .strokes = &stroke,
      .stroke_count = 1,
  };
  uint8_t* bytes = nullptr;
  if (rm_write_page(&draft, &bytes, out_size, nullptr) != RM_OK) return nullptr;
  return bytes;
}

/* The written page with the value type of its one group item damaged. The
 * group item links the layer into the root; its value is type 02 followed by
 * the node's identifier under tag 2, and this makes it a type no scene item
 * has. */
[[maybe_unused]] static uint8_t* rm_damaged_mistyped_group_item(
    size_t* out_size) {
  uint8_t* bytes = rm_damaged_stroke_page(out_size);
  size_t start, end;
  if (bytes == nullptr ||
      !rm_damaged_payload(bytes, *out_size, 0x04, &start, &end)) {
    rm_bytes_free(bytes);
    return nullptr;
  }
  /* 6C: tag 6, length-prefixed; the four bytes of length are whatever the
   * identifier takes; 02 2F: a group's type, then tag 2 as an identifier. */
  for (size_t at = start; at + 7 <= end; at += 1) {
    if (bytes[at] == 0x6C && bytes[at + 5] == 0x02 && bytes[at + 6] == 0x2F) {
      bytes[at + 5] = 0x09;
      return bytes;
    }
  }
  rm_bytes_free(bytes);
  return nullptr;
}

#endif /* RM_DAMAGED_H */
