/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What a page that will not wholly parse does. The rule the library is built
 * on is that an unreadable piece is a piece and not a page: the rest is kept,
 * the loss is counted, and the count is said out loud. Tested here because it
 * is the difference between a notebook that opens short and a notebook that
 * does not open — and because a loss that is not counted is the failure that
 * is found months later in somebody's page. */

#include <stdlib.h>
#include <time.h>

#include "rm_damaged.h"
#include "rm_internal.h"
#include "rm_test.h"
#include "rmlines/rmlines.h"

static const uint8_t AUTHOR[16] = {0x0a, 0x41, 0x8e, 0x1a, 0x11, 0x11,
                                   0x42, 0x22, 0x83, 0x33, 0x44, 0x44,
                                   0x55, 0x55, 0x66, 0x66};

/* A block envelope as the format writes one: the payload's length, three
 * version bytes, the type, then the payload. */
static void append_block(rm_buf* out, uint8_t type, const uint8_t* payload,
                         size_t length) {
  rm_write_top_block(out, type, 1, 1, payload, length);
}

static uint8_t* page_with_one_stroke(size_t* out_size) {
  rm_draft_paragraph paragraphs[] = {{.text = "Body"}};
  rm_draft_point points[] = {rm_draft_point_make(0, 0),
                             rm_draft_point_make(10, 0)};
  rm_draft_stroke strokes[] = {{.tool = RM_TOOL_FINELINER_2,
                                .color = RM_COLOR_BLACK,
                                .thickness = 2,
                                .points = points,
                                .point_count = 2}};
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .paragraphs = paragraphs,
      .paragraph_count = 1,
      .strokes = strokes,
      .stroke_count = 1,
  };
  memcpy(draft.author, AUTHOR, 16);
  uint8_t* bytes = nullptr;
  rm_error error;
  if (rm_write_page(&draft, &bytes, out_size, &error) != RM_OK) return nullptr;
  return bytes;
}

/* Past the 43-byte header, every cut lands inside a block: the length a block
 * declares does not fit what is left, and believing it would be reading
 * somebody else's bytes as this page's. */
static void truncation_is_an_error(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture page writes");
  if (whole == nullptr) return;

  const size_t cuts[] = {60, size / 2, size - 1};
  for (size_t i = 0; i < 3; i += 1) {
    rm_page* page = nullptr;
    rm_error error;
    rm_status status = rm_parse_page(whole, cuts[i], &page, &error);
    RM_CHECK(status == RM_ERR_UNEXPECTED_EOF,
             "a cut at %zu is refused (got %s)", cuts[i],
             rm_status_string(status));
    if (status == RM_OK) rm_page_free(page);
  }
  rm_bytes_free(whole);
}

static void block_length_past_the_end_is_an_error(void) {
  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  const uint8_t header[] = {0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x01, 0x01, 0x05};
  rm_buf_append(&file, header, sizeof header);

  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
  RM_CHECK(status == RM_ERR_UNEXPECTED_EOF,
           "a block that claims the world is refused");
  if (status == RM_OK) rm_page_free(page);
  rm_buf_free(&file);
}

static void a_bare_header_parses(void) {
  rm_page* page = nullptr;
  rm_error error;
  rm_status status =
      rm_parse_page((const uint8_t*)RM_HEADER, RM_HEADER_LENGTH, &page, &error);
  RM_CHECK(status == RM_OK,
           "a header and nothing else is an empty page, not a failure");
  if (status != RM_OK) return;
  RM_CHECK_EQ_SIZE(page->stroke_count, 0, "no strokes");
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "and nothing was skipped");
  RM_CHECK(!page->has_text, "and no text");
  rm_page_free(page);
}

/* A page of an older generation is refused by its version, and bytes of no
 * known shape as a format nobody can name. The answers differ: an application
 * tells a user "an older format" only where it has read a version. */
static void older_pages_are_refused_by_version(void) {
  for (int version = 3; version <= 5; version += 2) {
    char header[64];
    snprintf(header, sizeof header, "%s%d          ", RM_HEADER_PREFIX,
             version);
    rm_page* page = nullptr;
    rm_error error;
    rm_status status =
        rm_parse_page((const uint8_t*)header, strlen(header), &page, &error);
    RM_CHECK(status == RM_ERR_UNSUPPORTED_VERSION, "version %d is named",
             version);
    RM_CHECK(error.version == version, "and the number travels (%d)",
             error.version);
    if (status == RM_OK) rm_page_free(page);
  }

  const char strange[] = "reMarkable lines with selections and layers ";
  rm_page* page = nullptr;
  rm_error error;
  rm_status status =
      rm_parse_page((const uint8_t*)strange, strlen(strange), &page, &error);
  RM_CHECK(status == RM_ERR_UNSUPPORTED_FORMAT,
           "bytes we cannot name stay unnamed");
  RM_CHECK_EQ_STR(error.message, strlen(error.message),
                  "reMarkable lines with selections",
                  "and the first 32 of them travel");
  if (status == RM_OK) rm_page_free(page);
}

/* A type the format does not define. Counted, because the day one appears is
 * the day this library is behind and has to say so. */
static void an_unknown_block_is_counted(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture page writes");
  if (whole == nullptr) return;

  rm_page* intact = nullptr;
  rm_error error;
  RM_CHECK(rm_parse_page(whole, size, &intact, &error) == RM_OK, "it parses");
  RM_CHECK_EQ_SIZE(intact->skipped_blocks, 0,
                   "the writer's own page holds nothing we cannot read");
  size_t strokes = intact->stroke_count;
  rm_page_free(intact);

  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, whole, size);
  const uint8_t payload[] = {1, 2, 3, 4};
  append_block(&file, 0x7F, payload, sizeof payload);

  rm_page* page = nullptr;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &page, &error) == RM_OK,
           "and still parses");
  RM_CHECK_EQ_SIZE(page->stroke_count, strokes, "the rest of the page is kept");
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 1, "and the block is counted");
  rm_page_free(page);
  rm_buf_free(&file);
  rm_bytes_free(whole);
}

/* A stroke block whose payload is not a stroke: the preamble's first tag is
 * wrong, so the line parser fails before it has a point. */
static void an_unreadable_stroke_is_counted(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture page writes");
  if (whole == nullptr) return;

  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, whole, size);
  uint8_t payload[24] = {0};
  append_block(&file, 0x05, payload, sizeof payload);

  rm_page* page = nullptr;
  rm_error error;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &page, &error) == RM_OK,
           "the page survives");
  RM_CHECK_EQ_SIZE(page->stroke_count, 1,
                   "the stroke that did parse is still on the page");
  RM_CHECK_EQ_SIZE(page->skipped_blocks, 1,
                   "and the one that did not is counted");
  rm_page_free(page);
  rm_buf_free(&file);
  rm_bytes_free(whole);
}

/* A page built to be hostile rather than broken: it parses, and the question
 * is what it costs. Every text record names the same `left` and the same `id`
 * and carries no characters, so the ordering walk comes back to one tie on
 * every record of the page.
 *
 * A walk quadratic in the ties is quadratic in a count the file chooses. A
 * `.rm` arrives from a device or a sync service and is parsed inside an
 * application, in a task nothing interrupts, so that is not a slow parse but a
 * hang somebody else gets to choose. */
static void build_tied_text_page(rm_buf* file, size_t records) {
  rm_buf rows, list, container, text, position;
  rm_buf_init(&rows);
  rm_buf_init(&list);
  rm_buf_init(&container);
  rm_buf_init(&text);
  rm_buf_init(&position);

  rm_write_varuint(&rows, (uint64_t)records);
  for (size_t i = 0; i < records; i += 1) {
    rm_buf body, value;
    rm_buf_init(&body);
    rm_buf_init(&value);
    rm_write_id(&body, 2, RM_CRDT_NONE); /* id */
    rm_write_id(&body, 3, RM_CRDT_NONE); /* left — the walk's own start */
    rm_write_id(&body, 4, RM_CRDT_NONE); /* right */
    rm_write_tag(&body, 5, RM_TAG_BYTE4);
    rm_write_u32(&body, 0); /* nothing deleted: span 0 */
    rm_write_string(&value, "");
    rm_write_block(&body, 6, value.bytes, value.count);
    rm_write_block(&rows, 0, body.bytes, body.count);
    rm_buf_free(&body);
    rm_buf_free(&value);
  }
  rm_write_block(&list, 1, rows.bytes, rows.count);
  rm_write_block(&container, 1, list.bytes, list.count);

  rm_write_id(&text, 1, rm_crdt_make(1, 1));
  rm_write_block(&text, 2, container.bytes, container.count);
  rm_write_f64(&position, -468);
  rm_write_f64(&position, 234);
  rm_write_block(&text, 3, position.bytes, position.count);

  rm_buf_init(file);
  rm_buf_append(file, RM_HEADER, RM_HEADER_LENGTH);
  rm_write_top_block(file, 0x07, 1, 1, text.bytes, text.count);

  rm_buf_free(&rows);
  rm_buf_free(&list);
  rm_buf_free(&container);
  rm_buf_free(&text);
  rm_buf_free(&position);
}

static void a_tied_page_does_not_hang(void) {
  constexpr size_t records = 20000;
  rm_buf file;
  build_tied_text_page(&file, records);

  clock_t started = clock();
  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
  double elapsed = (double)(clock() - started) / (double)CLOCKS_PER_SEC;
  RM_CHECK(status == RM_OK, "the hostile page still parses");
  if (status == RM_OK) rm_page_free(page);

  /* Generous on purpose: the deadline catches a quadratic walk, not a slow
   * machine. */
  RM_CHECK(elapsed < 5.0,
           "%zu tied records took %.2fs — the ordering walk has gone quadratic",
           records, elapsed);
  rm_buf_free(&file);
}

/* A list that declares more entries than any object can hold is a corrupted
 * field, not a large list — and the block it sits in has to be reported as one
 * this reader could not take, rather than quietly answered with the empty list
 * the parser had got as far as. The attachment map is where it shows: a page
 * whose map is lost is a page whose pictures cannot be fetched, and nothing
 * else on the page says so. */
static void an_impossible_count_costs_its_block(void) {
  rm_buf body, list;
  rm_buf_init(&body);
  rm_buf_init(&list);
  /* The ten-byte form of UINT64_MAX, where a count belongs. */
  rm_write_varuint(&list, UINT64_MAX);
  rm_write_block(&body, 1, list.bytes, list.count);

  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  append_block(&file, 0x0E, body.bytes, body.count);

  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
  RM_CHECK(status == RM_OK, "the page itself still parses");
  if (status == RM_OK) {
    RM_CHECK_EQ_SIZE(page->skipped_blocks, 1,
                     "and the block is counted as lost");
    rm_page_free(page);
  }
  rm_buf_free(&body);
  rm_buf_free(&list);
  rm_buf_free(&file);
}

/* One entry of an attachment map, as the tablet spells one: the 16-byte key,
 * then the name in a stamped subblock. */
static void append_attachment_entry(rm_buf* entries, const uint8_t key[16],
                                    const char* name) {
  rm_buf entry, stamped, text;
  rm_buf_init(&entry);
  rm_buf_init(&stamped);
  rm_buf_init(&text);
  rm_buf_append(&entry, key, 16);
  rm_write_id(&stamped, 1, (rm_wid){.counter = 0, .author = 0});
  rm_write_string(&text, name);
  rm_write_block(&stamped, 2, text.bytes, text.count);
  rm_write_block(&entry, 1, stamped.bytes, stamped.count);
  rm_write_block(entries, 0, entry.bytes, entry.count);
  rm_buf_free(&entry);
  rm_buf_free(&stamped);
  rm_buf_free(&text);
}

/* The map is read into a hash on the key, not scanned entry by entry — a scan
 * is quadratic in a count the file chooses. What the hash must not change is
 * the rule: a later entry replaces an earlier one with the same key, and two
 * keys that differ stay two attachments.
 *
 * Read back through `rm_keymap` directly because a page draws only the
 * attachments its images name, and the point here is the map itself. */
static void the_attachment_map_keeps_its_rule(void) {
  rm_keymap map;
  rm_keymap_init(&map);

  uint8_t first[16] = {0}, second[16] = {0};
  first[0] = 1;
  second[0] = 1;
  second[15] = 1; /* differs in the last byte alone */

  RM_CHECK(rm_keymap_set(&map, first, 10), "a key goes in");
  RM_CHECK(rm_keymap_set(&map, second, 20),
           "and a key differing by one byte is another");
  uint64_t value = 0;
  RM_CHECK(rm_keymap_get(&map, first, &value) && value == 10,
           "the first is its own entry");
  RM_CHECK(rm_keymap_get(&map, second, &value) && value == 20,
           "and so is the second");

  /* The replacement the format's own merge does. */
  RM_CHECK(rm_keymap_set(&map, first, 30), "a repeated key is accepted");
  RM_CHECK(rm_keymap_get(&map, first, &value) && value == 30,
           "and it replaces what was there");
  RM_CHECK_EQ_SIZE(map.count, 2, "without adding an entry");

  uint8_t absent[16] = {0xFF};
  RM_CHECK(!rm_keymap_get(&map, absent, &value),
           "a key nobody set is not found");

  /* Past the growth threshold and back again: the rehash has to carry every
   * key to its new slot, and a linear probe that loses one answers "absent"
   * for an attachment that is there. */
  for (uint64_t i = 0; i < 500; i += 1) {
    uint8_t key[16] = {0};
    for (int b = 0; b < 8; b += 1) key[b] = (uint8_t)(i >> (b * 8));
    key[15] = 0x5A;
    RM_CHECK(rm_keymap_set(&map, key, i), "entry %llu goes in",
             (unsigned long long)i);
  }
  size_t found = 0;
  for (uint64_t i = 0; i < 500; i += 1) {
    uint8_t key[16] = {0};
    for (int b = 0; b < 8; b += 1) key[b] = (uint8_t)(i >> (b * 8));
    key[15] = 0x5A;
    if (rm_keymap_get(&map, key, &value) && value == i) found += 1;
  }
  RM_CHECK_EQ_SIZE(found, 500, "every entry survives the rehash");
  rm_keymap_free(&map);
}

/* The same rule through a whole page: two entries sharing a key, the later one
 * winning, and a third key standing on its own. A page whose map collapsed two
 * distinct assets would draw one picture twice. */
static void a_repeated_attachment_key_is_replaced(void) {
  uint8_t key_a[16] = {0}, key_b[16] = {0};
  key_a[0] = 0xAA;
  key_b[0] = 0xAA;
  key_b[15] = 0xBB;

  rm_buf entries, body, file;
  rm_buf_init(&entries);
  rm_buf_init(&body);
  rm_buf_init(&file);
  rm_write_varuint(&entries, 3);
  append_attachment_entry(&entries, key_a, "first.png");
  append_attachment_entry(&entries, key_b, "other.png");
  append_attachment_entry(&entries, key_a,
                          "second.png"); /* replaces "first.png" */
  rm_write_block(&body, 1, entries.bytes, entries.count);

  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  append_block(&file, 0x0E, body.bytes, body.count);

  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
  RM_CHECK(status == RM_OK, "the page parses");
  if (status == RM_OK) {
    /* No image item names them, so nothing is drawn — but the block was
     * read, which is what `skipped_blocks` reports on. */
    RM_CHECK_EQ_SIZE(page->skipped_blocks, 0,
                     "and the map is not counted as lost");
    rm_page_free(page);
  }
  rm_buf_free(&entries);
  rm_buf_free(&body);
  rm_buf_free(&file);
}

/* A later attachment map, read against the written image page whose picture
 * is keyed 0…15. A map that runs out early merges what it read; a map with an
 * entry that fails inside merges nothing of itself and is counted. */
static void a_broken_attachment_map_merges_nothing(void) {
  uint8_t key[16];
  for (uint8_t i = 0; i < 16; i += 1) key[i] = i;
  for (int broken = 0; broken < 2; broken += 1) {
    size_t size = 0;
    uint8_t* bytes = rm_damaged_image_page(&size);
    RM_CHECK(bytes != nullptr, "the image page writes");
    if (bytes == nullptr) return;

    rm_buf entries, body, file;
    rm_buf_init(&entries);
    rm_buf_init(&body);
    rm_buf_init(&file);
    rm_write_varuint(&entries, 3);
    append_attachment_entry(&entries, key, "late.png");
    if (broken) {
      /* An entry too short to hold its key. */
      const uint8_t short_key[] = {1, 2, 3};
      rm_write_block(&entries, 0, short_key, sizeof short_key);
    }
    rm_write_block(&body, 1, entries.bytes, entries.count);
    rm_buf_append(&file, bytes, size);
    append_block(&file, 0x0E, body.bytes, body.count);

    rm_page* page = nullptr;
    rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
    RM_CHECK(status == RM_OK, "the page parses (%s)",
             broken ? "broken" : "short");
    if (page != nullptr) {
      RM_CHECK_EQ_SIZE(page->image_count, 1, "the picture is drawn");
      if (page->image_count == 1) {
        const char* expected =
            broken ? "0a418e1a-1111-4222-8333-444455556666.png" : "late.png";
        RM_CHECK_EQ_STR(page->images[0].file.bytes, page->images[0].file.length,
                        expected, "under the name of the last map that read");
      }
      RM_CHECK_EQ_SIZE(page->skipped_blocks, broken ? 1 : 0,
                       "only the broken map is counted");
      rm_page_free(page);
    }
    rm_buf_free(&entries);
    rm_buf_free(&body);
    rm_buf_free(&file);
    rm_bytes_free(bytes);
  }
}

/* A block whose declared length runs past what the file holds is refused
 * rather than believed. On a machine whose `size_t` is 32 bits the position
 * and the length can each be in range and still carry past the type when
 * added, and the wrapped sum would pass the very check meant to catch it — so
 * the sum is formed only once it is known to fit. */
static void a_length_that_would_wrap_is_refused(void) {
  rm_buf file;
  rm_buf_init(&file);
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  /* An 8-byte envelope claiming very nearly 2^32 bytes of payload. */
  rm_write_u32(&file, 0xFFFFFFF0u);
  rm_buf_append_byte(&file, 0);
  rm_buf_append_byte(&file, 1);
  rm_buf_append_byte(&file, 1);
  rm_buf_append_byte(&file, 0x05);
  for (int i = 0; i < 32; i += 1) rm_buf_append_byte(&file, 0);

  rm_page* page = nullptr;
  rm_error error;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
  RM_CHECK(status == RM_ERR_UNEXPECTED_EOF,
           "a block longer than the file is the end of the parse (got %s)",
           rm_status_string(status));
  if (status == RM_OK) rm_page_free(page);
  rm_buf_free(&file);
}

/* Find one byte pattern in a page and answer where it starts, or SIZE_MAX. */
static size_t find_bytes(const uint8_t* page, size_t size,
                         const uint8_t* pattern, size_t length) {
  for (size_t at = 0; at + length <= size; at += 1) {
    if (memcmp(page + at, pattern, length) == 0) return at;
  }
  return SIZE_MAX;
}

/* A stroke whose thickness is not a number is lost whole and counted, the way
 * a stroke with a NaN coordinate is. The setting multiplies every width of the
 * stroke, so a NaN there would reach the sheet as a NaN line and JSON output as
 * `nan`, which no reader of JSON accepts. */
static void a_nan_thickness_costs_its_stroke(void) {
  size_t size = 0;
  uint8_t* page = page_with_one_stroke(&size);
  RM_CHECK(page != nullptr, "a page with one stroke is written");
  if (page == nullptr) return;

  /* Tag 3 as a byte8 (0x38), then the float64 of 2.0 the draft carries. */
  static const uint8_t thickness[] = {0x38, 0, 0, 0, 0, 0, 0, 0, 0x40};
  size_t at = find_bytes(page, size, thickness, sizeof thickness);
  RM_CHECK(at != SIZE_MAX, "the thickness field is found");
  if (at != SIZE_MAX) {
    static const uint8_t nan64[] = {0, 0, 0, 0, 0, 0, 0xF8, 0x7F};
    memcpy(page + at + 1, nan64, sizeof nan64);
    rm_page* parsed = nullptr;
    rm_error error;
    rm_status status = rm_parse_page(page, size, &parsed, &error);
    RM_CHECK(status == RM_OK, "the page still parses");
    if (status == RM_OK) {
      RM_CHECK_EQ_SIZE(parsed->stroke_count, 0, "the stroke is not drawn");
      RM_CHECK_EQ_SIZE(parsed->skipped_blocks, 1, "and it is counted as lost");
      rm_page_free(parsed);
    }
  }
  rm_bytes_free(page);
}

static void append_preamble(rm_buf* body, rm_crdt_id parent, rm_crdt_id id) {
  rm_write_id(body, 1, parent);
  rm_write_id(body, 2, id);
  rm_write_id(body, 3, RM_CRDT_NONE);
  rm_write_id(body, 4, RM_CRDT_NONE);
  rm_write_tag(body, 5, RM_TAG_BYTE4);
  rm_write_u32(body, 0);
}

static void append_group(rm_buf* file, rm_crdt_id parent, rm_crdt_id child) {
  rm_buf body = {0}, value = {0};
  append_preamble(&body, parent, rm_crdt_make(1, parent.counter + 100));
  rm_buf_append_byte(&value, 2);
  rm_write_id(&value, 2, child);
  rm_write_block(&body, 6, value.bytes, value.count);
  append_block(file, 4, body.bytes, body.count);
  rm_buf_free(&body);
  rm_buf_free(&value);
}

static void append_visibility(rm_buf* file, rm_crdt_id stamp, bool visible) {
  rm_buf body = {0};
  rm_write_id(&body, 1, rm_crdt_make(1, 11));
  rm_write_stamped_u8(&body, 3, stamp, visible);
  append_block(file, 2, body.bytes, body.count);
  rm_buf_free(&body);
}

static void hidden_and_removed_cycles_terminate(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture writes");
  if (whole == nullptr) return;
  for (unsigned removed = 0; removed < 2; removed += 1) {
    for (unsigned indirect = 0; indirect < 2; indirect += 1) {
      rm_buf file = {0};
      rm_buf_append(&file, whole, size);
      if (removed) {
        rm_buf body = {0};
        rm_write_id(&body, 1, rm_crdt_make(1, 200));
        rm_write_id(&body, 2, rm_crdt_make(1, 11));
        rm_write_tag(&body, 3, RM_TAG_BYTE1);
        rm_buf_append_byte(&body, 0);
        append_block(&file, 1, body.bytes, body.count);
        rm_buf_free(&body);
      } else {
        append_visibility(&file, rm_crdt_make(1, 200), false);
      }
      append_group(&file, rm_crdt_make(1, 11),
                   rm_crdt_make(1, indirect ? 12 : 11));
      if (indirect)
        append_group(&file, rm_crdt_make(1, 12), rm_crdt_make(1, 11));
      rm_page* page = nullptr;
      rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
      RM_CHECK(status == RM_OK, "a cyclic hidden/removed subtree terminates");
      if (page != nullptr) {
        RM_CHECK_EQ_SIZE(page->stroke_count, 0, "its strokes stay hidden");
        RM_CHECK_EQ_SIZE(page->image_count, 0,
                         "no picture comes out of the loop");
        RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "and the loop is no loss");
        RM_CHECK(page->has_text, "unrelated content survives");
        rm_page_free(page);
      }
      rm_buf_free(&file);
    }
  }
  rm_bytes_free(whole);
}

static void visibility_uses_the_latest_edit(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture writes");
  if (whole == nullptr) return;
  struct {
    rm_crdt_id stamp;
    bool visible;
    size_t strokes;
  } edits[] = {
      {{.author = 2, .counter = 100}, false, 0},
      {{.author = 1, .counter = 101}, true, 1},
      {{.author = 3, .counter = 99}, false, 1},
      {{.author = 1, .counter = 102}, false, 0},
      {{.author = 2, .counter = 101}, true, 0},
      {{.author = 2, .counter = 102}, true, 1},
      {{.author = 1, .counter = 102}, false, 1},
  };
  rm_buf file = {0};
  rm_buf_append(&file, whole, size);
  for (size_t i = 0; i < sizeof edits / sizeof *edits; i += 1) {
    append_visibility(&file, edits[i].stamp, edits[i].visible);
    rm_page* page = nullptr;
    rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
    RM_CHECK(status == RM_OK, "visibility edit %zu parses", i);
    if (page != nullptr) {
      RM_CHECK_EQ_SIZE(page->stroke_count, edits[i].strokes,
                       "edit %zu respects the newest stamp", i);
      RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "visibility is not data loss");
      rm_page_free(page);
    }
  }
  rm_buf_free(&file);
  rm_bytes_free(whole);
}

/* Modes 0/1/2 put the coordinates outside the top-level block, the value,
 * or the rectangle list respectively. Mode 3 is the valid control. */
static void append_test_highlight(rm_buf* file, unsigned mode) {
  rm_buf body = {0}, value = {0}, rectangles = {0}, text = {0};
  append_preamble(&body, rm_crdt_make(1, 11), rm_crdt_make(1, 50));
  rm_buf_append_byte(&value, 1);
  rm_write_tag(&value, 4, RM_TAG_BYTE4);
  rm_write_u32(&value, 3);
  rm_write_string(&text, "highlight");
  rm_write_block(&value, 5, text.bytes, text.count);
  rm_write_varuint(&rectangles, 1);
  rm_write_f64(&rectangles, 1);
  rm_write_f64(&rectangles, 2);
  rm_write_f64(&rectangles, 3);
  rm_write_f64(&rectangles, 4);
  rm_write_block(&value, 6, rectangles.bytes, mode == 2 ? 1 : rectangles.count);
  if (mode == 2)
    rm_buf_append(&value, rectangles.bytes + 1, rectangles.count - 1);
  rm_write_block(&body, 6, value.bytes,
                 mode == 1 ? value.count - 32 : value.count);
  if (mode == 1) rm_buf_append(&body, rectangles.bytes + 1, 32);
  append_block(file, 3, body.bytes, mode == 0 ? body.count - 32 : body.count);
  if (mode == 0) {
    /* The next block's header and payload must not become a rectangle. */
    append_block(file, 9, rectangles.bytes + 9, 24);
  }
  rm_buf_free(&body);
  rm_buf_free(&value);
  rm_buf_free(&rectangles);
  rm_buf_free(&text);
}

static void highlight_reads_stop_at_each_boundary(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the following page content writes");
  if (whole == nullptr) return;
  for (unsigned mode = 0; mode < 4; mode += 1) {
    rm_buf file = {0};
    rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
    append_test_highlight(&file, mode);
    rm_buf_append(&file, whole + RM_HEADER_LENGTH, size - RM_HEADER_LENGTH);
    rm_page* page = nullptr;
    rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
    RM_CHECK(status == RM_OK, "boundary %u: the rest of the page survives",
             mode);
    if (page != nullptr) {
      RM_CHECK_EQ_SIZE(page->stroke_count, 1,
                       "the following stroke stays intact");
      RM_CHECK_EQ_SIZE(page->highlight_count, mode == 3 ? 1 : 0,
                       "only the complete rectangle is drawn");
      RM_CHECK_EQ_SIZE(page->skipped_blocks, mode == 3 ? 0 : 1,
                       "the malformed highlight is counted");
      rm_page_free(page);
    }
    rm_buf_free(&file);
  }
  rm_bytes_free(whole);
}

/* Fewer bytes than a block header behind the last block are not a block, and
 * not a reason to lose the page before them: it reads whole. */
static void partial_block_headers_are_left_over(void) {
  size_t size = 0;
  uint8_t* whole = page_with_one_stroke(&size);
  RM_CHECK(whole != nullptr, "the fixture writes");
  if (whole == nullptr) return;
  const uint8_t header[] = {100, 0, 0, 0, 0, 1, 1, 5};
  for (size_t prefix = 0; prefix < 2; prefix += 1) {
    for (size_t tail = 1; tail < sizeof header; tail += 1) {
      rm_buf file = {0};
      rm_buf_append(&file, whole, prefix == 0 ? RM_HEADER_LENGTH : size);
      rm_buf_append(&file, header, tail);
      rm_page* page = nullptr;
      rm_error error;
      rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
      RM_CHECK(status == RM_OK, "%zu bytes left over are not a failure (%s)",
               tail, rm_status_string(status));
      if (page != nullptr) {
        RM_CHECK_EQ_SIZE(page->stroke_count, prefix == 0 ? 0 : 1,
                         "the page before them is whole");
        RM_CHECK_EQ_SIZE(page->skipped_blocks, 0,
                         "and nothing is counted lost");
      }
      rm_page_free(page);
      rm_buf_free(&file);
    }
  }
  rm_bytes_free(whole);
}

static void image_vertices_cannot_escape_their_list(void) {
  rm_draft_image image = {.file = "picture.png", .width = 10, .height = 20};
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .images = &image,
      .image_count = 1,
  };
  uint8_t* bytes = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &bytes, &size, nullptr) == RM_OK,
           "the image fixture writes");
  if (bytes == nullptr) return;
  const uint8_t vertices[] = {0x3C, 65, 0, 0, 0, 16};
  size_t at = find_bytes(bytes, size, vertices, sizeof vertices);
  RM_CHECK(at != SIZE_MAX, "the vertex list is found");
  if (at != SIZE_MAX) {
    /* Keep every float in the parent value, but outside the list. */
    bytes[at + 1] = 1;
    rm_page* page = nullptr;
    rm_status status = rm_parse_page(bytes, size, &page, nullptr);
    RM_CHECK(status == RM_OK, "the rest of the page survives");
    if (page != nullptr) {
      RM_CHECK_EQ_SIZE(page->image_count, 0,
                       "an incomplete placement is not drawn");
      RM_CHECK_EQ_SIZE(page->skipped_blocks, 1, "the lost image is counted");
      rm_page_free(page);
    }
  }
  rm_bytes_free(bytes);
}

static void allocation_failure_never_returns_a_shortened_page(void) {
  rm_draft_paragraph paragraph = {.text = "Hello", .bold = true};
  rm_draft_image image = {.file = "picture.png", .width = 10, .height = 20};
  rm_page_draft draft = {
      .sheet = {1404, 1872, 1404, 1872},
      .column = {936, 1},
      .paragraphs = &paragraph,
      .paragraph_count = 1,
      .images = &image,
      .image_count = 1,
  };
  uint8_t* bytes = nullptr;
  size_t size = 0;
  RM_CHECK(rm_write_page(&draft, &bytes, &size, nullptr) == RM_OK,
           "the fixture writes");
  if (bytes == nullptr) return;
  rm_buf file = {0};
  rm_buf_append(&file, bytes, size);
  append_test_highlight(&file, 3);
  bool completed = false;
  for (size_t allocation = 0; allocation < 100; allocation += 1) {
    rm_test_arena_fail_after(allocation);
    rm_page* page = nullptr;
    rm_error error;
    rm_status status = rm_parse_page(file.bytes, file.count, &page, &error);
    bool injected = rm_test_arena_did_fail();
    rm_test_arena_fail_after(SIZE_MAX);
    if (injected) {
      RM_CHECK(status == RM_ERR_OUT_OF_MEMORY && error.status == status,
               "allocation %zu: OOM must reach the caller (got %s)", allocation,
               rm_status_string(status));
      RM_CHECK(page == nullptr, "no shortened page is returned on OOM");
    } else {
      RM_CHECK(status == RM_OK && page != nullptr,
               "the control parse succeeds");
      if (page != nullptr) {
        RM_CHECK(page->has_text && page->text.paragraph_count == 1,
                 "text survives");
        if (page->has_text && page->text.paragraph_count == 1) {
          RM_CHECK_EQ_STR(page->text.paragraphs[0].text.bytes,
                          page->text.paragraphs[0].text.length, "Hello",
                          "all text");
          RM_CHECK_EQ_SIZE(page->text.paragraphs[0].face_count, 1,
                           "format survives");
        }
        RM_CHECK_EQ_SIZE(page->image_count, 1, "the attachment survives");
        RM_CHECK_EQ_SIZE(page->highlight_count, 1, "the highlight survives");
        RM_CHECK_EQ_SIZE(page->skipped_blocks, 0, "the fixture has no losses");
      }
      completed = true;
    }
    rm_page_free(page);
    if (completed) break;
  }
  RM_CHECK(completed, "every arena allocation was exercised");
  rm_buf_free(&file);
  rm_bytes_free(bytes);
}

static void put_u32(uint8_t* at, uint32_t value) {
  for (int i = 0; i < 4; i += 1) at[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get_u32(const uint8_t* at) {
  return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 |
         (uint32_t)at[3] << 24;
}

/* The written image page with `extra` floats of nought appended to its vertex
 * list — the list, the value and the block each grown to hold them. */
static uint8_t* image_page_with_extra_floats(size_t extra, size_t* out_size) {
  size_t size = 0;
  uint8_t* bytes = rm_damaged_image_page(&size);
  size_t start, end;
  if (bytes == nullptr ||
      !rm_damaged_payload(bytes, size, 0x0F, &start, &end)) {
    rm_bytes_free(bytes);
    return nullptr;
  }
  static const uint8_t count[] = {0x3C, 0x41, 0x00, 0x00, 0x00, 0x10};
  size_t list = SIZE_MAX, value = SIZE_MAX;
  for (size_t at = start; at + sizeof count <= end; at += 1) {
    if (list == SIZE_MAX && memcmp(bytes + at, count, sizeof count) == 0)
      list = at;
    if (value == SIZE_MAX && bytes[at] == 0x6C && at + 5 <= end &&
        get_u32(bytes + at + 1) == end - (at + 5)) {
      value = at;
    }
  }
  if (list == SIZE_MAX || value == SIZE_MAX) {
    rm_bytes_free(bytes);
    return nullptr;
  }
  size_t grow = extra * 4u;
  size_t insert_at = list + sizeof count + 64u;
  uint8_t* grown = calloc(size + grow, 1);
  if (grown == nullptr) {
    rm_bytes_free(bytes);
    return nullptr;
  }
  memcpy(grown, bytes, insert_at);
  memcpy(grown + insert_at + grow, bytes + insert_at, size - insert_at);
  grown[list + 5] = (uint8_t)(16u + extra);
  put_u32(grown + list + 1, get_u32(grown + list + 1) + (uint32_t)grow);
  put_u32(grown + value + 1, get_u32(grown + value + 1) + (uint32_t)grow);
  put_u32(grown + start - 8, get_u32(grown + start - 8) + (uint32_t)grow);
  rm_bytes_free(bytes);
  *out_size = size + grow;
  return grown;
}

/* Whole vertices are counted: one to three floats over the sixteen are no
 * fifth corner, and the picture is placed on its four. Four more are a fifth,
 * and there is no placing a picture on five. */
static void floats_short_of_a_fifth_vertex_still_place_the_image(void) {
  for (size_t extra = 1; extra <= 4; extra += 1) {
    size_t size = 0;
    uint8_t* bytes = image_page_with_extra_floats(extra, &size);
    RM_CHECK(bytes != nullptr, "the vertex list grows by %zu", extra);
    if (bytes == nullptr) continue;
    rm_page* page = nullptr;
    rm_status status = rm_parse_page(bytes, size, &page, nullptr);
    RM_CHECK(status == RM_OK, "the page parses with %zu more floats", extra);
    if (page != nullptr) {
      bool fifth = extra == 4;
      RM_CHECK_EQ_SIZE(page->image_count, fifth ? 0 : 1,
                       "%zu more floats: placed only on four corners", extra);
      RM_CHECK_EQ_SIZE(page->skipped_blocks, fifth ? 1 : 0,
                       "%zu more floats: a refused picture is counted", extra);
      rm_page_free(page);
    }
    free(bytes);
  }
}

/* "Loss is numbered, deletion is not." An item whose value is there but not of
 * its shape is not a tombstone — the answer for "the user erased this" — that
 * vanishes from the page with the count untouched. It costs the item, and the
 * cost is counted. */
static void a_malformed_image_item_is_a_counted_loss_not_an_erasure(void) {
  size_t size = 0;
  uint8_t* intact_bytes = rm_damaged_image_page(&size);
  rm_page* intact = nullptr;
  RM_CHECK(intact_bytes != nullptr &&
               rm_parse_page(intact_bytes, size, &intact, nullptr) == RM_OK,
           "the intact page parses");
  if (intact != nullptr) {
    RM_CHECK_EQ_SIZE(intact->image_count, 1, "with its picture");
    RM_CHECK_EQ_SIZE(intact->skipped_blocks, 0, "and nothing lost");
    rm_page_free(intact);
  }
  rm_bytes_free(intact_bytes);

  uint8_t* bytes = rm_damaged_image_with_three_corners(&size);
  RM_CHECK(bytes != nullptr, "the vertex count is found");
  rm_page* page = nullptr;
  if (bytes != nullptr && rm_parse_page(bytes, size, &page, nullptr) == RM_OK) {
    RM_CHECK_EQ_SIZE(page->image_count, 0,
                     "there is no placing a picture on three corners");
    RM_CHECK_EQ_SIZE(page->skipped_blocks, 1,
                     "and the picture that is not there is counted");
    rm_page_free(page);
  } else {
    RM_CHECK(false, "the damaged page still parses");
  }
  rm_bytes_free(bytes);
}

static void a_mistyped_group_item_is_a_counted_loss_and_the_ink_is_kept(void) {
  size_t size = 0;
  uint8_t* bytes = rm_damaged_mistyped_group_item(&size);
  RM_CHECK(bytes != nullptr, "the group item's value is found");
  rm_page* page = nullptr;
  if (bytes != nullptr && rm_parse_page(bytes, size, &page, nullptr) == RM_OK) {
    /* The layer comes unhooked from the tree and its stroke falls back to
     * file order: kept, because order is worth less than the ink. */
    RM_CHECK_EQ_SIZE(page->stroke_count, 1, "the stroke is kept");
    RM_CHECK_EQ_SIZE(page->skipped_blocks, 1, "and the link is counted lost");
    rm_page_free(page);
  } else {
    RM_CHECK(false, "the damaged page still parses");
  }
  rm_bytes_free(bytes);
}

int main(void) {
  a_malformed_image_item_is_a_counted_loss_not_an_erasure();
  floats_short_of_a_fifth_vertex_still_place_the_image();
  a_mistyped_group_item_is_a_counted_loss_and_the_ink_is_kept();
  hidden_and_removed_cycles_terminate();
  visibility_uses_the_latest_edit();
  highlight_reads_stop_at_each_boundary();
  image_vertices_cannot_escape_their_list();
  partial_block_headers_are_left_over();
  allocation_failure_never_returns_a_shortened_page();
  truncation_is_an_error();
  block_length_past_the_end_is_an_error();
  a_bare_header_parses();
  older_pages_are_refused_by_version();
  an_unknown_block_is_counted();
  an_unreadable_stroke_is_counted();
  an_impossible_count_costs_its_block();
  a_tied_page_does_not_hang();
  the_attachment_map_keeps_its_rule();
  a_repeated_attachment_key_is_replaced();
  a_broken_attachment_map_merges_nothing();
  a_length_that_would_wrap_is_refused();
  a_nan_thickness_costs_its_stroke();
  return rm_test_report("recovery");
}
