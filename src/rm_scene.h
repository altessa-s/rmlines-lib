/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* .rm v6 block schema, decoded scene/text intermediate state,
 * unified CRDT ID collision scanner, and scene query primitives. */

#ifndef RM_SCENE_H
#define RM_SCENE_H

#include "rm_crdt.h"
#include "rm_wire.h"

/* ------------------------------------------------------------------ */
/* Shared scene/text structures                                        */
/* ------------------------------------------------------------------ */

/* What a node hangs on in the typed text: the character the file names, and
 * the horizontal it carries alongside. */
typedef struct rmi_node_anchor {
  bool has_target;
  rm_crdt_id target_stamp, origin_stamp;
  rm_crdt_id anchor;
  bool has_origin_x;
  double origin_x;
} rmi_node_anchor;

typedef struct rmi_attachment {
  rm_crdt_id stamp;
  uint8_t key[16];
  rm_str name;
} rmi_attachment;

typedef struct rmi_style_entry {
  rm_crdt_id stamp;
  rm_crdt_id key;
  rm_paragraph_format format;
} rmi_style_entry;

/* A line under construction: its bytes, and the face of each of its scalars.
 *
 * The faces are kept per scalar rather than as ranges while reading, because
 * bold and italic switch independently and a range would have to be broken
 * and begun again at each of the four marks. */
typedef struct rmi_line_build {
  rm_buf text;

  /* uint8_t: bit 0 bold, bit 1 italic */
  rm_vec faces;
} rmi_line_build;

typedef struct rmi_text_para_meta {
  bool has_format;
  rm_paragraph_format format;
  bool has_bidi;
} rmi_text_para_meta;

/* The visible scalar at each edit position and the bytes and ID that produced
 * it. Duplicate IDs can be drawn but cannot unambiguously identify an edit
 * target. */
typedef struct rmi_text_flat_map {
  size_t count;
  rm_crdt_id* ids;
  uint32_t* scalars;
  uint8_t* faces;
  size_t* para_idx;
  bool* is_inline_image;
  size_t* byte_at;
  char* utf8;
  size_t para_count;
  rmi_text_para_meta* paras;
  rm_idmap pos_of;
  rm_idmap ambiguous_ids;
  rm_idmap newline_ambiguous_ids;
} rmi_text_flat_map;

/* The layout of typed text: where it starts and which line each character
 * stands on. */
typedef struct rmi_text_layout {
  double origin_x, origin_y;

  /* Where each character stands: its paragraph and how many characters of
   * that paragraph come before the pen once it is typed. It is what locates
   * anchored handwriting. Packed as paragraph << 32 | offset. */
  rm_idmap place_of_character;
  rm_idmap place_ambiguous;
  rm_idmap style_line_of_key;
  rm_vec lines; /* rmi_line_build */

  /* Paragraph style entries in file order, including later edits to the same
   * paragraph. */
  rm_vec styles;
  bool has_column_width;
  double column_width;
  rm_vec flat_ids;          /* rm_crdt_id, populated only when collected */
  rm_vec flat_inline_image; /* uint8_t, populated only when collected */
} rmi_text_layout;

typedef struct rmi_ordered_stroke {
  uint32_t item;
  rm_crdt_id parent;
} rmi_ordered_stroke;

/* An image item as the scene walk hands it over: the attachment it names, its
 * place in the drawing order, and the node it hangs on — the parent is what
 * says whether it is anchored. */
typedef struct rmi_placed_image {
  uint32_t item;
  size_t strokes_before;
  rm_crdt_id parent;
} rmi_placed_image;

/* Editors compare the interleaved order of ink, groups and images after a
 * change. */
typedef struct rmi_scene_event {
  rm_crdt_id parent;
  rm_crdt_id id;
  rmi_item_kind kind;
} rmi_scene_event;

typedef struct rmi_scene {
  rm_arena* arena;
  rm_reader reader;

  rm_vec items;       /* rmi_scene_item */
  rm_vec tree_events; /* rmi_tree_event */
  rm_vec attachments; /* rmi_attachment */

  /* key -> index into `attachments`. A scan per entry would be quadratic in
   * the number of attachments, and the number is the file's to choose: a page
   * of nothing but map entries would take seconds to read. */
  rm_keymap attachment_index;
  rm_vec node_anchors;     /* rmi_node_anchor */
  rm_idmap anchor_of_node; /* node id -> index into node_anchors */

  /* The nodes the user hid. Their strokes stay in the file with every one of
   * their points; it is the layer that is hidden, not its contents, and a
   * hidden layer is not part of the page as drawn. Drawing them shows the
   * user what they put away — the worst kind of discrepancy, because nothing
   * says it happened. */
  rm_idmap hidden_nodes;

  /* node id -> index into visibility_stamps */
  rm_idmap visibility_of_node;

  /* rm_crdt_id, including currently visible nodes */
  rm_vec visibility_stamps;

  bool has_text;
  bool collect_flat_map;
  rmi_text_layout text;

  /* Two reasons for skipping, not one. Mixed into a single counter they hide
   * the loss: nine hundred unknown blocks look ordinary, while two thousand
   * unread strokes in the same total do not. */
  size_t other_blocks;
  size_t unreadable_strokes;
  size_t deleted_items;
  char first_failure[160];

  /* What the block parser that failed last had to say, when it said more than
   * its status. Taken into `first_failure` with the status and cleared for
   * the next block. */
  char detail[120];

  rm_crdt_id sheet_stamp;
  bool has_stamped_sheet;
  bool has_canvas_size;
  bool has_paper_size;
  rm_size paper_size;

  /* Shown unless the file says otherwise: most pages carry no flag at all,
   * and a missing field means "never touched", not "hidden". */
  rm_crdt_id background_stamp, document_stamp;
  bool background_visible;
  bool root_document_visible;
} rmi_scene;

/* Byte ranges within a tree node for replacing the anchor stamp and target
 * while preserving any extension fields. */
typedef struct rm_anchor_field {
  rm_crdt_id node, stamp;
  rm_crdt_id target;
  size_t length_at, target_end, field_end;
} rm_anchor_field;

/* The node owns the layer's properties. Its link owns its position among the
 * root's children. */
typedef struct rmi_layer_info {
  rm_item_id node, link;
  bool visible;
  size_t link_block;
} rmi_layer_info;

static inline rm_stroke_point* rm_alloc_stroke_points(rm_arena* arena,
                                                      size_t count) {
  return rm_arena_calloc(arena, count, sizeof(rm_stroke_point));
}

/* A tree node carries its name, visibility and the character its handwriting
 * hangs on. Tag 10 gives the pen's horizontal directly. At a boundary between
 * writing directions the caret is ambiguous, so that coordinate cannot always
 * be recomputed. */
typedef struct rmi_tree_node {
  rm_crdt_id target_stamp, origin_stamp;
  rm_crdt_id id;
  bool has_anchor;
  rm_crdt_id anchor;
  bool has_origin_x;
  double origin_x;
  bool has_visible;
  bool visible;
  rm_crdt_id visibility_stamp;
  bool has_name;
  rm_crdt_id name_stamp;
  rm_str name;
} rmi_tree_node;

/* ------------------------------------------------------------------ */
/* Block registry & scene lifecycle                                    */
/* ------------------------------------------------------------------ */

/* A block can carry newer fields while declaring a minimum version this
 * reader understands. */
uint8_t rmi_block_supported_version(uint8_t block_type);
bool rmi_block_is_sequence(uint8_t block_type);

/* A short row-length prefix has already started a row. That is a different
 * stopping point from reaching the list boundary between rows. */
typedef enum rmi_row_envelope_status : uint8_t {
  RM_ROW_OK = 0,
  RM_ROW_EOF,
  RM_ROW_STOP,
  RM_ROW_SHORT_PREFIX,
} rmi_row_envelope_status;

rmi_row_envelope_status rmi_next_text_row_envelope(rm_reader* list,
                                                   size_t list_end,
                                                   size_t* out_row_start,
                                                   rm_reader* out_row);
bool rmi_next_text_settings_field(rm_reader* settings, uint32_t* out_tag,
                                  uint8_t* out_type, rm_reader* out_field);
rm_status rmi_parse_style_value(rm_reader* reader,
                                rm_paragraph_format* out_format,
                                bool* out_has_format);
rm_status rmi_decode_text_row_tag6(rm_arena* arena, rm_reader* row,
                                   text_item* out, bool* out_has_value_suffix);
rm_status rmi_parse_text_row(rmi_scene* ctx, rm_reader* reader, text_item* out);

/* Image values can carry the attachment key in a stamped field. The key bytes
 * borrow their storage from the reader. */
rm_status rmi_parse_image_key(rm_reader keyed, bool* out_has_stamp,
                              rm_crdt_id* out_stamp, const uint8_t** out_key,
                              size_t* out_key_len);
rm_status rmi_parse_tree_node(rm_reader* reader, size_t end,
                              rm_arena* name_arena, rmi_tree_node* out,
                              rm_anchor_field* location);

/* Supply an ordering edge for a placed group whose file has no live group
 * item at its current parent. */
bool rmi_append_unlinked_placements(rmi_scene* ctx, const rmi_tree_state* tree);
bool rmi_scene_group_by_parent(const rmi_scene_item* items, size_t count,
                               rm_grouping* out);

/* Keep hidden items during the block pass. Drawing and editing decide
 * separately which ones to expose. */
rm_status rmi_scene_read_blocks_ex(const uint8_t* data, size_t size,
                                   bool collect_flat_map, rmi_scene* ctx,
                                   rm_error* out_error);
rm_status rmi_scene_read_blocks(const uint8_t* data, size_t size,
                                rmi_scene* ctx, rm_error* out_error);
rm_status rmi_parse_page_with_edit_state(const uint8_t* data, size_t size,
                                         rm_page** out_page,
                                         rmi_text_flat_map* out_map,
                                         rm_idmap* out_edit_removed,
                                         rm_error* out_error);

/* Free the scene's scratch arrays after assembly. Destroy the arena too if no
 * page took ownership. */
void rmi_text_flat_map_free(rmi_text_flat_map* map);
void rmi_scene_free_scratch(rmi_scene* ctx);
void rmi_scene_destroy(rmi_scene* ctx);

/* ------------------------------------------------------------------ */
/* Scene ordering, layout, and edit query primitives                   */
/* ------------------------------------------------------------------ */

/* Drawing skips hidden nodes. An editor checking a replacement can include
 * them and also collect the ordering events. */
bool rmi_scene_order_items_ex(rmi_scene* ctx, rm_vec* out_strokes,
                              rm_vec* out_images, rm_vec* glyphs,
                              bool include_hidden, rm_vec* out_events,
                              rm_idmap* out_edit_removed);
bool rmi_scene_order_items(rmi_scene* ctx, rm_vec* out_strokes,
                           rm_vec* out_images, rm_vec* glyphs);

#ifdef RMLINES_TESTING
void rm_test_reset_scene_block_reads(void);
size_t rm_test_scene_block_reads(void);
/* Check the surviving highlights after all the original text was removed. */
rm_status rm_test_verify_glyph_deletion(rm_buf* candidate,
                                        const rmi_text_flat_map* original);
#endif

/* A placed item can borrow its parent's text anchor or resolve its file name
 * through the page's attachment map. */
bool rmi_scene_anchor_under(const rmi_scene* ctx, rm_crdt_id parent,
                            rm_text_anchor* out);
const rm_str* rmi_scene_attachment_name(const rmi_scene* ctx,
                                        const uint8_t* key);

[[nodiscard]] rm_status rm_read_node_anchor(rm_reader body,
                                            rm_anchor_field* anchor);
[[nodiscard]] rm_status rmi_scene_live_removed_nodes(const uint8_t* data,
                                                     size_t size,
                                                     rmi_tree_state* out_tree,
                                                     rm_error* out_error);

/* Check the whole proposed counter range, including IDs reserved by deleted
 * spans and references to items elsewhere on the page. */
[[nodiscard]] rm_status rmi_scene_check_ids(const uint8_t* data, size_t size,
                                            rm_item_id first, size_t count,
                                            rm_error* error);
[[nodiscard]] rm_status rmi_scene_check_layer_materialization_id(
    const uint8_t* data, size_t size, rm_item_id layer_node);
[[nodiscard]] rm_status rmi_scene_check_location(
    const uint8_t* data, size_t size, rm_item_id parent, rm_item_id left,
    rm_item_id right, bool has_metadata, rm_error* error);

/* A cut needs the source block as well as the next item in sequence. File
 * order alone supplies neither. */
[[nodiscard]] rm_status rmi_scene_resolve_stroke_targets(
    const uint8_t* data, size_t size, const rm_idmap* selected,
    bool duplicate_first, size_t* out_first_block_starts,
    rm_crdt_id* out_successors, rm_error* error);
[[nodiscard]] rm_status rmi_scene_stroke_successors(const uint8_t* data,
                                                    size_t size,
                                                    const rm_idmap* selected,
                                                    rm_crdt_id* successors,
                                                    rm_error* error);

/* Layer order comes from the tree. The node's name and visibility come from
 * their respective winning writes. */
[[nodiscard]] rm_status rmi_scene_layers(const uint8_t* data, size_t size,
                                         rmi_layer_info** out, size_t* count,
                                         rm_error* error);
[[nodiscard]] rm_status rmi_scene_node_state(const uint8_t* data, size_t size,
                                             rm_item_id node, bool* out_found,
                                             bool* out_visible,
                                             char** out_name);

/* Every living highlighter mark, including those on a hidden layer. The block
 * pass keeps those layers. If it counted a loss, the missing piece might
 * itself be a mark, so the query returns RM_ERR_INCOMPLETE_PAGE. */
[[nodiscard]] rm_status rm_scan_highlighter_items(const uint8_t* data,
                                                  size_t size, rm_vec* out_ids,
                                                  rm_error* out_error);

#endif /* RM_SCENE_H */
