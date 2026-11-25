/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* Writing fresh .rm v6 pages from rm_page_draft. */

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rm_wire_write.h"
#include "rmlines/parser.h"
#include "rmlines/writer.h"

/* Every identifier a generated page hands out */

/* In one place because the file cannot tell two meanings of one counter
 * apart: reuse a number and the page claims two edits are the same edit. */
constexpr uint8_t DRAFT_AUTHOR = 1;

/* The second author index a tablet page keeps for its formatting edits: its
 * marks all carry it, and its entry stands first in the authors table. */
constexpr uint8_t DRAFT_MARKS_AUTHOR = 2;

/* "Not set" — the start of the text, and the stamp of a field nobody has
 * touched. */
constexpr rm_wid ID_NONE = {.counter = 0, .author = 0};
constexpr rm_wid ID_ROOT = {.counter = 1, .author = 0};
constexpr rm_wid ID_TEXT = {.counter = 1, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_LAYER = {.counter = 11, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_GROUP_ITEM = {.counter = 12, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_CANVAS_STAMP = {.counter = 13, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_COLUMN_STAMP = {.counter = 14, .author = DRAFT_AUTHOR};
constexpr rm_wid ID_WIDTH_STAMP = {.counter = 15, .author = DRAFT_AUTHOR};

/* Where the characters of the text start counting. */
constexpr uint64_t FIRST_CHARACTER = 20;

/* Where the anchored marks start counting. Leave room below them for the
 * text, which spends one counter per scalar and can run to thousands on a
 * page. */
constexpr uint64_t FIRST_MARK = 100000;

/* And where the strokes that hang on nothing start — above the marks, which
 * take eight counters each. */
constexpr uint64_t FIRST_LOOSE_STROKE = 200000;

/* Where the placed images start — above everything else. Each is given
 * sixteen counters' worth of room and spends eight of them. */
constexpr uint64_t FIRST_IMAGE = 300000;
constexpr uint64_t MARK_STRIDE = 8;
constexpr uint64_t IMAGE_STRIDE = 16;

/* How many of each kind fit before one range runs into the next. Derived from
 * the starts above rather than written out beside them, so that moving a
 * start moves its limit with it. Past these the page would name two edits
 * with one counter. */
constexpr size_t MARK_LIMIT =
    (size_t)((FIRST_LOOSE_STROKE - FIRST_MARK) / MARK_STRIDE);
constexpr size_t LOOSE_STROKE_LIMIT =
    (size_t)(FIRST_IMAGE - FIRST_LOOSE_STROKE);

/* The room the characters have before the marks begin — every scalar takes
 * one, and an empty paragraph still takes one for its newline. */
constexpr size_t CHARACTER_LIMIT = (size_t)(FIRST_MARK - FIRST_CHARACTER);

/* Each range ends where the next one begins: a limit that does not match the
 * next start is a collision waiting for the draft that reaches it, or room
 * given away for nothing. */
static_assert(FIRST_MARK + MARK_LIMIT * MARK_STRIDE == FIRST_LOOSE_STROKE,
              "the marks must end where the loose strokes begin");
static_assert(FIRST_LOOSE_STROKE + LOOSE_STROKE_LIMIT == FIRST_IMAGE,
              "the loose strokes must end where the images begin");
static_assert(FIRST_CHARACTER + CHARACTER_LIMIT == FIRST_MARK,
              "the characters must end where the marks begin");

/* The threshold a line anchor carries, as the tablet writes it: 35.75 is how
 * it reads, 35.748031 is what it writes. */
constexpr float LINE_ANCHOR_THRESHOLD = 35.748031f;

/* An identifier out of its two parts, for the counters this file hands out. */
static rm_wid wid(uint8_t author, uint64_t counter) {
  return (rm_wid){.counter = counter, .author = author};
}

/* A stroke of no anchor, by its number on the page. */
static rm_wid loose_stroke_id(size_t number) {
  return wid(DRAFT_AUTHOR, FIRST_LOOSE_STROKE + (uint64_t)number);
}

/* The identifiers of one mark: the group, its place in the drawing order, the
 * stroke, and a stamp for each of the four anchor fields.
 *
 * In one place because two places could count them differently: the chain is
 * built where the marks are looped over and the blocks are written a function
 * away, and a stride that agreed in one and not the other would break the
 * chain into ties — which the format resolves by identifier, so the marks
 * would come back in an order nobody wrote. */
typedef struct mark_ids {
  rm_wid node, item, stroke;

  /* The stamps of the anchor and of the three fields beside it. Each is an
   * identifier of its own, as they are on a page the tablet wrote. */
  rm_wid anchor, type, threshold, origin;
} mark_ids;

/* The identifiers of the mark numbered `number`, its range's base plus the
 * fixed offsets `mark_ids` lists. */
static mark_ids mark_ids_of(size_t number) {
  uint64_t base = FIRST_MARK + (uint64_t)number * MARK_STRIDE;
  return (mark_ids){
      .node = wid(DRAFT_AUTHOR, base),
      .item = wid(DRAFT_AUTHOR, base + 1),
      .stroke = wid(DRAFT_AUTHOR, base + 2),
      .anchor = wid(DRAFT_AUTHOR, base + 3),
      .type = wid(DRAFT_AUTHOR, base + 4),
      .threshold = wid(DRAFT_AUTHOR, base + 5),
      .origin = wid(DRAFT_AUTHOR, base + 6),
  };
}

/* The identifiers of one placed image: the item, and a stamp for each of the
 * two fields written with one — the key on the item, the name in the
 * attachment map. The group an anchored image hangs on and the stamps of
 * three of its anchor fields are spent only when the image is anchored; a
 * loose one takes the first three counters and no more. The fourth field,
 * `anchor_origin_x`, is stamped with the node's own id: that is what a tablet
 * page carries, not a fresh counter. */
typedef struct image_ids {
  rm_wid item, key_stamp, name_stamp;
  rm_wid node, group_item, anchor, type, threshold;
} image_ids;

/* The identifiers of the image numbered `number`, its range's base plus the
 * fixed offsets `image_ids` lists. */
static image_ids image_ids_of(size_t number) {
  uint64_t base = FIRST_IMAGE + (uint64_t)number * IMAGE_STRIDE;
  return (image_ids){
      .item = wid(DRAFT_AUTHOR, base),
      .key_stamp = wid(DRAFT_AUTHOR, base + 1),
      .name_stamp = wid(DRAFT_AUTHOR, base + 2),
      .node = wid(DRAFT_AUTHOR, base + 3),
      .group_item = wid(DRAFT_AUTHOR, base + 4),
      .anchor = wid(DRAFT_AUTHOR, base + 5),
      .type = wid(DRAFT_AUTHOR, base + 6),
      .threshold = wid(DRAFT_AUTHOR, base + 7),
  };
}

rm_draft_column rm_draft_column_of(rm_text_column column) {
  return (rm_draft_column){
      .width = rm_text_column_width(column),
      .setting = rm_text_column_setting(column),
  };
}

rm_draft_point rm_draft_point_make(float x, float y) {
  return (rm_draft_point){
      .x = x, .y = y, .speed = 0, .width = 12, .direction = 0, .pressure = 6};
}

rm_draft_stroke rm_draft_stroke_make(rm_pen_tool tool, rm_pen_color color,
                                     const rm_draft_point* points,
                                     size_t point_count) {
  return (rm_draft_stroke){
      .tool = tool,
      .color = color,
      .has_argb = false,
      .argb = 0,
      .thickness = rm_pen_tool_is_highlighter(tool) ? 1.0 : 2.0,
      .points = points,
      .point_count = point_count};
}

/* How many Unicode scalars `text` holds, nought for a null string. */
static size_t scalar_count(const char* text) {
  if (text == nullptr) return 0;
  return rm_utf8_scalar_count(text, strlen(text));
}

/* The author the formatting marks are written under, when the draft names
 * none.
 *
 * Derived from the page's own author, and derived the same way every time. A
 * fresh UUID here would make the writer answer with different bytes for one
 * and the same draft: a byte-level regression test would be impossible,
 * content-addressed storage would see a change on every rewrite, and two pages
 * of one notebook would name different authors for the same kind of edit.
 *
 * The value decides nothing that is drawn — only that the marks' author index
 * resolves in the page's table — so the transformation has only to be stable
 * and to differ from the author it came from. The last byte is flipped and
 * nothing else: bytes 6 and 8 carry the version and the variant, and leaving
 * them be is what keeps the result a well-formed UUID rather than sixteen
 * bytes that merely look like one. Should the flip land on the nil UUID, the
 * last byte is set to another nonzero value instead. */
static void marks_author_derived_from(const uint8_t author[16],
                                      uint8_t out[16]) {
  memcpy(out, author, 16);
  out[15] ^= 0x01u;
  uint8_t zero[16] = {0};
  if (memcmp(out, zero, 16) == 0) out[15] = 0x02u;
}

/* The name of a node, as the format spells a string: how many bytes, the
 * encoding marker, the bytes. */
static void node_name(rm_buf* out, const char* label) {
  rm_write_id(out, 1, ID_NONE);
  rm_subblock_mark sub = rm_buf_begin_subblock(out, 2);
  rm_write_string(out, label);
  rm_buf_end_subblock(out, sub);
}

/* `0x04` SceneGroupItem: a node's place in the drawing order. The layer sits
 * under the root; an anchored group sits under the layer. */
static void group_item_block(rm_buf* out, rm_wid node, rm_wid item,
                             rm_wid parent, rm_wid previous) {
  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, item);
  rm_write_id(out, 3, previous);
  rm_write_id(out, 4, ID_NONE);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
  rm_subblock_mark value = rm_buf_begin_subblock(out, 6);
  rm_buf_append_byte(out, RM_VALUE_GROUP_ITEM);
  rm_write_id(out, 2, node);
  rm_buf_end_subblock(out, value);
}

/* The identifier a mark or an anchored image hangs on, and how the tablet
 * describes that kind of anchor: a character, or a boundary between lines. */
typedef struct anchor_spec {
  rm_wid id;
  uint8_t type;
  float threshold;
} anchor_spec;

/* The identifier and the type/threshold pair that spell `anchor` the way the
 * tablet does. False when the anchor names a paragraph the text has not got. */
static bool anchor_spec_of(rm_draft_anchor anchor, const rm_wid* characters,
                           size_t count, anchor_spec* out) {
  switch (anchor.kind) {
    case RM_ANCHOR_FIRST_CHARACTER:
      if (anchor.paragraph < 0 || (size_t)anchor.paragraph >= count)
        return false;
      *out = (anchor_spec){
          .id = characters[anchor.paragraph], .type = 1, .threshold = 0};
      return true;
    case RM_ANCHOR_BEFORE_TEXT:
      /* The pen went nowhere near a character, so the anchor is a line
       * boundary — here the one before everything typed. Type and threshold
       * travel as a hard pair. */
      *out = (anchor_spec){.id = wid(0, 0xFFFF'FFFF'FFFEu),
                           .type = 2,
                           .threshold = LINE_ANCHOR_THRESHOLD};
      return true;
    case RM_ANCHOR_AFTER_TEXT:
      *out = (anchor_spec){.id = wid(0, 0xFFFF'FFFF'FFFFu),
                           .type = 2,
                           .threshold = LINE_ANCHOR_THRESHOLD};
      return true;
  }
  return false;
}

/* The stamps of the four anchor fields of one group — each an identifier of
 * its own, as they are on a page the tablet wrote. */
typedef struct anchor_stamps {
  rm_wid anchor, type, threshold, origin;
} anchor_stamps;

/* The group that carries an anchor: its tree record, its node with the four
 * anchor fields, and its place in the drawing order — everything of an
 * anchored mark or image except the item that hangs inside. */
static void anchored_group_blocks(rm_buf* file, rm_wid node, rm_wid item,
                                  anchor_stamps stamps, anchor_spec anchor,
                                  float pen_origin, rm_wid previous) {
  rm_buf block;
  rm_buf_init(&block);

  rm_write_id(&block, 1, node);
  rm_write_id(&block, 2, ID_NONE);
  rm_write_tag(&block, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(&block, 1);
  rm_subblock_mark parent = rm_buf_begin_subblock(&block, 4);
  rm_write_id(&block, 1, ID_LAYER);
  rm_buf_end_subblock(&block, parent);
  rm_write_top_block(file, RM_BLOCK_SCENE_TREE, 1, 1, block.bytes, block.count);
  rmi_buf_propagate(file, &block);

  block.count = 0;
  rm_write_id(&block, 1, node);

  /* An anchored group carries a name and it is empty — the tablet names its
   * layers and not these. */
  rm_subblock_mark name = rm_buf_begin_subblock(&block, 2);
  node_name(&block, "");
  rm_buf_end_subblock(&block, name);
  rm_write_stamped_u8(&block, 3, ID_NONE, 1);

  /* Two identifiers, and the anchor is the second: the first belongs to the
   * node, the second names the character or the line boundary. */
  rm_subblock_mark anchor_field = rm_buf_begin_subblock(&block, 7);
  rm_write_id(&block, 1, stamps.anchor);
  rm_write_id(&block, 2, anchor.id);
  rm_buf_end_subblock(&block, anchor_field);

  /* The type and the threshold go in hard pairs: a character anchor is 1 and
   * 0, a line anchor 2 and 35.75. All three fields are stamped values, like
   * the anchor above them: written bare, the page still parses and the tablet
   * refuses to draw it. */
  rm_write_stamped_u8(&block, 8, stamps.type, anchor.type);
  rm_write_stamped_f32(&block, 9, stamps.threshold, anchor.threshold);

  /* The caller supplies anchor_origin_x. At a character anchor this is the
   * pen's horizontal on the page, which the tablet stores with the group. */
  rm_write_stamped_f32(&block, 10, stamps.origin, pen_origin);
  rm_write_top_block(file, RM_BLOCK_TREE_NODE, 1, 2, block.bytes, block.count);
  rmi_buf_propagate(file, &block);

  block.count = 0;
  group_item_block(&block, node, item, ID_LAYER, previous);
  rm_write_top_block(file, RM_BLOCK_SCENE_GROUP_ITEM, 1, 1, block.bytes,
                     block.count);
  rmi_buf_propagate(file, &block);

  rm_buf_free(&block);
}

/* One anchored stroke: the group that carries the anchor, its place in the
 * tree and in the drawing order, and the stroke itself.
 *
 * Four blocks rather than one because that is how the tablet says it: the
 * anchor lives on a **node**, and a stroke is anchored by having that node
 * for a parent. */
static void anchored_mark_blocks(rm_buf* file, const rm_draft_mark* mark,
                                 size_t number, anchor_spec anchor,
                                 rm_wid previous) {
  mark_ids ids = mark_ids_of(number);

  anchored_group_blocks(file, ids.node, ids.item,
                        (anchor_stamps){.anchor = ids.anchor,
                                        .type = ids.type,
                                        .threshold = ids.threshold,
                                        .origin = ids.origin},
                        anchor, mark->pen_origin, previous);

  /* The stroke hangs on the group, and the group on the text: that parent is
   * the whole of what "anchored" means in the file. The points are the v2
   * layout, and the block header is what says so — both halves of it: a
   * stroke block on a tablet page declares 2 as its minimum too. */
  rm_buf line;
  rm_buf_init(&line);
  rm_write_line_item(&line, &mark->stroke, ids.node, ids.stroke, ID_NONE,
                     ID_NONE);
  rm_write_top_block(file, RM_BLOCK_SCENE_LINE_ITEM, 2, 2, line.bytes,
                     line.count);
  rmi_buf_propagate(file, &line);
  rm_buf_free(&line);
}

/* `0x09` AuthorIds: our UUID against index 1.
 *
 * The bytes go in as the UUID reads, not byte-swapped: a page typed on the
 * tablet carries the same sixteen bytes its `.content` spells out. Swapped,
 * the page and the document it belongs to name two different authors. */
static void authors_block(rm_buf* out, const uint8_t author[16],
                          const uint8_t* marks) {
  rm_write_varuint(out, marks == nullptr ? 1u : 2u);

  /* The marks author's entry stands first, the way a tablet page keeps its
   * table. */
  if (marks != nullptr) {
    rm_subblock_mark entry = rm_buf_begin_subblock(out, 0);
    rm_write_varuint(out, 16);
    rm_buf_append(out, marks, 16);
    rm_write_u16(out, DRAFT_MARKS_AUTHOR);
    rm_buf_end_subblock(out, entry);
  }
  rm_subblock_mark entry = rm_buf_begin_subblock(out, 0);
  rm_write_varuint(out, 16);
  rm_buf_append(out, author, 16);
  rm_write_u16(out, DRAFT_AUTHOR);
  rm_buf_end_subblock(out, entry);
}

/* `0x00` MigrationInfo (also named `Conversion`): the tablet wrote the page.
 * Generated files are meant to be read as the device's own, and this is what
 * a page typed there says: `is_device` set, `migratedLegacy` clear — a native
 * v6 page, not one converted from `.lines`. The flag only steers how two
 * versions of a page merge; it decides nothing about drawing. */
static void migration_block(rm_buf* out) {
  rm_write_id(out, 1, ID_TEXT);
  rm_write_tag(out, 2, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 1);
  rm_write_tag(out, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 0);
}

/* `0x0A` PageInfo: the counters the tablet keeps per page. */
static void page_info_block(rm_buf* out, rm_text_counts stats) {
  rm_write_tag(out, 1, RM_TAG_BYTE4);
  rm_write_u32(out, 1);
  rm_write_tag(out, 2, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
  rm_write_tag(out, 3, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)stats.chars);
  rm_write_tag(out, 4, RM_TAG_BYTE4);
  rm_write_u32(out, (uint32_t)stats.lines);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);
}

/* `0x0D` SceneInfo: the current layer, what is shown under the ink, and the
 * sheet. */
static void scene_info_block(rm_buf* out, const rm_draft_sheet* sheet,
                             const rm_draft_column* column, bool has_images) {
  rm_subblock_mark current_layer = rm_buf_begin_subblock(out, 1);
  rm_write_id(out, 1, ID_NONE);
  rm_write_id(out, 2, ID_LAYER);
  rm_buf_end_subblock(out, current_layer);

  rm_write_stamped_u8(out, 2, ID_NONE, 1);

  /* The imported document's visibility. A notebook has no PDF underneath and
   * the field decides nothing here; the tablet writes it on its typed pages,
   * so ours carry it too. */
  rm_write_stamped_u8(out, 3, ID_NONE, 1);

  /* The sheet, declared the three ways the tablet declares it: whole units
   * under tag 5, the same size unrounded under tag 7, and once more stamped
   * under tag 8 — the one the format calls authoritative. Which sheet it is
   * decides where the text lands. */
  rm_subblock_mark size = rm_buf_begin_subblock(out, 5);
  rm_write_u32(out, sheet->screen_width);
  rm_write_u32(out, sheet->screen_height);
  rm_buf_end_subblock(out, size);

  /* Tag 6 is `extendedContentRect` — an LWW of four float64, all zeros. The
   * tablet may put it on every page; ours leaves it out of typed-only pages,
   * keeping them byte for byte what the tablet accepts. A page that carries
   * an image writes it, because a tablet page with an image does, and a
   * generated page is worth what it has in common with the tablet's own
   * file. */
  if (has_images) {
    rm_subblock_mark viewport = rm_buf_begin_subblock(out, 6);
    rm_write_id(out, 1, ID_NONE);
    static const uint8_t zeroes[32] = {0};
    rm_write_block(out, 2, zeroes, sizeof zeroes);
    rm_buf_end_subblock(out, viewport);
  }

  rm_subblock_mark canvas = rm_buf_begin_subblock(out, 7);
  rm_write_f64(out, sheet->canvas_width);
  rm_write_f64(out, sheet->canvas_height);
  rm_buf_end_subblock(out, canvas);

  rm_subblock_mark stamped_canvas = rm_buf_begin_subblock(out, 8);
  rm_write_id(out, 1, ID_CANVAS_STAMP);
  rm_subblock_mark stamped_inner = rm_buf_begin_subblock(out, 2);
  rm_write_f64(out, sheet->canvas_width);
  rm_write_f64(out, sheet->canvas_height);
  rm_buf_end_subblock(out, stamped_inner);
  rm_buf_end_subblock(out, stamped_canvas);

  /* Tag 9 is the column setting, not a width in page units. It names the
   * column used to place the text box. */
  rm_subblock_mark col = rm_buf_begin_subblock(out, 9);
  rm_write_id(out, 1, ID_COLUMN_STAMP);
  rm_subblock_mark col_val = rm_buf_begin_subblock(out, 2);
  rm_write_varuint(out, column->setting);
  rm_buf_end_subblock(out, col_val);
  rm_buf_end_subblock(out, col);
}

/* `0x01` SceneTree: the layer exists under the root. */
static void scene_tree_block(rm_buf* out) {
  rm_write_id(out, 1, ID_LAYER);
  rm_write_id(out, 2, ID_NONE);
  rm_write_tag(out, 3, RM_TAG_BYTE1);
  rm_buf_append_byte(out, 1);
  rm_subblock_mark parent = rm_buf_begin_subblock(out, 4);
  rm_write_id(out, 1, ID_ROOT);
  rm_buf_end_subblock(out, parent);
}

/* `0x02` SceneTreeNode: the layer's name and visibility. */
static void layer_node_block(rm_buf* out) {
  rm_write_id(out, 1, ID_LAYER);
  rm_subblock_mark name = rm_buf_begin_subblock(out, 2);
  node_name(out, "Layer 1");
  rm_buf_end_subblock(out, name);
  rm_write_stamped_u8(out, 3, ID_NONE, 1);
}

/* `0x0E` AttachmentMap: every attachment of the page, keyed the way the image
 * items name them.
 *
 * As the tablet writes it: version 3/3, one map per page. An entry is the
 * 16-byte key, the stamped file name — extension included — and a second
 * stamped field whose two bytes read `11 00`. That tail is mirrored verbatim:
 * its meaning is unknown, and inventing one would be writing a format the
 * tablet does not write. */
static void attachment_map_block(rm_buf* out, const rm_draft_image* images,
                                 size_t count) {
  rm_subblock_mark list = rm_buf_begin_subblock(out, 1);
  rm_write_varuint(out, (uint64_t)count);
  for (size_t number = 0; number < count; number += 1) {
    rm_subblock_mark entry = rm_buf_begin_subblock(out, 0);
    rm_buf_append(out, images[number].key, 16);

    rm_subblock_mark name = rm_buf_begin_subblock(out, 1);
    rm_write_id(out, 1, image_ids_of(number).name_stamp);
    rm_subblock_mark text = rm_buf_begin_subblock(out, 2);
    char* file = rmi_utf8_repair_alloc(images[number].file);
    if (file == nullptr) {
      rmi_buf_fail(out, RM_BUF_ERR_OOM);
    } else {
      rm_write_string(out, file);
      free(file);
    }
    rm_buf_end_subblock(out, text);
    rm_buf_end_subblock(out, name);

    static const uint8_t tail_value[2] = {0x11, 0x00};
    rm_write_stamped_block(out, 2, ID_NONE, tail_value, sizeof tail_value);
    rm_buf_end_subblock(out, entry);
  }
  rm_buf_end_subblock(out, list);
}

/* `0x0F` SceneImageItem: the placement of one attachment, hung on the layer
 * like a stroke.
 *
 * As the tablet writes it: value type 7, the key stamped under tag 1, four
 * vertices under tag 3 — the quad's corners with their UV, plain noughts and
 * ones for an uncropped image — and under tag 4 the quad as two triangles,
 * the same six indices on every item. The two identifiers at tag 2 and tag 5
 * are the insert flow's lineage — a moved or resized image points them at its
 * earlier incarnations — and a fresh page has none to point at. */
static void image_item(rm_buf* out, const rm_draft_image* image, image_ids ids,
                       rm_wid parent, rm_wid previous) {
  rm_write_id(out, 1, parent);
  rm_write_id(out, 2, ids.item);
  rm_write_id(out, 3, previous);
  rm_write_id(out, 4, ID_NONE);
  rm_write_tag(out, 5, RM_TAG_BYTE4);
  rm_write_u32(out, 0);

  rm_subblock_mark value = rm_buf_begin_subblock(out, 6);
  rm_buf_append_byte(out, RM_VALUE_IMAGE_ITEM);
  rm_subblock_mark keyed = rm_buf_begin_subblock(out, 1);
  rm_write_id(out, 1, ids.key_stamp);
  rm_write_block(out, 2, image->key, 16);
  rm_buf_end_subblock(out, keyed);
  rm_write_id(out, 2, ID_NONE);

  rm_subblock_mark vertices = rm_buf_begin_subblock(out, 3);
  rm_write_varuint(out, 16);
  const rm_image_vertex corners[4] = {
      {.x = image->x, .y = image->y, .u = 0, .v = 0},
      {.x = (double)image->x + (double)image->width,
       .y = image->y,
       .u = 1,
       .v = 0},
      {.x = (double)image->x + (double)image->width,
       .y = (double)image->y + (double)image->height,
       .u = 1,
       .v = 1},
      {.x = image->x,
       .y = (double)image->y + (double)image->height,
       .u = 0,
       .v = 1},
  };

  for (size_t i = 0; i < 4; i += 1) {
    rm_write_f32(out, (float)corners[i].x);
    rm_write_f32(out, (float)corners[i].y);
    rm_write_f32(out, (float)corners[i].u);
    rm_write_f32(out, (float)corners[i].v);
  }
  rm_buf_end_subblock(out, vertices);

  rm_subblock_mark indices = rm_buf_begin_subblock(out, 4);
  rm_write_varuint(out, 6);
  static constexpr uint32_t quad[6] = {0, 1, 2, 2, 3, 0};
  for (size_t i = 0; i < 6; i += 1) rm_write_u32(out, quad[i]);
  rm_buf_end_subblock(out, indices);
  rm_buf_end_subblock(out, value);
}

/* ------------------------------------------------------------------ */
/* `0x07` RootText                                                     */
/* ------------------------------------------------------------------ */

/* The paragraphs, the styles keyed to them, and the box they are set in.
 *
 * Hands back two things only this walk knows: how many characters there are,
 * which `0x0A` has to declare, and the identifier of the first character of
 * each paragraph, which is what an anchored stroke names. */
typedef struct text_writer {
  rm_buf* items;
  size_t records;
  uint64_t counter;

  /* Two different tails: `chain_tail` is the last text record of the CRDT
   * chain (what the next record's `left` names; face marks stand beside the
   * chain and do not move it); `last_newline` is the newline character that
   * keys the next paragraph's style record. */
  rm_wid chain_tail;
  rm_wid last_newline;
  rm_text_counts stats;
  size_t total_scalars;
  uint64_t mark_counter;
} text_writer;

/* One text record: `raw` repaired to well-formed UTF-8, chained onto the
 * record before it, and the chain tail and counters advanced past it. */
static void append_record(text_writer* state, const char* raw) {
  char* text = rmi_utf8_repair_alloc(raw);
  if (text == nullptr) {
    rmi_buf_fail(state->items, RM_BUF_ERR_OOM);
    return;
  }
  rm_subblock_mark row = rm_buf_begin_subblock(state->items, 0);
  rm_write_id(state->items, 2, wid(DRAFT_AUTHOR, state->counter));
  rm_write_id(state->items, 3, state->chain_tail);
  rm_write_id(state->items, 4, ID_NONE);
  rm_write_tag(state->items, 5, RM_TAG_BYTE4);
  rm_write_u32(state->items, 0);
  rm_subblock_mark value = rm_buf_begin_subblock(state->items, 6);
  rm_write_string(state->items, text);
  rm_buf_end_subblock(state->items, value);
  rm_buf_end_subblock(state->items, row);

  state->records += 1;

  /* The characters of a record continue its own identifier, and a record of
   * no characters still holds one place in the chain. */
  size_t scalars = scalar_count(text);
  uint64_t span = scalars < 1 ? 1u : (uint64_t)scalars;
  state->chain_tail = wid(DRAFT_AUTHOR, state->counter + span - 1u);
  state->counter += span;
  state->total_scalars += scalars;
  rm_text_counts_add(&state->stats, text, strlen(text));
  free(text);
}

/* A face mark, the way the tablet spells one: a second author's item standing
 * beside the text chain — its `left` names a text scalar, its `right` the
 * neighbour it precedes, and it consumes no scalar of the text counter. A
 * mark woven into the chain itself is a shape the tablet declines to draw. */
static rm_wid append_mark(text_writer* state, uint32_t mark, rm_wid left,
                          rm_wid right) {
  rm_wid id = wid(DRAFT_MARKS_AUTHOR, state->mark_counter);
  state->mark_counter += 1;

  rm_subblock_mark row = rm_buf_begin_subblock(state->items, 0);
  rm_write_id(state->items, 2, id);
  rm_write_id(state->items, 3, left);
  rm_write_id(state->items, 4, right);
  rm_write_tag(state->items, 5, RM_TAG_BYTE4);
  rm_write_u32(state->items, 0);
  rm_subblock_mark value = rm_buf_begin_subblock(state->items, 6);

  /* No text at all: what makes this a mark rather than a record is the field
   * behind the empty string. */
  rm_write_string(state->items, "");
  rm_write_tag(state->items, 2, RM_TAG_BYTE4);
  rm_write_u32(state->items, mark);
  rm_buf_end_subblock(state->items, value);
  rm_buf_end_subblock(state->items, row);

  state->records += 1;
  return id;
}

typedef struct text_block_result {
  rm_buf bytes;
  rm_text_counts stats;

  /* The identifier of each paragraph's first character; one entry per
   * paragraph, malloc'd, the caller frees. */
  rm_wid* first_characters;
} text_block_result;

/* Build the RootText payload and remember where each paragraph's first
 * character was numbered, so anchored handwriting can refer to it. Return the
 * write status before handing the result to the caller. */
static rm_status text_block(const rm_draft_paragraph* paragraphs, size_t count,
                            const rm_draft_sheet* sheet,
                            const rm_draft_column* column,
                            text_block_result* out) {
  size_t total_records = 0;
  size_t style_count = 0;
  for (size_t index = 0; index < count; index += 1) {
    const rm_draft_paragraph* paragraph = &paragraphs[index];
    if (paragraph->has_style) style_count += 1;
    total_records += 1;
    if (paragraph->text != nullptr && paragraph->text[0] != '\0') {
      if (paragraph->italic) total_records += 2;
      if (paragraph->bold) total_records += 2;
      if (index + 1 < count) total_records += 1;
    }
  }

  rm_buf_init(&out->bytes);
  text_writer state = {
      .items = &out->bytes,
      .records = 0,
      .counter = FIRST_CHARACTER,
      .chain_tail = ID_NONE,
      .last_newline = ID_NONE,
      .stats = {count != 0, count != 0},
      .total_scalars = 0,
      .mark_counter = 1000000,
  };

  rm_wid* first_characters =
      calloc(count == 0 ? 1 : count, sizeof *first_characters);

  /* The style records, in the order the paragraphs carry them. */
  size_t* styled = calloc(count == 0 ? 1 : count, sizeof *styled);
  rm_wid* style_keys = calloc(count == 0 ? 1 : count, sizeof *style_keys);
  if (first_characters == nullptr || styled == nullptr ||
      style_keys == nullptr) {
    free(first_characters);
    free(styled);
    free(style_keys);
    return RM_ERR_OUT_OF_MEMORY;
  }
  size_t style_i = 0;

  rm_write_id(&out->bytes, 1, ID_TEXT);

  /* Both lists share the one subblock — the only place in the format where
   * that happens. */
  rm_subblock_mark both = rm_buf_begin_subblock(&out->bytes, 2);
  rm_subblock_mark item_container = rm_buf_begin_subblock(&out->bytes, 1);
  rm_subblock_mark item_list = rm_buf_begin_subblock(&out->bytes, 1);
  rm_write_varuint(&out->bytes, (uint64_t)total_records);

  for (size_t index = 0; index < count; index += 1) {
    const rm_draft_paragraph* paragraph = &paragraphs[index];

    /* The style is keyed by the newline that ends the paragraph before; the
     * first paragraph is keyed by the start sentinel. */
    rm_wid key = index == 0 ? ID_NONE : state.last_newline;
    if (paragraph->has_style) {
      style_keys[style_i] = key;
      styled[style_i] = index;
      style_i += 1;
    }

    bool last = index + 1 == count;
    first_characters[index] = wid(DRAFT_AUTHOR, state.counter);

    if (paragraph->text == nullptr || paragraph->text[0] == '\0') {
      /* An empty paragraph is its newline and nothing else — one record, so
       * the scalar counters the anchors key on do not move. */
      append_record(&state, last ? "" : "\n");
      state.last_newline = state.chain_tail;
      continue;
    }

    /* Opening marks stand beside the chain, pointing right at the text item —
     * italic first, bold chained onto it, the tablet's order. */
    rm_wid text_id = wid(DRAFT_AUTHOR, state.counter);
    rm_wid opener = state.chain_tail;
    if (paragraph->italic)
      opener = append_mark(&state, 3, state.chain_tail, text_id);
    if (paragraph->bold) (void)append_mark(&state, 1, opener, text_id);

    append_record(&state, paragraph->text);
    rm_wid text_tail = state.chain_tail;
    rm_wid newline_id = last ? ID_NONE : wid(DRAFT_AUTHOR, state.counter);

    /* Closing marks hang off the last letter and point at the newline — bold
     * first, italic after, the nesting the tablet closes with. */
    if (paragraph->bold && paragraph->italic) {
      rm_wid italic_off = wid(DRAFT_MARKS_AUTHOR, state.mark_counter + 1u);
      (void)append_mark(&state, 2, text_tail, italic_off);
      (void)append_mark(&state, 4, text_tail, newline_id);
    } else if (paragraph->bold) {
      (void)append_mark(&state, 2, text_tail, newline_id);
    } else if (paragraph->italic) {
      (void)append_mark(&state, 4, text_tail, newline_id);
    }
    if (!last) {
      append_record(&state, "\n");
      state.last_newline = state.chain_tail;
    }
  }
  rm_buf_end_subblock(&out->bytes, item_list);
  rm_buf_end_subblock(&out->bytes, item_container);

  rm_subblock_mark style_list = rm_buf_begin_subblock(&out->bytes, 2);
  rm_subblock_mark style_rows = rm_buf_begin_subblock(&out->bytes, 1);
  rm_write_varuint(&out->bytes, (uint64_t)style_count);
  for (size_t i = 0; i < style_count; i += 1) {
    const rm_draft_paragraph* paragraph = &paragraphs[styled[i]];
    rm_write_bare_id(&out->bytes, style_keys[i]);
    rm_write_id(&out->bytes, 1, ID_NONE);
    rm_subblock_mark value = rm_buf_begin_subblock(&out->bytes, 2);
    rm_write_tag(&out->bytes, 1, RM_TAG_BYTE1);
    rm_buf_append_byte(&out->bytes, paragraph->style);
    if (paragraph->has_second) {
      rm_write_tag(&out->bytes, 2, RM_TAG_BYTE1);
      rm_buf_append_byte(&out->bytes, paragraph->second);
    }
    if (paragraph->has_third) {
      rm_write_tag(&out->bytes, 3, RM_TAG_BYTE4);
      rm_write_u32(&out->bytes, paragraph->third);
    }
    rm_buf_end_subblock(&out->bytes, value);
  }
  rm_buf_end_subblock(&out->bytes, style_rows);
  rm_buf_end_subblock(&out->bytes, style_list);
  rm_buf_end_subblock(&out->bytes, both);

  float resolved = rmi_resolved_column_width(
      column->setting, sheet->screen_width, sheet->screen_height);
  float effective = (column->width > 0.0f) ? column->width : resolved;
  bool use_resolved_for_tag4 = (column->setting == 4u || column->width <= 0.0f);
  float tag4_width = use_resolved_for_tag4 ? resolved : column->width;
  float tag5_width = (column->width == 0.0f) ? 0.0f : column->width;

  /* The box: the column is centred on the sheet, so it starts half its own
   * width to the left of the middle. */
  rm_subblock_mark position = rm_buf_begin_subblock(&out->bytes, 3);
  rm_write_f64(&out->bytes, -((double)effective) / 2.0);
  rm_write_f64(&out->bytes, 234.0);
  rm_buf_end_subblock(&out->bytes, position);
  rm_write_tag(&out->bytes, 4, RM_TAG_BYTE4);
  rm_write_f32(&out->bytes, tag4_width);

  /* Tag 5 repeats the column width as a stamped setting. A positive setting
   * takes precedence over tag 4. For automatic width, write zero here and the
   * resolved width in tag 4. */
  rmi_write_text_settings_width(&out->bytes, ID_WIDTH_STAMP, tag5_width);

  rm_status status = rmi_buf_status(&out->bytes);
  free(styled);
  free(style_keys);

  if (status != RM_OK) {
    rm_buf_free(&out->bytes);
    free(first_characters);
    return status;
  }
  out->stats = state.stats;
  out->first_characters = first_characters;
  return RM_OK;
}

/* ------------------------------------------------------------------ */
/* Validation & door                                                   */
/* ------------------------------------------------------------------ */

/* An array the draft declares a length for has to be there. A count without
 * its items is a caller's slip and not a page — and read on trust it is a
 * null dereference inside the writer rather than a status at its door.
 *
 * The pointer may be null when the count is nought: that is how a draft says
 * it has no strokes, and every loop over it is skipped. */
static rm_status check_arrays_are_there(const rm_page_draft* draft,
                                        rm_error* out_error) {
  struct {
    const void* items;
    size_t count;
    const char* what;
  } declared[] = {
      {draft->paragraphs, draft->paragraph_count, "paragraphs"},
      {draft->strokes, draft->stroke_count, "strokes"},
      {draft->marks, draft->mark_count, "marks"},
      {draft->images, draft->image_count, "images"},
  };
  for (size_t i = 0; i < sizeof declared / sizeof *declared; i += 1) {
    if (declared[i].count != 0 && declared[i].items == nullptr) {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0,
                    (long)declared[i].count, declared[i].what);
      return RM_ERR_INVALID_ARGUMENT;
    }
  }
  return RM_OK;
}

/* Only a nonempty formatted paragraph emits face markers and needs their
 * author in the page's table. */
static bool draft_emits_face_marks(const rm_page_draft* draft) {
  for (size_t i = 0; i < draft->paragraph_count; i += 1) {
    const rm_draft_paragraph* p = &draft->paragraphs[i];
    if ((p->bold || p->italic) && p->text != nullptr && p->text[0] != '\0')
      return true;
  }
  return false;
}

/* The same check as `check_arrays_are_there`, one level down: a stroke or a
 * mark's stroke whose point count is not nought has to carry the points. */
static rm_status validate_stroke_arrays_and_wire(
    const rm_draft_stroke* stroke, size_t index, const char* array_name,
    rm_wid parent, rm_wid id, rm_wid left, rm_wid right, rm_error* out_error) {
  if (stroke->point_count != 0 && stroke->points == nullptr) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                  array_name);
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (!rmi_line_item_wire_fits(stroke, parent, id, left, right)) {
    rmi_error_set(out_error, RM_ERR_TOO_MANY_TO_NUMBER, 0, (long)index,
                  array_name);
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  return RM_OK;
}

static rm_status validate_stroke(const rm_draft_stroke* stroke, size_t index,
                                 const char* array_name, rm_error* out_error) {
  if (stroke->point_count != 0 && stroke->points == nullptr) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                  array_name);
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (!rmi_pen_tool_valid(stroke->tool)) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                  "stroke.tool");
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (!rmi_stroke_color_valid(stroke->tool, stroke->color, stroke->has_argb)) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                  "stroke.color");
    return RM_ERR_INVALID_ARGUMENT;
  }

  if (!isfinite(stroke->thickness) ||
      !rmi_pen_thickness_valid(stroke->tool, stroke->thickness)) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                  "stroke.thickness");
    return RM_ERR_INVALID_ARGUMENT;
  }
  for (size_t p = 0; p < stroke->point_count; p += 1) {
    if (!isfinite(stroke->points[p].x) || !isfinite(stroke->points[p].y)) {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)index,
                    array_name);
      return RM_ERR_INVALID_ARGUMENT;
    }
  }
  return RM_OK;
}

/* Which paragraph an anchor names, for the error that says it could not be
 * found. The two sentinels name none, and never fail to resolve. */
static long paragraph_index_of(rm_draft_anchor anchor) {
  return anchor.kind == RM_ANCHOR_FIRST_CHARACTER ? (long)anchor.paragraph
                                                  : -1L;
}

/* Reject a draft whose values would be lost when written and read back,
 * including ambiguous attachment keys. */
static rm_status check_draft_geometry_and_text(const rm_page_draft* draft,
                                               rm_error* out_error) {
  if (rmi_uuid_is_nil(draft->author)) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, "author");
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (draft_emits_face_marks(draft) && draft->has_marks_author &&
      (memcmp(draft->marks_author, draft->author, 16) == 0 ||
       rmi_uuid_is_nil(draft->marks_author))) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, "marks_author");
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (!isfinite(draft->sheet.canvas_width) || draft->sheet.canvas_width < 0.0 ||
      !isfinite(draft->sheet.canvas_height) ||
      draft->sheet.canvas_height < 0.0) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, "sheet");
    return RM_ERR_INVALID_ARGUMENT;
  }

  if (!isfinite(draft->column.width) || draft->column.width < 0.0f) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, "column.width");
    return RM_ERR_INVALID_ARGUMENT;
  }
  if (draft->column.setting > 2u && draft->column.setting != 4u) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0,
                  (long)draft->column.setting, "column.setting");
    return RM_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < draft->paragraph_count; i += 1) {
    const char* text = draft->paragraphs[i].text;
    if (text != nullptr && strchr(text, '\n') != nullptr) {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)i,
                    "paragraphs.text");
      return RM_ERR_INVALID_ARGUMENT;
    }
  }

  for (size_t i = 0; i < draft->stroke_count; i += 1) {
    RM_TRY(validate_stroke(&draft->strokes[i], i, "strokes.points", out_error));
  }
  for (size_t i = 0; i < draft->mark_count; i += 1) {
    if (!isfinite(draft->marks[i].pen_origin)) {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)i,
                    "marks.pen_origin");
      return RM_ERR_INVALID_ARGUMENT;
    }
    RM_TRY(validate_stroke(&draft->marks[i].stroke, i, "marks.stroke.points",
                           out_error));
    if ((draft->marks[i].anchor.kind == RM_ANCHOR_FIRST_CHARACTER &&
         (draft->marks[i].anchor.paragraph < 0 ||
          (size_t)draft->marks[i].anchor.paragraph >=
              draft->paragraph_count)) ||
        (draft->marks[i].anchor.kind != RM_ANCHOR_FIRST_CHARACTER &&
         draft->marks[i].anchor.kind != RM_ANCHOR_BEFORE_TEXT &&
         draft->marks[i].anchor.kind != RM_ANCHOR_AFTER_TEXT)) {
      rmi_error_set(out_error, RM_ERR_ANCHOR_OUT_OF_RANGE, 0,
                    paragraph_index_of(draft->marks[i].anchor), "a mark");
      return RM_ERR_ANCHOR_OUT_OF_RANGE;
    }
  }

  rm_keymap seen_keys;
  rm_keymap_init(&seen_keys);
  rm_status img_status = RM_OK;
  for (size_t i = 0; i < draft->image_count; i += 1) {
    const rm_draft_image* im = &draft->images[i];
    if (im->file == nullptr || im->file[0] == '\0') {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)i,
                    "images.file");
      img_status = RM_ERR_INVALID_ARGUMENT;
      break;
    }
    double max_x = (double)im->x + (double)im->width;

    double max_y = (double)im->y + (double)im->height;
    if (!isfinite(im->x) || !isfinite(im->y) || !isfinite(im->width) ||
        !isfinite(im->height) || !isfinite(max_x) || !isfinite(max_y) ||
        fabs(max_x) > (double)FLT_MAX || fabs(max_y) > (double)FLT_MAX ||
        (im->has_anchor && !isfinite(im->pen_origin))) {
      rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)i, "images");
      img_status = RM_ERR_INVALID_ARGUMENT;
      break;
    }
    if (im->has_anchor &&
        ((im->anchor.kind == RM_ANCHOR_FIRST_CHARACTER &&
          (im->anchor.paragraph < 0 ||
           (size_t)im->anchor.paragraph >= draft->paragraph_count)) ||
         (im->anchor.kind != RM_ANCHOR_FIRST_CHARACTER &&
          im->anchor.kind != RM_ANCHOR_BEFORE_TEXT &&
          im->anchor.kind != RM_ANCHOR_AFTER_TEXT))) {
      rmi_error_set(out_error, RM_ERR_ANCHOR_OUT_OF_RANGE, 0,
                    paragraph_index_of(im->anchor), "an image");
      img_status = RM_ERR_ANCHOR_OUT_OF_RANGE;
      break;
    }

    uint64_t prev_index;
    if (rm_keymap_get(&seen_keys, im->key, &prev_index)) {
      char* a = rmi_utf8_repair_alloc(draft->images[prev_index].file);
      char* b = rmi_utf8_repair_alloc(im->file);
      bool alloc_failed = (a == nullptr || b == nullptr);
      bool same = (!alloc_failed && strcmp(a, b) == 0);
      free(a);
      free(b);
      if (alloc_failed) {
        rmi_error_set(out_error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
        img_status = RM_ERR_OUT_OF_MEMORY;
        break;
      }

      if (!same) {
        rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, (long)i,
                      "images.key");
        img_status = RM_ERR_INVALID_ARGUMENT;
        break;
      }
    } else if (!rm_keymap_set(&seen_keys, im->key, i)) {
      rmi_error_set(out_error, RM_ERR_OUT_OF_MEMORY, 0, 0, nullptr);
      img_status = RM_ERR_OUT_OF_MEMORY;
      break;
    }
  }
  rm_keymap_free(&seen_keys);
  return img_status;
}

/* Marks and loose strokes have fixed ranges. Stop before their counters reach
 * the next range. Images come last and have no following range to run into. */
static rm_status check_item_counts_fit(const rm_page_draft* draft,
                                       rm_error* out_error) {
  if (draft->mark_count > MARK_LIMIT) {
    rmi_error_set(out_error, RM_ERR_TOO_MANY_TO_NUMBER, 0, (long)MARK_LIMIT,
                  "marks");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  if (draft->stroke_count > LOOSE_STROKE_LIMIT) {
    rmi_error_set(out_error, RM_ERR_TOO_MANY_TO_NUMBER, 0,
                  (long)LOOSE_STROKE_LIMIT, "strokes");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  return RM_OK;
}

/* The text and paragraph boundaries draw on the same range of counters as the
 * characters. Count repaired Unicode scalars before reserving the space for
 * paragraphs. */
static rm_status check_characters_fit(const rm_page_draft* draft,
                                      size_t scalars, rm_error* out_error) {
  /* Allow one counter per paragraph as well as those used by its text. This
   * covers a separator or the empty record that still occupies a place in the
   * chain. */
  if (scalars > CHARACTER_LIMIT ||
      draft->paragraph_count > CHARACTER_LIMIT - scalars) {
    rmi_error_set(out_error, RM_ERR_TOO_MANY_TO_NUMBER, 0,
                  (long)CHARACTER_LIMIT, "characters");
    return RM_ERR_TOO_MANY_TO_NUMBER;
  }
  return RM_OK;
}

/* The whole `.rm` file of one page: the service four, the text, and a
 * one-layer scene tree — the inventory of a page the tablet itself writes for
 * a typed-only notebook.
 *
 * The service four go out first and in the tablet's order; `0x07` stands
 * before the tree — a placement of our own. Block order past the four is the
 * writer's property, and the tablet accepts it as written. */
rm_status rm_write_page(const rm_page_draft* draft, uint8_t** out_bytes,
                        size_t* out_size, rm_error* out_error) {
  if (out_error != nullptr) *out_error = (rm_error){.status = RM_OK};
  if (draft == nullptr || out_bytes == nullptr || out_size == nullptr) {
    rmi_error_set(out_error, RM_ERR_INVALID_ARGUMENT, 0, 0, nullptr);
    return RM_ERR_INVALID_ARGUMENT;
  }

  /* Everything the draft can be refused for on its own terms, before a single
   * byte is written — in this order: the characters, then the marks and the
   * strokes. Arrays a count promises and does not hand over come first of
   * all; the text cannot be laid out without them. */
  rm_status status = check_arrays_are_there(draft, out_error);
  if (status != RM_OK) return status;

  /* Step 2: character consumption preflight. */
  size_t total_scalars = 0;
  for (size_t i = 0; i < draft->paragraph_count; i += 1) {
    const char* t = draft->paragraphs[i].text;
    if (t == nullptr || t[0] == '\0') continue;
    size_t n = rmi_utf8_repaired_scalar_count(t, strlen(t));
    if (n > SIZE_MAX - total_scalars) {
      total_scalars = SIZE_MAX;
      break;
    }
    total_scalars += n;
  }
  status = check_characters_fit(draft, total_scalars, out_error);
  if (status != RM_OK) return status;

  /* Step 3: mark and stroke item counts. */
  status = check_item_counts_fit(draft, out_error);
  if (status != RM_OK) return status;

  /* Step 4: nested point pointers & wire-size preflight. */
  rm_wid previous = ID_NONE;
  for (size_t i = 0; i < draft->stroke_count; i += 1) {
    rm_wid cur = loose_stroke_id(i);
    status = validate_stroke_arrays_and_wire(&draft->strokes[i], i,
                                             "strokes.points", ID_LAYER, cur,
                                             previous, ID_NONE, out_error);
    if (status != RM_OK) return status;
    previous = cur;
  }
  for (size_t i = 0; i < draft->mark_count; i += 1) {
    mark_ids ids = mark_ids_of(i);
    status = validate_stroke_arrays_and_wire(
        &draft->marks[i].stroke, i, "marks.stroke.points", ids.node, ids.stroke,
        ID_NONE, ID_NONE, out_error);
    if (status != RM_OK) return status;
  }

  /* Step 5: geometry, text, anchor, author, and attachment validation. */
  status = check_draft_geometry_and_text(draft, out_error);
  if (status != RM_OK) return status;

  /* Step 6: build RootText block. */
  text_block_result text = {0};
  status = text_block(draft->paragraphs, draft->paragraph_count, &draft->sheet,
                      &draft->column, &text);
  if (status != RM_OK) {
    rmi_error_set(out_error, status, 0,
                  status == RM_ERR_TOO_MANY_TO_NUMBER
                      ? rmi_error_size(rmi_block_limit())
                      : 0,
                  nullptr);
    return status;
  }

  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);

  /* A page whose text carries faces names a second author: on a tablet page
   * the formatting marks are another session's edits, and their author index
   * has to resolve in the table. */
  bool faces_present = draft_emits_face_marks(draft);
  uint8_t marks_author[16];
  const uint8_t* marks = nullptr;
  if (faces_present) {
    if (draft->has_marks_author)
      memcpy(marks_author, draft->marks_author, 16);
    else
      marks_author_derived_from(draft->author, marks_author);
    marks = marks_author;
  }

  rm_buf block;
  rm_buf_init(&block);

#define EMIT(type, min_version, current_version, build)                 \
  do {                                                                  \
    block.count = 0;                                                    \
    build;                                                              \
    rm_write_top_block(&file, (type), (min_version), (current_version), \
                       block.bytes, block.count);                       \
    rmi_buf_propagate(&file, &block);                                   \
  } while (0)

  EMIT(RM_BLOCK_AUTHOR_IDS, 1, 1, authors_block(&block, draft->author, marks));
  EMIT(RM_BLOCK_MIGRATION_INFO, 1, 1, migration_block(&block));
  EMIT(RM_BLOCK_PAGE_INFO, 0, 1, page_info_block(&block, text.stats));
  EMIT(RM_BLOCK_SCENE_INFO, 0, 1,
       scene_info_block(&block, &draft->sheet, &draft->column,
                        draft->image_count != 0));

  /* The attachment map stands between SceneInfo and the scene tree — where a
   * tablet page with an image keeps it. One map per page, all attachments in
   * it. */
  if (draft->image_count != 0) {
    EMIT(RM_BLOCK_ATTACHMENT_MAP, 3, 3,
         attachment_map_block(&block, draft->images, draft->image_count));
  }

  /* A page with no typed text carries no RootText block at all — a tablet
   * image-only page does not, and a generated page is worth what it has in
   * common with the tablet's own file. Marks and anchored images presuppose
   * text: they anchor to it. */
  bool anchored_image = false;
  for (size_t i = 0; i < draft->image_count; i += 1) {
    if (draft->images[i].has_anchor) anchored_image = true;
  }
  if (draft->paragraph_count != 0 || draft->mark_count != 0 || anchored_image) {
    rm_write_top_block(&file, RM_BLOCK_ROOT_TEXT, 1, 1, text.bytes.bytes,
                       text.bytes.count);
  }

  EMIT(RM_BLOCK_SCENE_TREE, 1, 1, scene_tree_block(&block));
  EMIT(RM_BLOCK_TREE_NODE, 1, 2, layer_node_block(&block));
  EMIT(RM_BLOCK_SCENE_GROUP_ITEM, 1, 1,
       group_item_block(&block, ID_LAYER, ID_GROUP_ITEM, ID_ROOT, ID_NONE));

  /* The marks and the loose strokes are one chain, not a heap: items sharing
   * a `left` are a tie, and the format breaks it in favour of the newer one —
   * so anything left unchained comes back in the reverse of the order it was
   * written in. They share the chain because they share a parent: both hang
   * on the layer. */
  previous = ID_NONE;
  for (size_t number = 0; number < draft->stroke_count; number += 1) {
    block.count = 0;
    rm_write_line_item(&block, &draft->strokes[number], ID_LAYER,
                       loose_stroke_id(number), previous, ID_NONE);
    rm_write_top_block(&file, RM_BLOCK_SCENE_LINE_ITEM, 2, 2, block.bytes,
                       block.count);
    rmi_buf_propagate(&file, &block);
    previous = loose_stroke_id(number);
  }

  for (size_t number = 0; number < draft->mark_count; number += 1) {
    const rm_draft_mark* mark = &draft->marks[number];
    anchor_spec anchor;

    /* A refusal, not a skip: a mark whose paragraph is not on the page would
     * vanish without a trace, and silently losing ink is the one thing a
     * writer must never do. */
    if (!anchor_spec_of(mark->anchor, text.first_characters,
                        draft->paragraph_count, &anchor)) {
      status = RM_ERR_ANCHOR_OUT_OF_RANGE;
      rmi_error_set(out_error, status, 0, paragraph_index_of(mark->anchor),
                    "a mark");
      rm_buf_free(&block);
      rm_buf_free(&file);
      goto fail;
    }
    anchored_mark_blocks(&file, mark, number, anchor, previous);
    previous = mark_ids_of(number).item;
  }

  for (size_t number = 0; number < draft->image_count; number += 1) {
    const rm_draft_image* image = &draft->images[number];
    image_ids ids = image_ids_of(number);
    if (image->has_anchor) {
      anchor_spec anchor;

      /* The same refusal as a mark's, for the same reason. */
      if (!anchor_spec_of(image->anchor, text.first_characters,
                          draft->paragraph_count, &anchor)) {
        status = RM_ERR_ANCHOR_OUT_OF_RANGE;
        rmi_error_set(out_error, status, 0, paragraph_index_of(image->anchor),
                      "an image");
        rm_buf_free(&block);
        rm_buf_free(&file);
        goto fail;
      }

      /* The group carries the anchor and the image hangs inside it, alone —
       * as on a tablet page, where the item's `left` is unset and the group's
       * place in the layer's chain is what orders the drawing. The tablet
       * stamps `anchor_origin_x` with the id of the group's own node, not a
       * fresh counter — mirrored, like every other byte of the group. */
      anchored_group_blocks(&file, ids.node, ids.group_item,
                            (anchor_stamps){.anchor = ids.anchor,
                                            .type = ids.type,
                                            .threshold = ids.threshold,
                                            .origin = ids.node},
                            anchor, image->pen_origin, previous);
      block.count = 0;
      image_item(&block, image, ids, ids.node, ID_NONE);

      rm_write_top_block(&file, RM_BLOCK_IMAGE_ITEM, 2, 2, block.bytes,
                         block.count);
      rmi_buf_propagate(&file, &block);
      previous = ids.group_item;
    } else {
      block.count = 0;
      image_item(&block, image, ids, ID_LAYER, previous);
      rm_write_top_block(&file, RM_BLOCK_IMAGE_ITEM, 2, 2, block.bytes,
                         block.count);
      rmi_buf_propagate(&file, &block);
      previous = ids.item;
    }
  }

#undef EMIT

  rm_buf_free(&block);
  status = rmi_buf_status(&file);
  if (status != RM_OK) {
    rm_buf_free(&file);
    rmi_error_set(out_error, status, 0, 0, nullptr);
    goto fail;
  }

  *out_bytes = rm_buf_take(&file, out_size);
  if (*out_bytes == nullptr) {
    status = RM_ERR_OUT_OF_MEMORY;
    rmi_error_set(out_error, status, 0, 0, nullptr);
    goto fail;
  }
  rm_buf_free(&text.bytes);
  free(text.first_characters);
  return RM_OK;

fail:
  rm_buf_free(&text.bytes);
  free(text.first_characters);
  return status;
}

void rm_bytes_free(uint8_t* bytes) { free(bytes); }
