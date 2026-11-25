/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* CRDT sequence ordering, compact run splitting, and scene tree
 * liveness propagation. */

#include "rm_crdt.h"

#include <stdlib.h>
#include <string.h>

#ifdef RMLINES_TESTING
static _Thread_local size_t rm_test_chain_calls = 0;
static _Thread_local size_t rm_test_split_count = 0;
static _Thread_local size_t rm_test_split_visits = 0;

void rm_test_reset_chain_order_calls(void) {
  rm_test_chain_calls = 0;
  rm_test_split_count = 0;
  rm_test_split_visits = 0;
}

size_t rm_test_chain_order_calls(void) { return rm_test_chain_calls; }

size_t rm_test_split_output_count(void) { return rm_test_split_count; }

void rm_test_set_split_output_count(size_t count) {
  rm_test_split_count = count;
}

size_t rm_test_run_split_visits(void) { return rm_test_split_visits; }
#endif

/* How many identifiers the row covers. Text counts by Unicode scalars, a
 * marker takes one and a deletion keeps its whole range. For an unknown
 * encoding, keep the span the value decoder reserved. */
uint64_t rmi_text_item_span(const text_item* item) {
  if (item->deleted != 0) return (uint64_t)item->deleted;
  if (item->opaque_span != 0) return item->opaque_span;
  if (item->has_marker) return 1;
  return item->text.length == 0 ? 0
                                : (uint64_t)rm_utf8_scalar_count(
                                      item->text.bytes, item->text.length);
}

static int compare_positions(const void* a, const void* b) {
  const rm_crdt_id* x = a;
  const rm_crdt_id* y = b;
  if (x->author != y->author) return (int)x->author - (int)y->author;
  return (x->counter > y->counter) - (x->counter < y->counter);
}

/* Split only where a neighbour names a position inside a run. Keep each
 * fragment's source row so an untouched row can still go back verbatim. */
rm_status rm_split_text_runs_with_cuts(const text_item* rows, size_t count,
                                       const rm_crdt_id* cuts, size_t cut_count,
                                       size_t max_parts, rm_vec* parts) {
  for (size_t i = 0; i < count; ++i) {
    text_item part = rows[i];
    part.source = i;
    uint64_t span = rmi_text_item_span(&part);
    if (span <= 1 || cut_count == 0) {
      if (parts->count >= max_parts) return RM_ERR_MALFORMED_PAGE;
      if (!rm_vec_push(parts, &part)) return RM_ERR_OUT_OF_MEMORY;
      continue;
    }
    size_t lo = 0, hi = cut_count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2u;
      if (compare_positions(&cuts[mid], &part.id) <= 0)
        lo = mid + 1u;
      else
        hi = mid;
    }

    for (; lo < cut_count && cuts[lo].author == part.id.author; ++lo) {
#ifdef RMLINES_TESTING
      rm_test_split_visits += 1u;
#endif
      uint64_t length = cuts[lo].counter - part.id.counter;
      if (length >= span) break;
      if (length == 0) continue;
      if (parts->count >= max_parts) return RM_ERR_MALFORMED_PAGE;
      text_item prefix = part;
      prefix.fragment = part.fragment = true;
      prefix.right = cuts[lo];
      if (part.deleted == 0 && part.text.length != 0) {
        size_t byte = 0;

        for (uint64_t k = 0; k < length; ++k)
          byte = rm_utf8_next(part.text.bytes, part.text.length, byte, nullptr);
        prefix.text.length = byte;
        part.text.bytes += byte;
        part.text.length -= byte;
      } else if (part.opaque_span != 0) {
        prefix.opaque_span = length;
        part.opaque_span -= length;
      } else {
        prefix.deleted = (uint32_t)length;
        part.deleted -= prefix.deleted;
      }

      if (!rm_vec_push(parts, &prefix)) return RM_ERR_OUT_OF_MEMORY;
      part.id = part.left = cuts[lo];
      --part.left.counter;
      span -= length;
    }
    if (parts->count >= max_parts) return RM_ERR_MALFORMED_PAGE;
    if (!rm_vec_push(parts, &part)) return RM_ERR_OUT_OF_MEMORY;
  }
#ifdef RMLINES_TESTING
  rm_test_split_count = parts->count;
#endif
  return RM_OK;
}

/* A left reference cuts after its scalar, a right reference before it. Sort
 * the cut positions so even a large deleted span costs work only for the
 * boundaries it contains. */
rm_status rm_split_text_runs_checked(const text_item* rows, size_t count,
                                     rm_vec* parts) {
  rm_vec cuts;
  rm_vec_init(&cuts, sizeof(rm_crdt_id));
  rm_status status = RM_ERR_OUT_OF_MEMORY;
  for (size_t i = 0; i < count; ++i) {
    rm_crdt_id left = rows[i].left, right = rows[i].right;
    if (!rm_crdt_equal(left, RM_CRDT_NONE) && left.counter < UINT64_MAX) {
      ++left.counter;
      if (!rm_vec_push(&cuts, &left)) goto done;
    }
    if (!rm_crdt_equal(right, RM_CRDT_NONE) && !rm_vec_push(&cuts, &right))
      goto done;
  }
  rm_crdt_id* cut = cuts.data;
  size_t unique_cuts = 0;
  if (cuts.count > 0) {
    qsort(cut, cuts.count, sizeof *cut, compare_positions);

    for (size_t i = 0; i < cuts.count; ++i) {
      if (unique_cuts == 0 || !rm_crdt_equal(cut[unique_cuts - 1], cut[i])) {
        cut[unique_cuts++] = cut[i];
      }
    }
  }
  size_t base = unique_cuts <= SIZE_MAX - 16u ? unique_cuts + 16u : SIZE_MAX;
  size_t max_parts =
      count <= (SIZE_MAX - base) / 4u ? count * 4u + base : SIZE_MAX;
  status = rm_split_text_runs_with_cuts(rows, count, cut, unique_cuts,
                                        max_parts, parts);
done:
  rm_vec_free(&cuts);
  return status;
}

bool rm_split_text_runs(const text_item* rows, size_t count, rm_vec* parts) {
  return rm_split_text_runs_checked(rows, count, parts) == RM_OK;
}

/* The order the text reads in */
typedef struct text_key_ctx {
  const text_item* items;
} text_key_ctx;

/* The grouping key for a text record: the identifier its `left` names. */
static rm_crdt_id text_left_key(size_t index, void* ctx) {
  return ((text_key_ctx*)ctx)->items[index].left;
}

typedef struct neighbours {
  rm_crdt_id id, left, right;
} neighbours;

typedef struct neighbour_ctx {
  const uint32_t* indices;
  const text_item* text;
  const rmi_scene_item* scene;
  const rm_idmap* run_positions;
} neighbour_ctx;

/* Both text and scene items use left and right identifiers. Resolve them
 * against the same traversal order so the two walks agree on ties. */
static neighbours neighbours_at(const neighbour_ctx* ctx, size_t position) {
  size_t i = ctx->indices[position];
  if (ctx->text != nullptr) {
    const text_item* row = &ctx->text[i];
    return (neighbours){row->id, row->left, row->right};
  }
  const rmi_scene_item* item = &ctx->scene[i];
  return (neighbours){item->id, item->left, item->right};
}

static rm_crdt_id neighbour_right(size_t i, void* opaque) {
  const neighbour_ctx* ctx = opaque;
  rm_crdt_id right = neighbours_at(ctx, i).right;
  uint64_t owner;
  if (ctx->run_positions != nullptr && !rm_crdt_equal(right, RM_CRDT_NONE) &&
      rm_idmap_get(ctx->run_positions, right, &owner))
    return neighbours_at(ctx, owner).id;
  return right;
}

/* Emit prerequisites first. Each edge is visited once, and a back edge into
 * the current path is ignored so malformed cycles terminate. */
static bool order_neighbours(const uint32_t* order, size_t count,
                             neighbour_ctx* ctx, uint32_t* sorted) {
  typedef struct frame {
    uint32_t index;
    size_t next, end;
    bool left_done;
  } frame;
  rm_grouping before = {0};
  rm_idmap positions;
  rm_idmap_init(&positions);
  frame* path = calloc(count, sizeof *path);
  uint8_t* state = calloc(count, 1);
  bool ok = false;
  if (!path || !state) goto done;
  for (size_t i = 0; i < count; i++) {
    rm_crdt_id id = neighbours_at(ctx, i).id;
    if (rm_crdt_equal(id, RM_CRDT_NONE)) continue;
    if (!rm_idmap_set(&positions, id, i)) goto done;

    /* A neighbour can name the final position of a compact run. */
    uint64_t span = ctx->text != nullptr
                        ? rmi_text_item_span(&ctx->text[order[i]])
                        : ctx->scene[order[i]].deleted;
    uint64_t last = rmi_span_last(id.counter, span);
    if (last != id.counter) {
      id.counter = last;
      if (!rm_idmap_set(&positions, id, i)) goto done;
    }
  }
  neighbour_ctx keys = *ctx;
  keys.run_positions = &positions;
  if (!rm_grouping_build(&before, count, neighbour_right, &keys)) goto done;
  size_t written = 0, depth = 0;

  for (size_t root = 0; root < count; root++) {
    if (state[root]) continue;
    path[depth++] = (frame){.index = (uint32_t)root};
    while (depth) {
      frame* f = &path[depth - 1];
      neighbours item = neighbours_at(ctx, f->index);
      uint64_t dep;
      if (!state[f->index]) {
        state[f->index] = 1;
        size_t g;
        if (rm_grouping_number(&before, item.id, &g) &&
            !rm_crdt_equal(item.id, RM_CRDT_NONE)) {
          f->next = before.starts[g];
          f->end = before.ends[g];

          /* Identical IDs share the same predecessors. */
          before.starts[g] = before.ends[g];
        }
      }
      if (!f->left_done) {
        f->left_done = true;
        if (rm_crdt_equal(item.left, RM_CRDT_NONE) ||
            !rm_idmap_get(&positions, item.left, &dep))
          continue;
      } else if (f->next < f->end)
        dep = before.items[f->next++];
      else {
        state[f->index] = 2;
        sorted[written++] = order[f->index];
        --depth;
        continue;
      }
      if (!state[dep]) path[depth++] = (frame){.index = (uint32_t)dep};
    }
  }
  ok = true;

done:
  free(path);
  free(state);
  rm_idmap_free(&positions);
  rm_grouping_free(&before);
  return ok;
}

/* First walk the left-linked branches to get the reading order of ties. Then
 * impose every explicit neighbour dependency on that sequence. */
bool rm_text_record_order(const text_item* items, size_t count,
                          uint32_t** out_order) {
  *out_order = nullptr;
  if (count == 0) return true;

  bool ok = false;
  rm_grouping by_left = {0};
  uint32_t* scratch = nullptr;
  bool* spent = nullptr;
  bool* taken = nullptr;
  uint32_t* ordered = nullptr;

  text_key_ctx key_ctx = {.items = items};
  if (!rm_grouping_build(&by_left, count, text_left_key, &key_ctx)) goto done;

  /* Use the left-link traversal as the tie-breaker for unrelated records. */
  scratch = calloc(count, sizeof *scratch);
  spent = calloc(by_left.group_count, sizeof *spent);
  taken = calloc(count, sizeof *taken);
  ordered = calloc(count, sizeof *ordered);
  if (scratch == nullptr || spent == nullptr || taken == nullptr ||
      ordered == nullptr) {
    goto done;
  }
  size_t written = 0;
  rm_crdt_id cursor = RM_CRDT_NONE;
  size_t pending = 0;
  for (;;) {
    size_t group;
    if (rm_grouping_number(&by_left, cursor, &group) && !spent[group]) {
      spent[group] = true;
      for (size_t j = by_left.ends[group]; j > by_left.starts[group]; --j)
        scratch[pending++] = by_left.items[j - 1];
    }
    if (!pending) break;
    uint32_t index = scratch[--pending];
    taken[index] = true;
    ordered[written++] = index;

    /* The characters this record covers: the ones it carries, the ones it
     * deleted — a deletion holds its place in the sequence — or the one of a
     * marker, whatever its string spells. */
    const text_item* item = &items[index];
    uint64_t span = rmi_text_item_span(item);

    /* A compact run can start so high that its last counter would wrap into
     * another character's range. Stop at the largest counter in that case. */
    cursor =
        rm_crdt_make(item->id.author, rmi_span_last(item->id.counter, span));
  }

  /* The links did not add up — the rest is appended as it lies, in file
   * order, so as not to lose it. */
  for (size_t i = 0; i < count; i += 1) {
    if (!taken[i]) ordered[written++] = (uint32_t)i;
  }

  neighbour_ctx ctx = {.indices = ordered, .text = items};
  if (!order_neighbours(ordered, count, &ctx, scratch)) goto done;
  *out_order = scratch;
  scratch = nullptr;
  ok = true;

done:
  free(ordered);
  free(taken);
  free(spent);
  free(scratch);
  rm_grouping_free(&by_left);
  return ok;
}

/* ------------------------------------------------------------------ */
/* The order the scene draws in */
/* ------------------------------------------------------------------ */

/* Which of two identifiers is the older — the chain's tie-break reads "newer
 * first". The counters grow monotonically as a journal of edits, so the
 * counter decides; two counters can only tie across authors, and there the
 * **lower** author goes first in the chain. On a page edited on two tablets
 * at once, where each added a layer as the same item after the same
 * neighbour, the second author's layer is drawn over the first's — the higher
 * author later in the chain, on top. The other way round would put the first
 * author's stroke over the second's where the two cross. */
static bool is_ordered_before(rm_crdt_id left, rm_crdt_id right) {
  if (left.counter != right.counter) return left.counter < right.counter;
  return left.author > right.author;
}

/* A stable bottom-up merge sort. Stable and not `qsort` because two records
 * may carry one identifier, and where the rule cannot separate them the
 * file's own order has to stand — twice over the same page must draw the same
 * picture. */
static void sort_by_chain_rule(uint32_t* values, size_t count,
                               const rmi_scene_item* items, uint32_t* scratch) {
  for (size_t width = 1; width < count; width *= 2) {
    for (size_t start = 0; start < count; start += 2 * width) {
      size_t middle = start + width < count ? start + width : count;
      size_t end = start + 2 * width < count ? start + 2 * width : count;
      size_t left = start, right = middle, out = start;
      while (left < middle && right < end) {
        if (is_ordered_before(items[values[right]].id,
                              items[values[left]].id)) {
          scratch[out++] = values[right++];
        } else {
          scratch[out++] = values[left++];
        }
      }
      while (left < middle) scratch[out++] = values[left++];
      while (right < end) scratch[out++] = values[right++];
    }
    memcpy(values, scratch, count * sizeof *values);
  }
}

typedef struct scene_left_ctx {
  const rmi_scene_item* items;
  const uint32_t* positions;
} scene_left_ctx;

/* The grouping key for a position among a node's children: its `left`. */
static rm_crdt_id scene_left_key(size_t position, void* ctx) {
  scene_left_ctx* state = ctx;
  return state->items[state->positions[position]].left;
}

/* A node's children in the order of their chain: from the head at (0, 0),
 * each record followed by whatever hangs on it, and a tie between records
 * sharing a `left` going to the newer — larger — identifier.
 *
 * Iterative, because a chain is as long as a page has strokes and the walk
 * must not be a stack overflow. The result is malloc'd and the caller frees
 * it. */
uint32_t* rmi_chain_order(const uint32_t* indices, size_t count,
                          const rmi_scene_item* items, size_t* out_count) {
#ifdef RMLINES_TESTING
  rm_test_chain_calls += 1;
#endif
  *out_count = count;
  uint32_t* result = calloc(count == 0 ? 1 : count, sizeof *result);
  if (result == nullptr) return nullptr;

  /* Unlinked placements follow all sequence items in placement-record order.
   * They have no sequence IDs and cannot participate in neighbour resolution.
   */
  while (count && items[indices[count - 1]].placement_only) {
    --count;
    result[count] = indices[count];
  }
  if (count <= 1) {
    if (count == 1) result[0] = indices[0];
    return result;
  }

  rm_grouping by_left = {0};
  bool* spent = nullptr;
  bool* taken = nullptr;
  uint32_t* stack = nullptr;
  uint32_t* scratch = nullptr;

  scene_left_ctx key_ctx = {.items = items, .positions = indices};
  if (!rm_grouping_build(&by_left, count, scene_left_key, &key_ctx)) goto fail;
  spent =
      calloc(by_left.group_count == 0 ? 1 : by_left.group_count, sizeof *spent);
  taken = calloc(count, sizeof *taken);
  stack = calloc(count, sizeof *stack);
  scratch = calloc(count, sizeof *scratch);
  if (spent == nullptr || taken == nullptr || stack == nullptr ||
      scratch == nullptr)
    goto fail;

  /* The chain, walked from the head at (0, 0): each record, then whatever
   * hangs on it. A bucket leaves its group whole — every record sharing a
   * `left` is a tie, and the tie is ordered once, when it is first reached.
   *
   * The stack holds item indexes and never grows past `count`, because every
   * bucket is pushed at most once and the buckets partition the input. */
  size_t stack_count = 0;
  size_t written = 0;
  rm_crdt_id cursor = RM_CRDT_NONE;
  for (;;) {
    size_t group;
    if (rm_grouping_number(&by_left, cursor, &group) && !spent[group]) {
      spent[group] = true;
      uint32_t start = by_left.starts[group];
      size_t size = by_left.ends[group] - start;
      uint32_t* run = stack + stack_count;
      for (size_t i = 0; i < size; i += 1) {
        uint32_t position = by_left.items[start + i];
        taken[position] = true;
        run[i] = indices[position];
      }

      /* Ascending, because a stack pops from the end: the newest — the one
       * that goes first — has to be pushed last. */
      sort_by_chain_rule(run, size, items, scratch);
      stack_count += size;
    }
    if (stack_count == 0) break;
    uint32_t index = stack[--stack_count];
    result[written++] = index;
    cursor = rm_crdt_make(
        items[index].id.author,
        rmi_span_last(items[index].id.counter, items[index].deleted));
  }

  /* Links that did not add up — appended as they lie, not lost. */
  for (size_t position = 0; position < count && written < count;
       position += 1) {
    if (!taken[position]) result[written++] = indices[position];
  }

  neighbour_ctx ctx = {.indices = result, .scene = items};
  if (!order_neighbours(result, count, &ctx, scratch)) goto fail;
  memcpy(result, scratch, count * sizeof *result);
  free(spent);
  free(taken);
  free(stack);
  free(scratch);
  rm_grouping_free(&by_left);
  return result;

fail:
  free(spent);
  free(taken);
  free(stack);
  free(scratch);
  rm_grouping_free(&by_left);
  free(result);
  return nullptr;
}

/* Apply the page's tree records in order. False on allocation failure; the
 * state is to be freed either way. */
bool rmi_tree_state_build(const rmi_tree_event* events, size_t event_count,
                          rmi_tree_state* out) {
  rm_idmap_init(&out->removed);
  rm_idmap_init(&out->placed_by);
  out->events = events;
  out->has_root_placements = false;
  for (size_t i = 0; i < event_count; i += 1) {
    const rmi_tree_event* event = &events[i];
    if (!event->in_tree) {
      if (!rm_idmap_set(&out->removed, event->node, 1)) return false;
      continue;
    }
    if (event->has_parent && rm_crdt_equal(event->parent, RM_CRDT_NONE)) {
      if (!rm_idmap_set(&out->removed, event->node, 1) ||
          !rm_idmap_set(&out->placed_by, event->node, i))
        return false;
      continue;
    }
    (void)rm_idmap_take(&out->removed, event->node, nullptr);
    if (event->has_parent && !rm_idmap_set(&out->placed_by, event->node, i))
      return false;
  }

  size_t cursor = 0;
  rm_crdt_id node;
  uint64_t idx;
  while (rm_idmap_next(&out->placed_by, &cursor, &node, &idx)) {
    if (!rm_idmap_contains(&out->removed, node) &&
        rm_crdt_equal(events[idx].parent, RM_CRDT_ROOT)) {
      out->has_root_placements = true;
      break;
    }
  }
  return true;
}

/* Release the tables built from the tree records. The records themselves
 * belong to the scene. */
void rmi_tree_state_free(rmi_tree_state* tree) {
  rm_idmap_free(&tree->placed_by);
  rm_idmap_free(&tree->removed);
}

/* The parent the records last put `node` under, or false when no record
 * places it. */
bool rmi_tree_parent_of(const rmi_tree_state* tree, rm_crdt_id node,
                        rm_crdt_id* out) {
  uint64_t index;
  if (!rm_idmap_get(&tree->placed_by, node, &index)) return false;
  *out = tree->events[index].parent;
  return true;
}

/* A moved node keeps its old group item until the tablet tombstones it.
 * Follow its current placement when there is one. A root link without a
 * placement needs separate handling on pages that explicitly place their
 * layers. */
bool rmi_tree_links(const rmi_tree_state* tree, rm_crdt_id parent,
                    rm_crdt_id child) {
  rm_crdt_id placed;
  if (tree->has_root_placements && rm_crdt_equal(parent, RM_CRDT_ROOT)) {
    return rmi_tree_parent_of(tree, child, &placed) &&
           rm_crdt_equal(placed, parent);
  }
  return !rmi_tree_parent_of(tree, child, &placed) ||
         rm_crdt_equal(placed, parent);
}

/* A node under its parent, as a group item or a tree record puts it. */
typedef struct tree_link {
  rm_crdt_id parent;
  rm_crdt_id child;
} tree_link;

/* The grouping key for a link, by its position in the links: the node it
 * hangs on. */
static rm_crdt_id link_parent_key(size_t position, void* ctx) {
  const tree_link* links = ctx;
  return links[position].parent;
}

/* Combine the surviving group links with the last placement of each node.
 * Some pages carry only one of the two. */
static bool collect_tree_links(const rmi_scene_item* items, size_t item_count,
                               const rmi_tree_state* tree,
                               bool collect_unplaced, rm_vec* links,
                               rm_vec* unplaced_root) {
  for (size_t i = 0; i < item_count; i += 1) {
    if (items[i].deleted != 0 || items[i].kind != ITEM_GROUP) continue;
    if (tree->has_root_placements &&
        rm_crdt_equal(items[i].parent, RM_CRDT_ROOT) &&
        !rmi_tree_parent_of(tree, items[i].group, &(rm_crdt_id){0})) {
      if (collect_unplaced && !rm_vec_push(unplaced_root, &items[i].group))
        return false;
      continue;
    }
    if (!rmi_tree_links(tree, items[i].parent, items[i].group)) continue;
    tree_link link = {.parent = items[i].parent, .child = items[i].group};
    if (!rm_vec_push(links, &link)) return false;
  }
  size_t cursor = 0;
  rm_crdt_id node;
  uint64_t index;
  while (rm_idmap_next(&tree->placed_by, &cursor, &node, &index)) {
    if (rm_crdt_equal(tree->events[index].parent, RM_CRDT_NONE)) continue;
    tree_link link = {.parent = tree->events[index].parent, .child = node};
    if (!rm_vec_push(links, &link)) return false;
  }
  return true;
}

/* Seed the queue with known nodes. A child reached twice, including through a
 * cycle, needs visiting only once. */
static bool propagate_removed(const rm_grouping* children_of,
                              const rm_vec* links, const rm_idmap* keep,
                              rm_vec* queue, rm_idmap* removed) {
  const tree_link* all = links->data;
  while (queue->count > 0) {
    rm_crdt_id node = *(rm_crdt_id*)rm_vec_at(queue, queue->count - 1);
    queue->count -= 1;
    const uint32_t* children;
    size_t count;
    if (!rm_grouping_find(children_of, node, &children, &count)) continue;
    for (size_t k = 0; k < count; k += 1) {
      rm_crdt_id child = all[children[k]].child;
      if ((keep != nullptr && rm_idmap_contains(keep, child)) ||
          rm_idmap_contains(removed, child))
        continue;
      if (!rm_idmap_set(removed, child, 1) || !rm_vec_push(queue, &child))
        return false;
    }
  }
  return true;
}

/* Layer edits need to know whether a node can still be reached from the page
 * root before using it as a parent. */
bool rmi_tree_reachable_from_root(const rmi_scene_item* items,
                                  size_t item_count, const rmi_tree_state* tree,
                                  rm_idmap* out_reachable) {
  rm_idmap_init(out_reachable);
  if (rm_idmap_contains(&tree->removed, RM_CRDT_ROOT)) return true;

  bool ok = false;
  rm_vec links, queue;
  rm_vec_init(&links, sizeof(tree_link));
  rm_vec_init(&queue, sizeof(rm_crdt_id));
  rm_grouping children_of = {0};

  if (!collect_tree_links(items, item_count, tree, false, &links, nullptr) ||
      !rm_grouping_build(&children_of, links.count, link_parent_key,
                         links.data))
    goto done;

  rm_crdt_id root = RM_CRDT_ROOT;
  if (!rm_idmap_set(out_reachable, root, 1) || !rm_vec_push(&queue, &root))
    goto done;
  if (!propagate_removed(&children_of, &links, &tree->removed, &queue,
                         out_reachable))
    goto done;
  ok = true;

done:
  if (!ok) rm_idmap_free(out_reachable);
  rm_grouping_free(&children_of);
  rm_vec_free(&queue);
  rm_vec_free(&links);
  return ok;
}

/* Carry removals down through current placements and group links. First mark
 * children still reachable by a live parent so removing another parent cannot
 * take them with it. Walk each queue iteratively: a cycle must end, however
 * deep the page's groups nest. */
bool rmi_removed_subtrees(const rmi_scene_item* items, size_t item_count,
                          rmi_tree_state* tree) {
  bool ok = false;
  rm_vec links, queue, live_queue, unplaced_root;
  rm_vec_init(&links, sizeof(tree_link));
  rm_vec_init(&queue, sizeof(rm_crdt_id));
  rm_vec_init(&live_queue, sizeof(rm_crdt_id));
  rm_vec_init(&unplaced_root, sizeof(rm_crdt_id));
  rm_grouping children_of = {0};
  rm_idmap reachable, has_parent;
  rm_idmap_init(&reachable);
  rm_idmap_init(&has_parent);

  if (!collect_tree_links(items, item_count, tree, true, &links,
                          &unplaced_root) ||
      !rm_grouping_build(&children_of, links.count, link_parent_key,
                         links.data))
    goto done;

  /* Compute surviving reachable nodes first (excluding explicitly removed or
   * hidden nodes), so shared children with another live parent survive. */
  rm_crdt_id root = RM_CRDT_ROOT;
  if (!rm_idmap_contains(&tree->removed, root)) {
    if (!rm_idmap_set(&reachable, root, 1) || !rm_vec_push(&live_queue, &root))
      goto done;
  }
  if (!tree->has_root_placements) {
    const tree_link* all = links.data;
    for (size_t k = 0; k < links.count; k += 1) {
      if (!rm_idmap_set(&has_parent, all[k].child, 1)) goto done;
    }

    for (size_t k = 0; k < links.count; k += 1) {
      rm_crdt_id p = all[k].parent;
      if (rm_idmap_contains(&has_parent, p) ||
          rm_idmap_contains(&tree->removed, p) ||
          rm_idmap_contains(&reachable, p))
        continue;
      if (!rm_idmap_set(&reachable, p, 1) || !rm_vec_push(&live_queue, &p))
        goto done;
    }
  }
  if (!propagate_removed(&children_of, &links, &tree->removed, &live_queue,
                         &reachable))
    goto done;

  size_t cursor = 0;
  rm_crdt_id node;
  uint64_t index;
  while (rm_idmap_next(&tree->removed, &cursor, &node, &index)) {
    if (!rm_vec_push(&queue, &node)) goto done;
  }
  if (!propagate_removed(&children_of, &links, &reachable, &queue,
                         &tree->removed))
    goto done;

  if (tree->has_root_placements && unplaced_root.count > 0) {
    const rm_crdt_id* cands = unplaced_root.data;
    for (size_t u = 0; u < unplaced_root.count; u += 1) {
      rm_crdt_id g = cands[u];
      if (rm_idmap_contains(&reachable, g) ||
          rm_idmap_contains(&tree->removed, g))
        continue;
      if (!rm_idmap_set(&tree->removed, g, 1) || !rm_vec_push(&queue, &g))
        goto done;
    }
    if (!propagate_removed(&children_of, &links, &reachable, &queue,
                           &tree->removed))
      goto done;
  }
  ok = true;

done:
  rm_idmap_free(&has_parent);
  rm_idmap_free(&reachable);
  rm_grouping_free(&children_of);
  rm_vec_free(&unplaced_root);
  rm_vec_free(&live_queue);
  rm_vec_free(&queue);
  rm_vec_free(&links);
  return ok;
}
