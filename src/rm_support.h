/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The plumbing the parser and the writer are built out of: an arena,
 * growable arrays and a byte buffer, a UTF-8 decoder that repairs by the
 * Unicode "maximal subpart" rule, hash maps on CRDT identifiers, and a
 * grouping of items by key. Nothing here knows the format itself; its
 * byte-level reader and writer are in rm_wire.h.
 *
 * Internal: nothing here is in the installed headers, and no symbol below is
 * exported from the shared library. */

#ifndef RM_SUPPORT_H
#define RM_SUPPORT_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rmlines/types.h"

/* Return from the enclosing function on the first failure. */
#define RM_TRY(expr)                                    \
  do {                                                  \
    rm_status rm_try_status_ = (expr);                  \
    if (rm_try_status_ != RM_OK) return rm_try_status_; \
  } while (false)

/* ------------------------------------------------------------------ */
/* Arena                                                               */
/* ------------------------------------------------------------------ */

/* A bump allocator in chunks. A parsed page is a graph of small arrays that
 * all live and die together, so it is one allocation policy and one free —
 * and a caller who loses track of a stroke's points cannot free them out from
 * under the page. */
typedef struct rm_arena rm_arena;

/* A fresh, empty arena, or null on allocation failure. */
rm_arena* rm_arena_create(void);

/* Free an arena and everything allocated from it. Null is accepted and
 * ignored. */
void rm_arena_destroy(rm_arena* arena);

/* Aligned to `max_align_t`, zeroed nowhere: callers that need zeroes ask. */
void* rm_arena_alloc(rm_arena* arena, size_t size);

/* Like `rm_arena_alloc`, zeroed, and null without allocating on an overflow
 * of `count * size`. */
void* rm_arena_calloc(rm_arena* arena, size_t count, size_t size);

#ifdef RMLINES_TESTING
/* Fail one arena allocation after this many successes; SIZE_MAX disables it. */
void rm_test_arena_fail_after(size_t count);

/* Whether the counted allocation has failed since the last `_fail_after`. */
bool rm_test_arena_did_fail(void);
#endif

/* A copy of `length` bytes with a NUL behind them, so the result reaches
 * `printf` unchanged. */
char* rm_arena_strndup(rm_arena* arena, const char* bytes, size_t length);

/* ------------------------------------------------------------------ */
/* Growable arrays                                                     */
/* ------------------------------------------------------------------ */

/* Scratch storage for a parse: the counts are the file's to choose, so the
 * arrays grow, and what survives is copied into the page's arena at the end.
 * Backed by malloc rather than by the arena because an arena cannot
 * reallocate. */
typedef struct rm_vec {
  void* data;
  size_t count;
  size_t capacity;
  size_t elem;
} rm_vec;

/* Ready an empty vector of elements `elem_size` bytes wide. */
void rm_vec_init(rm_vec* vec, size_t elem_size);

/* Release the backing storage and leave the vector empty. */
void rm_vec_free(rm_vec* vec);

/* False on allocation failure, and then the vector is untouched. */
[[nodiscard]] bool rm_vec_push(rm_vec* vec, const void* element);

/* Grow the backing storage to hold at least `capacity` elements. False and
 * the vector untouched on allocation failure. */
[[nodiscard]] bool rm_vec_reserve(rm_vec* vec, size_t capacity);

/* The element at `index`, unchecked. */
void* rm_vec_at(const rm_vec* vec, size_t index);

typedef enum rm_buf_err : uint8_t {
  RM_BUF_OK = 0,
  RM_BUF_ERR_OOM,
  RM_BUF_ERR_TOO_LONG,
} rm_buf_err;

/* A growable byte buffer: what the writer builds a block in. */
typedef struct rm_buf {
  uint8_t* bytes;
  size_t count;
  size_t capacity;

  /* The first failure sticks, including a length that will not fit on the
   * wire. A chain of writes can be checked once, at the end. */
  rm_buf_err error;
} rm_buf;

void rm_buf_init(rm_buf* buf);

/* Release the backing storage and leave the buffer empty. */
void rm_buf_free(rm_buf* buf);

/* Append `length` bytes, or mark the buffer failed on allocation failure. */
void rm_buf_append(rm_buf* buf, const void* bytes, size_t length);

/* Append one byte, or mark the buffer failed on allocation failure. */
void rm_buf_append_byte(rm_buf* buf, uint8_t byte);
void rm_buf_shrink(rm_buf* buf, size_t count);

/* Backfill a previously reserved length by offset. Growing the buffer may
 * have moved its storage. */
void rm_write_u32_at(rm_buf* buf, size_t offset, uint32_t value);
void rmi_buf_fail(rm_buf* buf, rm_buf_err error);
void rmi_buf_propagate(rm_buf* dst, const rm_buf* src);
rm_status rmi_buf_status(const rm_buf* buf);

/* Hand the bytes over to the caller and leave the buffer empty. Null when the
 * buffer had failed. */
uint8_t* rm_buf_take(rm_buf* buf, size_t* out_size);

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

bool rm_utf8_is_valid(const char* bytes, size_t length);

/* How many Unicode scalars `length` bytes of **valid** UTF-8 hold. */
size_t rm_utf8_scalar_count(const char* bytes, size_t length);

/* Count the scalars after repair without materializing the repaired string,
 * so the ID preflight agrees with the writer. */
size_t rmi_utf8_repaired_scalar_count(const char* bytes, size_t length);

/* Copy `length` bytes into `out`, replacing every ill-formed sequence with
 * U+FFFD, and answer how many bytes were written.
 *
 * The replacement is the Unicode "maximal subpart" rule, which conformant
 * decoders follow — and matching it is not cosmetic: the file numbers
 * characters by scalar, so a byte sequence counted differently here shifts
 * every anchor and every style key behind it.
 *
 * `out` must hold at least `rm_utf8_repair_bound(length)` bytes. */
size_t rm_utf8_repair(const char* bytes, size_t length, char* out);

/* Worst-case repair size including NUL, or zero if unrepresentable. */
size_t rm_utf8_repair_bound(size_t length);
char* rmi_utf8_repair_alloc(const char* text);

/* Decode one scalar of valid UTF-8 and return the next byte offset. Callers
 * that only need to advance can leave out the scalar. */
size_t rm_utf8_next(const char* bytes, size_t length, size_t offset,
                    uint32_t* out_scalar);

typedef struct rm_text_counts {
  uint64_t chars, lines;
} rm_text_counts;

/* Start with {1,1} for a text document's implicit final paragraph end. */
void rm_text_counts_add(rm_text_counts* counts, const char* text,
                        size_t length);

/* ------------------------------------------------------------------ */
/* CRDT identifiers & hash maps                                        */
/* ------------------------------------------------------------------ */

/* The identifier of a scene element: the author of the edit and its counter. */
typedef struct rm_crdt_id {
  uint64_t counter;
  uint8_t author;
} rm_crdt_id;

/* Whether the two identifiers name the same record. */
bool rm_crdt_equal(rm_crdt_id a, rm_crdt_id b);

/* An identifier out of its two parts. */
rm_crdt_id rm_crdt_make(uint8_t author, uint64_t counter);

/* The public spelling of an internal identifier. */
rm_item_id rm_item_id_of(rm_crdt_id id);

/* The internal spelling of a public identifier. */
rm_crdt_id rm_crdt_of(rm_item_id id);

constexpr rm_crdt_id RM_CRDT_NONE = {.counter = 0, .author = 0};

/* Keep the last occupied counter representable even when a corrupt run
 * extends past the end of its author's range. */
static inline uint64_t rmi_span_last(uint64_t start, uint64_t span) {
  if (span <= 1u) return start;
  uint64_t delta = span - 1u;
  return delta <= UINT64_MAX - start ? start + delta : UINT64_MAX;
}

/* Compare counters first for stamped fields. At equal counters the author
 * breaks the tie. */
static inline bool rm_stamp_newer(rm_crdt_id a, rm_crdt_id b) {
  return a.counter > b.counter ||
         (a.counter == b.counter && a.author > b.author);
}

/* Compare ranges by subtraction. Forming their ends first could wrap near the
 * last counter. */
static inline bool rm_id_overlaps(rm_crdt_id id, uint64_t span,
                                  rm_item_id first, uint64_t count) {
  if (!count || !span || id.author != first.author) return false;
  return id.counter < first.counter ? first.counter - id.counter < span
                                    : id.counter - first.counter < count;
}

/* An open-addressing map from an identifier to a 64-bit value — an index into
 * an array of items, a count, or a set's presence.
 *
 * A map and not a sorted array because the parse asks it O(n) times: the
 * children of a node, the bucket of a `left` link, the place of a character.
 * At a few thousand records a linear scan per question is the parse itself
 * hanging. */
typedef struct rm_idmap {
  struct rm_idmap_slot* slots;
  size_t capacity;
  size_t count;
} rm_idmap;

void rm_idmap_init(rm_idmap* map);

/* Release the backing storage and leave the map empty. */
void rm_idmap_free(rm_idmap* map);

/* Set or replace the value under `key`. False on allocation failure, and then
 * the map is untouched. */
[[nodiscard]] bool rm_idmap_set(rm_idmap* map, rm_crdt_id key, uint64_t value);

/* The value under `key`, or false when there is none. */
bool rm_idmap_get(const rm_idmap* map, rm_crdt_id key, uint64_t* out_value);

/* Whether `key` is in the map. */
bool rm_idmap_contains(const rm_idmap* map, rm_crdt_id key);

/* Take the entry out and answer what it held. Used, among other things, to
 * mark a bucket spent. */
bool rm_idmap_take(rm_idmap* map, rm_crdt_id key, uint64_t* out_value);

/* Walk every entry. `*cursor` starts at 0 and is opaque. */
bool rm_idmap_next(const rm_idmap* map, size_t* cursor, rm_crdt_id* out_key,
                   uint64_t* out_value);

/* A map from a 16-byte opaque key to a 64-bit value.
 *
 * The attachment keys are not CRDT identifiers and do not fit `rm_idmap`:
 * they are sixteen opaque bytes the document's index hands out. A map and not
 * a scan for the same reason the identifiers have one — the page asks it once
 * per attachment while the list is being built and once per image afterwards,
 * and the number of attachments is the file's to choose. */
typedef struct rm_keymap {
  struct rm_keymap_slot* slots;
  size_t capacity;
  size_t count;
} rm_keymap;

void rm_keymap_init(rm_keymap* map);

/* Release the backing storage and leave the map empty. */
void rm_keymap_free(rm_keymap* map);

/* Set or replace the value under `key`. False on allocation failure, and then
 * the map is untouched. */
[[nodiscard]] bool rm_keymap_set(rm_keymap* map, const uint8_t key[16],
                                 uint64_t value);

/* The value under `key`, or false when there is none. */
bool rm_keymap_get(const rm_keymap* map, const uint8_t key[16],
                   uint64_t* out_value);

/* ------------------------------------------------------------------ */
/* Grouping                                                            */
/* ------------------------------------------------------------------ */

/* Items grouped by an identifier, in a compressed-row layout: one array of
 * item indexes and, per group, where that group's run begins and ends.
 *
 * Order inside a group is the order the items were seen, and the chain walk
 * depends on it, because file order is the tie-break of last resort. */
typedef struct rm_grouping {
  rm_idmap index;

  /* group number -> [start, end) in `items` */
  uint32_t* starts;
  uint32_t* ends;
  uint32_t* items;
  size_t group_count;
  size_t item_count;
} rm_grouping;

/* Group `count` items, `key_of(i, ctx)` naming each one's group. */
[[nodiscard]] bool rm_grouping_build(rm_grouping* grouping, size_t count,
                                     rm_crdt_id (*key_of)(size_t index,
                                                          void* ctx),
                                     void* ctx);

/* Release the grouping and everything built for it. */
void rm_grouping_free(rm_grouping* grouping);

/* The run of items in the group named by `key`, or false when there is none. */
bool rm_grouping_find(const rm_grouping* grouping, rm_crdt_id key,
                      const uint32_t** out_items, size_t* out_count);

/* The group's number, for the callers that keep a cursor or a spent flag per
 * group rather than per key. */
bool rm_grouping_number(const rm_grouping* grouping, rm_crdt_id key,
                        size_t* out_number);

#endif /* RM_SUPPORT_H */
