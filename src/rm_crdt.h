/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* CRDT sequence ordering, run splitting, and scene tree state. */

#ifndef RM_CRDT_H
#define RM_CRDT_H

#include "rm_support.h"

/* ------------------------------------------------------------------ */
/* Text CRDT records                                                   */
/* ------------------------------------------------------------------ */

/* One item of text: its own identifier, its neighbours in the CRDT list and
 * the characters themselves. The characters get identifiers running on from
 * the item's identifier. */
typedef struct text_item {
  /* Original row for verbatim editor copies. */
  size_t source;
  bool fragment;
  rm_crdt_id id;
  rm_crdt_id left;
  rm_crdt_id right;
  rm_str text;

  /* How many characters this record deletes. Deleted characters keep their
   * identifiers, so the next record's `left` counts past them. */
  uint32_t deleted;
  uint64_t opaque_span;

  /* The code a record carries when it is not text but a marker — any value
   * but nought; absent for text. 1 bold on, 2 bold off, 3 italic on, 4 italic
   * off: odd switches on and even switches off — but not one pair for every
   * purpose, or italic would use the same two numbers as bold. 5 and 6 open
   * and close a link, and the opening record's string is the address.
   * 0xFFFFFFFF is an image in the flow of the text, and its string is the
   * `{UUID}` key of the image's row in block 0x0E.
   *
   * Whatever its string, a marker takes exactly one identifier: the address
   * of a link adds no characters to the text. */
  bool has_marker;
  uint32_t marker;
} text_item;

/* A compact row can own several consecutive counters. Split where another
 * item names an interior position before resolving the neighbours. */
uint64_t rmi_text_item_span(const text_item* item);
rm_status rm_split_text_runs_with_cuts(const text_item* rows, size_t count,
                                       const rm_crdt_id* cuts, size_t cut_count,
                                       size_t max_parts, rm_vec* parts);
rm_status rm_split_text_runs_checked(const text_item* rows, size_t count,
                                     rm_vec* parts);
bool rm_split_text_runs(const text_item* rows, size_t count, rm_vec* parts);
bool rm_text_record_order(const text_item* items, size_t count, uint32_t** out);

constexpr rm_crdt_id RM_CRDT_ROOT = {.counter = 1, .author = 0};

/* ------------------------------------------------------------------ */
/* Scene CRDT items & tree state                                       */
/* ------------------------------------------------------------------ */

typedef enum : int {
  ITEM_TOMBSTONE,
  ITEM_STROKE,
  ITEM_GROUP,
  ITEM_IMAGE,
  ITEM_GLYPH
} rmi_item_kind;

/* One item of a node's list of children. Strokes, images, highlights and
 * child groups share the list. Keep the tombstones while ordering it because
 * a later record may hang its left link on one. */
typedef struct rmi_scene_item {
  rm_crdt_id parent;
  rm_crdt_id id;
  rm_crdt_id left, right;

  /* The number of records deleted here. A zero leaves the chain position
   * intact even when the block carries no drawable value. */
  uint32_t deleted;
  rmi_item_kind kind;
  bool placement_only;
  size_t block_start;
  union {
    rm_stroke stroke;
    rm_highlight glyph;
  };
  rm_crdt_id group;

  /* The opaque key of an image's attachment, pointing into the file's own
   * bytes, and the four vertices. */
  const uint8_t* image_key;
  rm_image_vertex vertices[4];
} rmi_scene_item;

/* One record of the scene tree (0x01): a node entering or leaving. Applied in
 * file order, these are the one authority on which layers exist — 0x02 and
 * 0x04 both survive a layer's deletion. */
typedef struct rmi_tree_event {
  rm_crdt_id node;
  bool in_tree;

  /* Where the record puts the node (tag 4) — read only for a node going in;
   * `has_parent` false when the record names no parent. */
  bool has_parent;
  rm_crdt_id parent;
} rmi_tree_event;

/* The scene tree as its records leave it, applied in file order: the nodes
 * taken out, and the parent each node in the tree was last put under. */
typedef struct rmi_tree_state {
  /* At first the table records explicit removals, including placement under
   * (0, 0). The tree walk extends it to descendants and stale root links. A
   * page with no placement records can still draw its groups. */
  rm_idmap removed;

  /* node -> index into `events` of the record that last put it under a
   * parent. The record's place and not the parent itself: an identifier is
   * wider than the map's value. */
  rm_idmap placed_by;
  const rmi_tree_event* events;
  bool has_root_placements;
} rmi_tree_state;

/* Replay tree records before following group links. Old links can survive a
 * move or a deletion. */
bool rmi_tree_state_build(const rmi_tree_event* events, size_t event_count,
                          rmi_tree_state* out);
void rmi_tree_state_free(rmi_tree_state* tree);
bool rmi_tree_parent_of(const rmi_tree_state* tree, rm_crdt_id node,
                        rm_crdt_id* out);
bool rmi_tree_links(const rmi_tree_state* tree, rm_crdt_id parent,
                    rm_crdt_id child);
bool rmi_tree_reachable_from_root(const rmi_scene_item* items,
                                  size_t item_count, const rmi_tree_state* tree,
                                  rm_idmap* out_reachable);
bool rmi_removed_subtrees(const rmi_scene_item* items, size_t item_count,
                          rmi_tree_state* tree);
uint32_t* rmi_chain_order(const uint32_t* indices, size_t count,
                          const rmi_scene_item* items, size_t* out_count);

/* Count visits and generated fragments so tests can check work on long runs
 * without timing a parse. */
#ifdef RMLINES_TESTING
void rm_test_reset_chain_order_calls(void);
size_t rm_test_chain_order_calls(void);
size_t rm_test_split_output_count(void);
void rm_test_set_split_output_count(size_t count);
size_t rm_test_run_split_visits(void);
#endif

#endif /* RM_CRDT_H */
