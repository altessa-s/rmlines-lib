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
      .author = {1},
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
      .author = {1},
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

static void repeated_group_references_scale_linearly(void) {
  rm_buf file = {0};
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id root = rm_crdt_make(0, 1);
  rm_crdt_id child = rm_crdt_make(1, 50000);
  constexpr size_t K = 2000;
  constexpr size_t M = 2000;

  for (size_t i = 0; i < K; ++i) {
    rm_buf group = {0}, val = {0};
    rm_write_id(&group, 1, root);
    rm_write_id(&group, 2, rm_crdt_make(1, 1 + i));
    rm_write_id(&group, 3, RM_CRDT_NONE);
    rm_write_id(&group, 4, RM_CRDT_NONE);
    rm_write_tag(&group, 5, RM_TAG_BYTE4);
    rm_write_u32(&group, 0);
    rm_buf_append_byte(&val, 2);
    rm_write_id(&val, 2, child);
    rm_write_block(&group, 6, val.bytes, val.count);
    append_block(&file, 4, group.bytes, group.count);
    rm_buf_free(&group);
    rm_buf_free(&val);
  }
  for (size_t i = 0; i < M; ++i) {
    rm_buf tomb = {0};
    rm_write_id(&tomb, 1, child);
    rm_write_id(&tomb, 2, rm_crdt_make(2, 1 + i));
    rm_write_id(&tomb, 3, i ? rm_crdt_make(2, i) : RM_CRDT_NONE);
    rm_write_id(&tomb, 4, RM_CRDT_NONE);
    rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
    rm_write_u32(&tomb, 1);
    append_block(&file, 8, tomb.bytes, tomb.count);
    rm_buf_free(&tomb);
  }

  rm_test_reset_chain_order_calls();
  rm_page* page = nullptr;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
  size_t chain_calls = rm_test_chain_order_calls();
  printf("repeated_group_references: chain_calls=%zu (K=%zu, M=%zu)\n",
         chain_calls, K, M);
  RM_CHECK(status == RM_OK && page != nullptr, "page parses");
  RM_CHECK(chain_calls <= 2,
           "repeated group links invoke rm_chain_order once per parent (%zu)",
           chain_calls);
  rm_page_free(page);
  rm_buf_free(&file);
}

static void append_scene_group(rm_buf* file, rm_crdt_id parent, rm_crdt_id id,
                               rm_crdt_id child) {
  rm_buf group = {0}, val = {0};
  rm_write_id(&group, 1, parent);
  rm_write_id(&group, 2, id);
  rm_write_id(&group, 3, RM_CRDT_NONE);
  rm_write_id(&group, 4, RM_CRDT_NONE);
  rm_write_tag(&group, 5, RM_TAG_BYTE4);
  rm_write_u32(&group, 0);
  rm_buf_append_byte(&val, 2);
  rm_write_id(&val, 2, child);
  rm_write_block(&group, 6, val.bytes, val.count);
  append_block(file, 4, group.bytes, group.count);
  rm_buf_free(&group);
  rm_buf_free(&val);
}

static void append_scene_stroke(rm_buf* file, rm_crdt_id parent, rm_crdt_id id,
                                rm_crdt_id left, rm_crdt_id right,
                                rm_pen_tool tool) {
  rm_draft_point pt = rm_draft_point_make(0, 0);
  rm_draft_stroke stroke = rm_draft_stroke_make(tool, RM_COLOR_YELLOW, &pt, 1);
  if (!rm_pen_tool_is_highlighter(tool)) stroke.color = RM_COLOR_BLACK;
  rm_buf body = {0};
  rm_write_line_item(&body, &stroke, parent, id, left, right);
  rm_write_top_block(file, RM_BLOCK_SCENE_LINE_ITEM, 2, 2, body.bytes,
                     body.count);
  rm_buf_free(&body);
}

static void duplicate_tombstone_runs_do_not_split_quadratically(void) {
  rm_buf file = {0};
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id root = rm_crdt_make(0, 1);
  rm_crdt_id child = rm_crdt_make(1, 50000);
  append_scene_group(&file, root, rm_crdt_make(1, 1), child);

  /* Part 1: M = 500 overlapping [1, 1000) deleted spans on author 2 x K = 200
   * interior cuts -> component exceeds comp_budget cleanly, returning RM_OK
   * with skipped_blocks >= 1 and RM_ERR_INCOMPLETE_PAGE from
   * rm_highlighter_items. */
  constexpr size_t M = 500;
  constexpr size_t K = 200;
  for (size_t i = 0; i < M; ++i) {
    rm_buf tomb = {0};
    rm_write_id(&tomb, 1, child);
    rm_write_id(&tomb, 2, rm_crdt_make(2, 1));
    rm_write_id(&tomb, 3, RM_CRDT_NONE);
    rm_write_id(&tomb, 4, RM_CRDT_NONE);
    rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
    rm_write_u32(&tomb, 1000);
    append_block(&file, 8, tomb.bytes, tomb.count);
    rm_buf_free(&tomb);
  }
  for (size_t k = 0; k < K; ++k) {
    rm_buf ref = {0};
    rm_write_id(&ref, 1, child);
    rm_write_id(&ref, 2, rm_crdt_make(3, 1 + k));
    rm_write_id(&ref, 3, rm_crdt_make(2, 2 + k * 2));
    rm_write_id(&ref, 4, rm_crdt_make(2, 4 + k * 2));
    rm_write_tag(&ref, 5, RM_TAG_BYTE4);
    rm_write_u32(&ref, 1);
    append_block(&file, 8, ref.bytes, ref.count);
    rm_buf_free(&ref);
  }

  rm_test_reset_chain_order_calls();
  rm_page* page = nullptr;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
  RM_CHECK(status == RM_OK && page != nullptr && page->skipped_blocks >= 1,
           "excessive overlapping tombstones parse with skipped_blocks >= 1");
  rm_page_free(page);
  rm_item_id* ids = (rm_item_id*)0x1;
  size_t count = 99;
  RM_CHECK(rm_highlighter_items(file.bytes, file.count, &ids, &count,
                                nullptr) == RM_ERR_INCOMPLETE_PAGE &&
               ids == (rm_item_id*)0x1 && count == 99,
           "rm_highlighter_items reports incomplete page with untouched outs");
  rm_buf_free(&file);

  /* Part 2: repeated duplicate boundary cuts keep rm_test_run_split_visits()
   * O(unique_cuts), not quadratic (R45). */
  constexpr size_t DUPS = 400;
  text_item rows[1 + DUPS];
  rows[0] = (text_item){
      .id = rm_crdt_make(1, 1),
      .left = RM_CRDT_NONE,
      .right = RM_CRDT_NONE,
      .deleted = 1000,
  };
  for (size_t i = 0; i < DUPS; ++i) {
    rows[1 + i] = (text_item){
        .id = rm_crdt_make(2, 1 + i),
        .left = rm_crdt_make(1, 500),
        .right = rm_crdt_make(1, 501),
        .deleted = 1,
    };
  }
  rm_vec parts;
  rm_vec_init(&parts, sizeof(text_item));
  rm_test_reset_chain_order_calls();
  rm_status split_status = rm_split_text_runs_checked(rows, 1 + DUPS, &parts);
  RM_CHECK(split_status == RM_OK && parts.count == DUPS + 2 &&
               rm_test_run_split_visits() <= 8,
           "repeated boundary cuts stay O(unique_cuts) (visits=%zu)",
           rm_test_run_split_visits());
  rm_vec_free(&parts);
}

static void cross_author_and_uint64_max_tombstone_cuts(void) {
  rm_buf file = {0};
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id layer = rm_crdt_make(1, 10);
  append_scene_group(&file, rm_crdt_make(0, 1), rm_crdt_make(1, 5), layer);

  /* Author-2 tombstones cut only by 26 distinct author-3 external cuts up to
   * UINT64_MAX. */
  rm_buf tomb = {0};
  rm_write_id(&tomb, 1, layer);
  rm_write_id(&tomb, 2, rm_crdt_make(2, 100));
  rm_write_id(&tomb, 3, RM_CRDT_NONE);
  rm_write_id(&tomb, 4, RM_CRDT_NONE);
  rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
  rm_write_u32(&tomb, 100);
  append_block(&file, 8, tomb.bytes, tomb.count);
  rm_buf_free(&tomb);

  for (size_t k = 0; k < 25; ++k) {
    append_scene_stroke(&file, layer, rm_crdt_make(3, 25 - k),
                        rm_crdt_make(2, 102 + k * 3),
                        rm_crdt_make(2, 104 + k * 3), RM_TOOL_FINELINER_2);
  }
  /* Tombstone starting at UINT64_MAX - 1 with deleted = 3 (clamped to
   * UINT64_MAX) placed after author-2 chain and cut at UINT64_MAX by an
   * external left reference. */
  rm_buf max_tomb = {0};
  rm_write_id(&max_tomb, 1, layer);
  rm_write_id(&max_tomb, 2, rm_crdt_make(4, UINT64_MAX - 1u));
  rm_write_id(&max_tomb, 3, rm_crdt_make(2, 199));
  rm_write_id(&max_tomb, 4, RM_CRDT_NONE);
  rm_write_tag(&max_tomb, 5, RM_TAG_BYTE4);
  rm_write_u32(&max_tomb, 3);
  append_block(&file, 8, max_tomb.bytes, max_tomb.count);
  rm_buf_free(&max_tomb);

  append_scene_stroke(&file, layer, rm_crdt_make(5, 1),
                      rm_crdt_make(4, UINT64_MAX), RM_CRDT_NONE,
                      RM_TOOL_FINELINER_2);
  append_scene_stroke(&file, layer, rm_crdt_make(5, 2),
                      rm_crdt_make(4, UINT64_MAX - 1u),
                      rm_crdt_make(4, UINT64_MAX), RM_TOOL_FINELINER_2);

  rm_page* page = nullptr;
  rm_status status = rm_parse_page(file.bytes, file.count, &page, nullptr);
  RM_CHECK(status == RM_OK && page != nullptr && page->skipped_blocks == 0 &&
               page->stroke_count == 27,
           "external cuts and UINT64_MAX tombstones parse cleanly");
  if (page != nullptr && page->stroke_count == 27) {
    bool ordered =
        page->strokes[25].id.author == 5 && page->strokes[25].id.counter == 2 &&
        page->strokes[26].id.author == 5 && page->strokes[26].id.counter == 1;
    for (size_t k = 0; k < 25; ++k) {
      if (page->strokes[k].id.author != 3 ||
          page->strokes[k].id.counter != 25 - k)
        ordered = false;
    }
    RM_CHECK(ordered, "external cuts and UINT64_MAX endpoints order strokes");
  }
  rm_page_free(page);
  rm_buf_free(&file);
}

static void interleaved_parent_highlighters_and_component_isolation(void) {
  /* Complete interleaved parents + multi-item split tombstone. */
  rm_buf complete = {0};
  rm_buf_append(&complete, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id l1 = rm_crdt_make(1, 10), l2 = rm_crdt_make(1, 20);
  append_scene_group(&complete, rm_crdt_make(0, 1), rm_crdt_make(1, 1), l1);
  append_scene_group(&complete, rm_crdt_make(0, 1), rm_crdt_make(1, 2), l2);
  append_scene_stroke(&complete, l1, rm_crdt_make(1, 31), RM_CRDT_NONE,
                      RM_CRDT_NONE, RM_TOOL_HIGHLIGHTER_1);
  append_scene_stroke(&complete, l2, rm_crdt_make(1, 32), RM_CRDT_NONE,
                      RM_CRDT_NONE, RM_TOOL_HIGHLIGHTER_1);
  append_scene_stroke(&complete, l1, rm_crdt_make(1, 33), rm_crdt_make(1, 31),
                      RM_CRDT_NONE, RM_TOOL_HIGHLIGHTER_1);
  rm_buf tomb = {0}, cut = {0};
  rm_write_id(&tomb, 1, l1);
  rm_write_id(&tomb, 2, rm_crdt_make(1, 80));
  rm_write_id(&tomb, 3, RM_CRDT_NONE);
  rm_write_id(&tomb, 4, RM_CRDT_NONE);
  rm_write_tag(&tomb, 5, RM_TAG_BYTE4);
  rm_write_u32(&tomb, 2);
  append_block(&complete, 8, tomb.bytes, tomb.count);
  rm_write_id(&cut, 1, l2);
  rm_write_id(&cut, 2, rm_crdt_make(1, 82));
  rm_write_id(&cut, 3, rm_crdt_make(1, 80));
  rm_write_id(&cut, 4, rm_crdt_make(1, 81));
  rm_write_tag(&cut, 5, RM_TAG_BYTE4);
  rm_write_u32(&cut, 1);
  append_block(&complete, 8, cut.bytes, cut.count);
  rm_buf_free(&tomb);
  rm_buf_free(&cut);

  rm_item_id* ids = nullptr;
  size_t count = 0;
  RM_CHECK(rm_highlighter_items(complete.bytes, complete.count, &ids, &count,
                                nullptr) == RM_OK &&
               count == 3 && ids[0].counter == 31 && ids[1].counter == 32 &&
               ids[2].counter == 33,
           "interleaved-parent highlighter items preserve exact file order");
  rm_item_ids_free(ids);
  rm_buf_free(&complete);

  /* Malformed overlapping cluster on the SAME layer as valid interior cuts
   * (201 before 200 via 101/102 in deleted 100..103). */
  rm_buf isolated = {0};
  rm_buf_append(&isolated, RM_HEADER, RM_HEADER_LENGTH);
  append_scene_group(&isolated, rm_crdt_make(0, 1), rm_crdt_make(1, 1), l1);
  append_scene_stroke(&isolated, l1, rm_crdt_make(1, 200), rm_crdt_make(1, 102),
                      RM_CRDT_NONE, RM_TOOL_HIGHLIGHTER_1);
  append_scene_stroke(&isolated, l1, rm_crdt_make(1, 201), rm_crdt_make(1, 101),
                      rm_crdt_make(1, 102), RM_TOOL_HIGHLIGHTER_1);
  rm_buf good_t = {0};
  rm_write_id(&good_t, 1, l1);
  rm_write_id(&good_t, 2, rm_crdt_make(1, 100));
  rm_write_id(&good_t, 3, RM_CRDT_NONE);
  rm_write_id(&good_t, 4, RM_CRDT_NONE);
  rm_write_tag(&good_t, 5, RM_TAG_BYTE4);
  rm_write_u32(&good_t, 4);
  append_block(&isolated, 8, good_t.bytes, good_t.count);
  rm_buf_free(&good_t);

  for (size_t i = 0; i < 40; ++i) {
    rm_buf bad_t = {0};
    rm_write_id(&bad_t, 1, l1);
    rm_write_id(&bad_t, 2, rm_crdt_make(2, 1));
    rm_write_id(&bad_t, 3, RM_CRDT_NONE);
    rm_write_id(&bad_t, 4, RM_CRDT_NONE);
    rm_write_tag(&bad_t, 5, RM_TAG_BYTE4);
    rm_write_u32(&bad_t, 500);
    append_block(&isolated, 8, bad_t.bytes, bad_t.count);
    rm_buf_free(&bad_t);
  }
  for (size_t k = 0; k < 200; ++k) {
    rm_buf bad_c = {0};
    rm_write_id(&bad_c, 1, l1);
    rm_write_id(&bad_c, 2, rm_crdt_make(3, 1 + k));
    rm_write_id(&bad_c, 3, rm_crdt_make(2, 2 + k * 2));
    rm_write_id(&bad_c, 4, rm_crdt_make(2, 4 + k * 2));
    rm_write_tag(&bad_c, 5, RM_TAG_BYTE4);
    rm_write_u32(&bad_c, 1);
    append_block(&isolated, 8, bad_c.bytes, bad_c.count);
    rm_buf_free(&bad_c);
  }

  rm_page* p = nullptr;
  RM_CHECK(
      rm_parse_page(isolated.bytes, isolated.count, &p, nullptr) == RM_OK &&
          p != nullptr && p->skipped_blocks >= 1 && p->stroke_count == 2 &&
          p->strokes[0].id.counter == 201 && p->strokes[1].id.counter == 200,
      "malformed component on same parent preserves sibling [201, 200]");
  rm_page_free(p);

  rm_item_id* out_ids = (rm_item_id*)0x1;
  size_t out_count = 77;
  RM_CHECK(
      rm_highlighter_items(isolated.bytes, isolated.count, &out_ids, &out_count,
                           nullptr) == RM_ERR_INCOMPLETE_PAGE &&
          out_ids == (rm_item_id*)0x1 && out_count == 77,
      "incomplete isolated page leaves rm_highlighter_items outs untouched");
  rm_buf_free(&isolated);
}

static void root_text_fragment_budget_isolates_unrelated_strokes(void) {
  constexpr size_t ROWS = 40;
  constexpr size_t CUTS = 200;
  text_item direct[ROWS + CUTS];
  for (size_t i = 0; i < ROWS; ++i) {
    direct[i] = (text_item){
        .id = rm_crdt_make(2, 1),
        .left = RM_CRDT_NONE,
        .right = RM_CRDT_NONE,
        .deleted = 500,
    };
  }
  for (size_t k = 0; k < CUTS; ++k) {
    direct[ROWS + k] = (text_item){
        .id = rm_crdt_make(3, 1 + k),
        .left = rm_crdt_make(2, 2 + k * 2),
        .right = rm_crdt_make(2, 4 + k * 2),
        .deleted = 1,
    };
  }
  rm_vec direct_parts;
  rm_vec_init(&direct_parts, sizeof(text_item));
  RM_CHECK(rm_split_text_runs_checked(direct, ROWS + CUTS, &direct_parts) ==
               RM_ERR_MALFORMED_PAGE,
           "direct rm_split_text_runs_checked rejects excessive fragments");
  rm_vec_free(&direct_parts);

  rm_buf file = {0}, list = {0}, inner = {0}, items = {0}, root = {0},
         pos = {0};
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id layer = rm_crdt_make(1, 10);
  append_scene_group(&file, rm_crdt_make(0, 1), rm_crdt_make(1, 5), layer);
  append_scene_stroke(&file, layer, rm_crdt_make(1, 50), RM_CRDT_NONE,
                      RM_CRDT_NONE, RM_TOOL_FINELINER_2);

  rm_write_varuint(&list, ROWS + CUTS);
  for (size_t i = 0; i < ROWS; ++i) {
    rm_buf r = {0};
    rm_write_id(&r, 2, rm_crdt_make(2, 1));
    rm_write_id(&r, 3, RM_CRDT_NONE);
    rm_write_id(&r, 4, RM_CRDT_NONE);
    rm_write_tag(&r, 5, RM_TAG_BYTE4);
    rm_write_u32(&r, 500);
    rm_write_block(&list, 0, r.bytes, r.count);
    rm_buf_free(&r);
  }
  for (size_t k = 0; k < CUTS; ++k) {
    rm_buf r = {0};
    rm_write_id(&r, 2, rm_crdt_make(3, 1 + k));
    rm_write_id(&r, 3, rm_crdt_make(2, 2 + k * 2));
    rm_write_id(&r, 4, rm_crdt_make(2, 4 + k * 2));
    rm_write_tag(&r, 5, RM_TAG_BYTE4);
    rm_write_u32(&r, 1);
    rm_write_block(&list, 0, r.bytes, r.count);
    rm_buf_free(&r);
  }
  rm_write_block(&inner, 1, list.bytes, list.count);
  rm_write_block(&items, 1, inner.bytes, inner.count);
  rm_write_id(&root, 1, RM_CRDT_NONE);
  rm_write_block(&root, 2, items.bytes, items.count);
  rm_write_f64(&pos, -468.0);
  rm_write_f64(&pos, 234.0);
  rm_write_block(&root, 3, pos.bytes, pos.count);
  rm_write_top_block(&file, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

  rm_page* p = nullptr;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &p, nullptr) == RM_OK &&
               p != nullptr && p->skipped_blocks >= 1 && p->stroke_count == 1 &&
               !p->has_text,
           "over-budget RootText is skipped while preserving stroke");
  size_t skipped = p ? p->skipped_blocks : 0;
  rm_page_free(p);

  rm_edit_session* s = (rm_edit_session*)0x1;
  rm_error err = {0};
  RM_CHECK(rm_edit_open(file.bytes, file.count, &s, &err) ==
                   RM_ERR_INCOMPLETE_PAGE &&
               s == nullptr && (size_t)err.value == skipped,
           "rm_edit_open refuses incomplete page");
  rmi_edit_candidate cand = {.bytes = nullptr, .size = 0};
  rm_error rerr = {0};
  RM_CHECK(rmi_edit_replace_text(file.bytes, file.count, 0, 0, "hi",
                                 (rm_item_id){1, 900}, &cand,
                                 &rerr) == RM_ERR_INCOMPLETE_PAGE &&
               (size_t)rerr.value == skipped && cand.bytes == nullptr,
           "rmi_edit_replace_text refuses incomplete RootText with value=%zu",
           skipped);
  rm_buf_free(&pos);
  rm_buf_free(&root);
  rm_buf_free(&items);
  rm_buf_free(&inner);
  rm_buf_free(&list);
  rm_buf_free(&file);
}

static void a2_deleted_glyph_and_undo_tombstone_envelopes_and_missing_tag6(
    void) {
  /* 1) Deleted 0x03 glyph: valid opaque tag 6 succeeds vs overlong tag 6. */
  rm_buf base = {0};
  rm_buf_append(&base, RM_HEADER, RM_HEADER_LENGTH);
  rm_crdt_id layer = rm_crdt_make(1, 10);
  append_scene_group(&base, rm_crdt_make(0, 1), rm_crdt_make(1, 5), layer);

  for (int overlong = 0; overlong < 2; ++overlong) {
    rm_buf b = {0}, g = {0};
    rm_buf_append(&b, base.bytes, base.count);
    rm_write_id(&g, 1, layer);
    rm_write_id(&g, 2, rm_crdt_make(1, 30));
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 1);
    const uint8_t junk[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    rm_write_block(&g, 6, junk, sizeof junk);
    if (overlong) g.bytes[g.count - 8] = 10; /* overclaims past end */
    rm_write_top_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, g.bytes, g.count);

    rm_page* p = nullptr;
    rm_status st = rm_parse_page(b.bytes, b.count, &p, nullptr);
    RM_CHECK(st == RM_OK && p != nullptr &&
                 p->skipped_blocks == (size_t)overlong &&
                 p->highlight_count == 0,
             "deleted glyph overlong=%d skipped=%zu", overlong,
             p ? p->skipped_blocks : 0);
    rm_page_free(p);
    rm_item_id* ids = nullptr;
    size_t cnt = 0;
    RM_CHECK(rm_highlighter_items(b.bytes, b.count, &ids, &cnt, nullptr) ==
                 (overlong ? RM_ERR_INCOMPLETE_PAGE : RM_OK),
             "rm_highlighter_items matches overlong=%d", overlong);
    rm_item_ids_free(ids);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) ==
                 (overlong ? RM_ERR_INCOMPLETE_PAGE : RM_OK),
             "rm_edit_open matches overlong=%d", overlong);
    rm_edit_close(s);
    rm_buf_free(&g);
    rm_buf_free(&b);
  }

  /* 2) Live 0x03 glyph discriminator mutated 1 -> 3 (R139, RV46-10). */
  {
    rm_buf b = {0}, g = {0}, val = {0}, tbuf = {0}, rects = {0};
    rm_buf_append(&b, base.bytes, base.count);
    rm_buf_append_byte(&val, RM_VALUE_GLYPH_ITEM);
    rm_write_tag(&val, 4, RM_TAG_BYTE4);
    rm_write_u32(&val, RM_COLOR_YELLOW);
    rm_write_string(&tbuf, "hi");
    rm_write_block(&val, 5, tbuf.bytes, tbuf.count);
    rm_write_varuint(&rects, 1);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 0);
    rm_write_f64(&rects, 10);
    rm_write_f64(&rects, 10);
    rm_write_block(&val, 6, rects.bytes, rects.count);
    rm_write_id(&g, 1, layer);
    rm_write_id(&g, 2, rm_crdt_make(1, 31));
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 0);
    rm_write_block(&g, 6, val.bytes, val.count);
    size_t clean_sz = b.count;
    rm_write_top_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, g.bytes, g.count);
    rm_page* clean = nullptr;
    RM_CHECK(rm_parse_page(b.bytes, b.count, &clean, nullptr) == RM_OK &&
                 clean != nullptr && clean->skipped_blocks == 0 &&
                 clean->highlight_count == 1,
             "control glyph parses cleanly before discriminator mutation");
    rm_page_free(clean);

    val.bytes[0] = 0x03; /* Only the discriminator changes. */
    b.count = clean_sz;
    g.count = 0;
    rm_write_id(&g, 1, layer);
    rm_write_id(&g, 2, rm_crdt_make(1, 31));
    rm_write_id(&g, 3, RM_CRDT_NONE);
    rm_write_id(&g, 4, RM_CRDT_NONE);
    rm_write_tag(&g, 5, RM_TAG_BYTE4);
    rm_write_u32(&g, 0);
    rm_write_block(&g, 6, val.bytes, val.count);
    rm_write_top_block(&b, RM_BLOCK_SCENE_GLYPH_ITEM, 1, 1, g.bytes, g.count);
    rm_page* p = nullptr;
    RM_CHECK(rm_parse_page(b.bytes, b.count, &p, nullptr) == RM_OK &&
                 p != nullptr && p->skipped_blocks == 0 &&
                 p->highlight_count == 1 &&
                 p->highlights[0].rectangle_count == 1 &&
                 p->highlights[0].rectangles[0].width == 10 &&
                 p->highlights[0].rectangles[0].height == 10,
             "glyph with another discriminator draws its rectangle");
    rm_page_free(p);
    rm_item_id* ids = nullptr;
    size_t cnt = 0;
    RM_CHECK(
        rm_highlighter_items(b.bytes, b.count, &ids, &cnt, nullptr) == RM_OK &&
            cnt == 1 && rm_item_id_equal(ids[0], (rm_item_id){1, 31}),
        "rm_highlighter_items includes the glyph with another discriminator");
    rm_item_ids_free(ids);
    rm_buf_free(&rects);
    rm_buf_free(&tbuf);
    rm_buf_free(&val);
    rm_buf_free(&g);
    rm_buf_free(&b);
  }

  /* 3) 0x08 undo tombstone with no tag 6 and with overlong trailing tag 6
   * parses cleanly and reserves preamble IDs (R153). */
  for (int with_overlong = 0; with_overlong < 2; ++with_overlong) {
    rm_buf b = {0}, t = {0};
    rm_buf_append(&b, base.bytes, base.count);
    rm_write_id(&t, 1, layer);
    rm_write_id(&t, 2, rm_crdt_make(1, 40));
    rm_write_id(&t, 3, RM_CRDT_NONE);
    rm_write_id(&t, 4, RM_CRDT_NONE);
    rm_write_tag(&t, 5, RM_TAG_BYTE4);
    rm_write_u32(&t, 1);
    if (with_overlong) {
      rm_write_tag(&t, 6, RM_TAG_LENGTH4);
      rm_write_u32(&t, 99);
    }
    rm_write_top_block(&b, RM_BLOCK_UNDO_TOMBSTONE, 1, 1, t.bytes, t.count);

    rm_page* p = nullptr;
    RM_CHECK(rm_parse_page(b.bytes, b.count, &p, nullptr) == RM_OK &&
                 p != nullptr && p->skipped_blocks == 0,
             "0x08 undo tombstone overlong=%d skips nothing", with_overlong);
    rm_page_free(p);
    rm_edit_session* s = nullptr;
    RM_CHECK(rm_edit_open(b.bytes, b.count, &s, nullptr) == RM_OK, "open 0x08");
    rm_draft_point pt = rm_draft_point_make(0, 0);
    rm_draft_stroke st =
        rm_draft_stroke_make(RM_TOOL_FINELINER_2, RM_COLOR_BLACK, &pt, 1);
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(layer), (rm_item_id){1, 40},
                                   (rm_item_id){0}, (rm_item_id){0}, &st,
                                   nullptr) == RM_ERR_INVALID_ARGUMENT,
             "0x08 preamble id collision rejected");
    RM_CHECK(rm_edit_insert_stroke(s, rm_item_id_of(layer), (rm_item_id){1, 41},
                                   (rm_item_id){0}, (rm_item_id){0}, &st,
                                   nullptr) == RM_OK,
             "0x08 allows fresh id insertion");
    rm_edit_close(s);
    rm_buf_free(&t);
    rm_buf_free(&b);
  }

  rm_buf_free(&base);
}

typedef struct recovery_log {
  size_t errors;
  char message[256];
} recovery_log;

static void recovery_capture_error(rm_log_level level, const char* message,
                                   void* context) {
  if (level != RM_LOG_ERROR) return;

  recovery_log* log = context;
  log->errors++;
  snprintf(log->message, sizeof log->message, "%s", message);
}

static void scene_placeholders_keep_order_and_report_deletions(void) {
  /* A value-less item still connects its two neighbours. Put their blocks
   * in reverse order so falling back to file order is observable. */
  const uint8_t types[] = {RM_BLOCK_SCENE_LINE_ITEM, RM_BLOCK_SCENE_GROUP_ITEM,
                           RM_BLOCK_IMAGE_ITEM, RM_BLOCK_UNDO_TOMBSTONE,
                           RM_BLOCK_SCENE_GLYPH_ITEM};
  for (size_t i = 0; i < sizeof types; ++i) {
    for (unsigned deleted = 0; deleted <= 2; ++deleted) {
      rm_buf b = {0}, item = {0};
      rm_buf_append(&b, RM_HEADER, RM_HEADER_LENGTH);
      rm_crdt_id layer = rm_crdt_make(1, 10);
      append_scene_group(&b, rm_crdt_make(0, 1), rm_crdt_make(1, 5), layer);
      rm_crdt_id first = rm_crdt_make(1, 50), last = rm_crdt_make(1, 70);
      append_scene_stroke(&b, layer, last, RM_CRDT_NONE, RM_CRDT_NONE,
                          RM_TOOL_HIGHLIGHTER_2);
      append_scene_stroke(&b, layer, first, RM_CRDT_NONE, RM_CRDT_NONE,
                          RM_TOOL_HIGHLIGHTER_2);

      rm_write_id(&item, 1, layer);
      rm_write_id(&item, 2, rm_crdt_make(1, 60));
      rm_write_id(&item, 3, first);
      rm_write_id(&item, 4, last);
      rm_write_tag(&item, 5, RM_TAG_BYTE4);
      rm_write_u32(&item, deleted);
      if (deleted && types[i] != RM_BLOCK_SCENE_GLYPH_ITEM) {
        rm_write_tag(&item, 9, RM_TAG_BYTE1);
        rm_buf_append_byte(&item, 42);
      }
      uint8_t version = types[i] == RM_BLOCK_SCENE_LINE_ITEM ||
                                types[i] == RM_BLOCK_IMAGE_ITEM
                            ? 2
                            : 1;
      rm_write_top_block(&b, types[i], version, version, item.bytes,
                         item.count);

      bool lost_glyph = types[i] == RM_BLOCK_SCENE_GLYPH_ITEM && !deleted;
      recovery_log log = {0};
      rm_log_install(recovery_capture_error, &log);
      rm_page* p = nullptr;
      RM_CHECK(rm_parse_page(b.bytes, b.count, &p, nullptr) == RM_OK && p &&
                   p->skipped_blocks == (size_t)lost_glyph &&
                   p->highlight_count == 0 && p->stroke_count == 2 &&
                   rm_item_id_equal(p->strokes[0].id, rm_item_id_of(first)) &&
                   rm_item_id_equal(p->strokes[1].id, rm_item_id_of(last)),
               "placeholder 0x%02x deleted=%u orders its neighbours", types[i],
               deleted);
      rm_log_silence();
      RM_CHECK(log.errors == (size_t)lost_glyph,
               "only a missing live highlight has a loss diagnostic");
      rm_page_free(p);

      rm_item_id* ids = nullptr;
      size_t count = 0;
      rm_error error = {0};
      rm_status status =
          rm_highlighter_items(b.bytes, b.count, &ids, &count, &error);
      if (lost_glyph) {
        RM_CHECK(status == RM_ERR_INCOMPLETE_PAGE && error.value == 1,
                 "a missing live highlight makes the query incomplete");
      } else {
        RM_CHECK(status == RM_OK && count == 2 &&
                     rm_item_id_equal(ids[0], rm_item_id_of(last)) &&
                     rm_item_id_equal(ids[1], rm_item_id_of(first)),
                 "highlighter query keeps file order across placeholder 0x%02x "
                 "deleted=%u",
                 types[i], deleted);
      }
      rm_item_ids_free(ids);

      /* An unrelated unknown block makes the parser print all four counts,
       * even on a page where the placeholder itself was read quietly. */
      append_block(&b, 0x7F, nullptr, 0);
      log = (recovery_log){0};
      rm_log_install(recovery_capture_error, &log);
      p = nullptr;
      RM_CHECK(rm_parse_page(b.bytes, b.count, &p, nullptr) == RM_OK && p &&
                   p->skipped_blocks == 1u + lost_glyph,
               "skipped count with an unknown block");
      rm_log_silence();
      char expected[96];
      snprintf(expected, sizeof expected,
               "4 scene items, 0 unreadable, %u deleted, %u other blocks;",
               (unsigned)(deleted != 0), 1u + lost_glyph);
      RM_CHECK(log.errors == 1 && strstr(log.message, expected),
               "block 0x%02x deletion length %u: expected %s, got %s", types[i],
               deleted, expected, log.message);
      rm_page_free(p);

      rm_buf_free(&item);
      rm_buf_free(&b);
    }
  }
}

static void arena_oversized_chunks_preserve_head_capacity(void) {
  rm_arena* a1 = rm_arena_create();
  RM_CHECK(a1 != nullptr, "arena1");
  void* huge1 = rm_arena_alloc(a1, 256u * 1024u);
  void* small1 = rm_arena_alloc(a1, 64u);
  RM_CHECK(huge1 != nullptr && small1 != nullptr,
           "oversized first allocation followed by small succeeds");
  rm_arena_destroy(a1);

  rm_arena* a2 = rm_arena_create();
  RM_CHECK(a2 != nullptr, "arena2");
  void* pre = rm_arena_alloc(a2, 64u);
  void* huge2 = rm_arena_alloc(a2, 256u * 1024u);
  void* post = rm_arena_alloc(a2, 64u);
  RM_CHECK(pre != nullptr && huge2 != nullptr && post != nullptr,
           "oversized allocation between two small allocations succeeds");
  if (pre != nullptr && post != nullptr) {
    ptrdiff_t delta = (const char*)post - (const char*)pre;
    RM_CHECK(delta > 0 && delta < 4096,
             "small allocations share retained head chunk (delta=%td)", delta);
  }
  rm_arena_destroy(a2);
}

static void append_root_text_styled_block(rm_buf* file, uint64_t id_base,
                                          bool newline_style,
                                          bool corrupt_second_item) {
  rm_buf list = {0}, inner = {0}, items = {0}, style_entries = {0},
         style_list = {0}, root = {0}, pos = {0};
  const uint32_t chars[3] = {'A', '\n', 'B'};
  rm_write_varuint(&list, corrupt_second_item ? 4 : 3);
  for (int i = 0; i < 3; ++i) {
    rm_buf r = {0}, val = {0};
    char buf[2] = {(char)chars[i], '\0'};
    rm_write_id(&r, 2, rm_crdt_make(1, id_base + (uint64_t)i));
    rm_write_id(
        &r, 3,
        i == 0 ? RM_CRDT_NONE : rm_crdt_make(1, id_base + (uint64_t)i - 1));
    rm_write_id(&r, 4, RM_CRDT_NONE);
    rm_write_tag(&r, 5, RM_TAG_BYTE4);
    rm_write_u32(&r, 0);
    rm_write_string(&val, buf);
    rm_write_block(&r, 6, val.bytes, val.count);
    rm_write_block(&list, 0, r.bytes, r.count);
    rm_buf_free(&val);
    rm_buf_free(&r);
  }
  if (corrupt_second_item) {
    rm_buf bad = {0};
    rm_write_id(&bad, 2, rm_crdt_make(1, id_base + 50));
    rm_buf_append_byte(&bad, 0x3C);
    rm_write_u32(&bad, 999);
    rm_write_block(&list, 0, bad.bytes, bad.count);
    rm_buf_free(&bad);
  }
  rm_write_block(&inner, 1, list.bytes, list.count);
  rm_write_block(&items, 1, inner.bytes, inner.count);

  if (newline_style) {
    rm_write_varuint(&style_entries, 2);
    /* Style keyed to newline id (id_base + 1) -> heading 2. */
    rm_write_bare_id(&style_entries, rm_crdt_make(1, id_base + 1));
    rm_write_id(&style_entries, 1, rm_crdt_make(1, id_base + 10));
    uint8_t fmt_h[] = {0x11, 0x02};
    rm_write_block(&style_entries, 2, fmt_h, sizeof fmt_h);
    /* Newer stamp style keyed to 'A' (non-newline id_base) -> must NOT apply!
     */
    rm_write_bare_id(&style_entries, rm_crdt_make(1, id_base));
    rm_write_id(&style_entries, 1, rm_crdt_make(1, id_base + 20));
    uint8_t fmt_b[] = {0x11, 0x03};
    rm_write_block(&style_entries, 2, fmt_b, sizeof fmt_b);
  } else {
    rm_write_varuint(&style_entries, 0);
  }
  rm_write_block(&style_list, 1, style_entries.bytes, style_entries.count);
  rm_write_block(&items, 2, style_list.bytes, style_list.count);

  rm_write_id(&root, 1, RM_CRDT_NONE);
  rm_write_block(&root, 2, items.bytes, items.count);
  rm_write_f64(&pos, -468.0);
  rm_write_f64(&pos, 234.0);
  rm_write_block(&root, 3, pos.bytes, pos.count);
  rm_write_top_block(file, RM_BLOCK_ROOT_TEXT, 1, 1, root.bytes, root.count);

  rm_buf_free(&pos);
  rm_buf_free(&root);
  rm_buf_free(&style_list);
  rm_buf_free(&style_entries);
  rm_buf_free(&items);
  rm_buf_free(&inner);
  rm_buf_free(&list);
}

static void apply_styles_newline_only_keys_and_no_leaks(void) {
  /* Repeated published 0x07 blocks + 'A'-keyed style ignored vs '\n'-keyed
   * style applied cleanly, followed by composition OOM/failure leak check. */
  rm_buf file = {0};
  rm_buf_append(&file, RM_HEADER, RM_HEADER_LENGTH);
  append_root_text_styled_block(&file, 100, true, false);
  append_root_text_styled_block(&file, 200, true, false);

  rm_page* p = nullptr;
  RM_CHECK(rm_parse_page(file.bytes, file.count, &p, nullptr) == RM_OK &&
               p != nullptr && p->has_text && p->text.paragraph_count == 2 &&
               !p->text.paragraphs[0].has_format &&
               p->text.paragraphs[1].has_format &&
               p->text.paragraphs[1].format.raw == 2,
           "only newline-keyed style applies across repeated RootText");
  rm_page_free(p);

  for (size_t k = 0; k < 25; ++k) {
    rm_test_arena_fail_after(k);
    rm_page* fail_page = nullptr;
    rm_status st = rm_parse_page(file.bytes, file.count, &fail_page, nullptr);
    bool failed = rm_test_arena_did_fail();
    rm_test_arena_fail_after(SIZE_MAX);
    if (failed) RM_CHECK(st == RM_ERR_OUT_OF_MEMORY, "OOM clean");
    rm_page_free(fail_page);
  }
  rm_buf bad = {0};
  rm_buf_append(&bad, RM_HEADER, RM_HEADER_LENGTH);
  append_root_text_styled_block(&bad, 300, true, true);
  rm_page* bp = nullptr;
  RM_CHECK(rm_parse_page(bad.bytes, bad.count, &bp, nullptr) == RM_OK &&
               bp != nullptr,
           "list failure cleans style_line_of_key with zero leaks");
  rm_page_free(bp);
  rm_buf_free(&bad);
  rm_buf_free(&file);
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
  repeated_group_references_scale_linearly();
  duplicate_tombstone_runs_do_not_split_quadratically();
  cross_author_and_uint64_max_tombstone_cuts();
  interleaved_parent_highlighters_and_component_isolation();
  root_text_fragment_budget_isolates_unrelated_strokes();
  a2_deleted_glyph_and_undo_tombstone_envelopes_and_missing_tag6();
  scene_placeholders_keep_order_and_report_deletions();
  arena_oversized_chunks_preserve_head_capacity();
  apply_styles_newline_only_keys_and_no_leaks();
  return rm_test_report("recovery");
}
