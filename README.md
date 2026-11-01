# RMLines (C23)

Reading and writing the reMarkable `.rm` v6 page format: the page and its strokes, typed text and its paragraphs, highlights and images — and
writing fresh pages the tablet accepts. Nothing here draws, and nothing here has heard of the cloud, the document index or a file manager.

A standalone C23 library with no dependency beyond the C standard library. It is the portable sibling of
**[rmlines-swift](https://github.com/altessa-s/rmlines-swift)**, the Swift package for Apple platforms: the two read the same pages to the same
strokes, text and styles, and write the same draft to the same bytes.

## Why it exists

A notebook on disk is a CRDT of tagged blocks, not a picture. Turning one into the other is two jobs: **read the file**, then **draw what was read**.
This library is the first job and only the first: a renderer, a converter and a test corpus all have to reach it, and a parser that lives inside any one
of them is a parser the other two cannot use.

## Building

A C23 compiler is the one requirement, and not every compiler that takes a C23 flag is one: the library spells its constants with `constexpr`,
which **clang 19** and **GCC 13** read and clang 18 does not. On Windows that means clang — MSVC's C compiler reads none of the C23 the code is
written in (`constexpr`, `nullptr`, `[[nodiscard]]`, typed enums), and is not supported. CMake 3.21 or newer; the build never spells the standard
flag itself and lets CMake pick the one the compiler answers to — GCC 13 knows the standard only as `-std=c2x`, GCC 14 also as `-std=c23`. A consumer
compiling against the installed headers needs the same: they use `constexpr` too.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --build build --target check      # the test suites, through CTest
```

| Option | Default | What it does |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build the shared library instead of the static one |
| `RMLINES_BUILD_TESTS` | on when top-level | Build the test suites |
| `RMLINES_BUILD_TOOLS` | on when top-level | Build `rmdump` |
| `RMLINES_WERROR` | `OFF` | Turn warnings into errors |
| `RMLINES_BUILD_FUZZERS` | `OFF` | Build the libFuzzer target (clang only) |

Installing gives headers, the library, a CMake package and a pkg-config file:

```sh
cmake --install build --prefix /usr/local
```

```cmake
set(CMAKE_C_STANDARD 23)   # the installed headers are C23
find_package(RMLines REQUIRED)
target_link_libraries(yours PRIVATE RMLines::rmlines)
```

```sh
cc -std=c2x yours.c $(pkg-config --cflags --libs rmlines)   # -std=c23 on GCC 14 and clang 19
```

The shared library exports the functions of the contract and nothing else: the arena, the hash map and the byte reader are compiled with hidden
visibility, so nobody links against plumbing that is free to change.

## Reading

```c
#include <rmlines/rmlines.h>

rm_page *page = NULL;
rm_error error;
if (rm_parse_page(bytes, size, &page, &error) == RM_OK) {
    for (size_t i = 0; i < page->stroke_count; i += 1) {
        const rm_stroke *stroke = &page->strokes[i];
        /* stroke->points, stroke->color, stroke->anchor … */
    }
    rm_page_free(page);
}
```

One `rm_page_free` releases the page and everything it points at: the whole parse comes out of one arena, so a caller who loses track of a stroke's
points cannot free them out from under the page, and there is no per-field ownership to get wrong.

Five statuses reach this caller:

| Status | When |
|---|---|
| `RM_ERR_INVALID_ARGUMENT` | `data` or `out_page` is null. |
| `RM_ERR_OUT_OF_MEMORY` | An allocation failed. The whole page is lost, not quietly shortened — a caller that reads it as "no content here" is wrong about what it has. |
| `RM_ERR_UNSUPPORTED_VERSION` | A `.rm` whose header names a generation this reader does not read. `rm_error.version` is the number. |
| `RM_ERR_UNSUPPORTED_FORMAT` | Bytes that do not begin as a page at all. The first 32 of them travel in `rm_error.message`. |
| `RM_ERR_UNEXPECTED_EOF` | The stream is shorter than a header, or a block declares a length past the end of the file. A field that fails inside a block costs only that block, and fewer than eight bytes left over after the last block are ignored. A broken list of text items or paragraph styles is the exception: it is dropped uncounted, and the text comes back without those records or styles. |

`RM_ERR_MALFORMED_PAGE` is what the individual block parsers raise — a length past the block, a NaN coordinate, a count too large for the machine — and
it stops there. A page is not lost for one stroke it could not read: the block is stepped over, the loss is added to `rm_page.skipped_blocks`, and a
line goes to the diagnostics sink.

## Writing

```c
rm_draft_paragraph paragraphs[] = {
    { .text = "Heading", .has_style = true, .style = 2 },
    { .text = "Body" },
};
rm_page_draft draft = {
    .sheet = { 1404, 1872, 1404, 1872 },
    .column = { 936, 1 },
    .paragraphs = paragraphs, .paragraph_count = 2,
};
memcpy(draft.author, author_uuid, 16);

uint8_t *bytes = NULL;
size_t size = 0;
if (rm_write_page(&draft, &bytes, &size, &error) == RM_OK) {
    /* … */
    rm_bytes_free(bytes);
}
```

`rm_draft_point_make` and `rm_draft_stroke_make` fill in the defaults a point and a stroke usually carry. Text is UTF-8; ill-formed bytes are written as
U+FFFD, and every count and anchor is taken from the text as written.

The writer builds **fresh pages only**. It never edits an existing file, because inserting into somebody else's CRDT chains is a different contract with
a user's notebook at stake — that contract is the editor's, below. It writes a deliberately small subset of the format; the parser's tolerance is no
licence for the writer.

Two refusals, and both mean nothing was written — a half-page is worse than none:

- `RM_ERR_ANCHOR_OUT_OF_RANGE` — ink fastened to a paragraph the draft has not got. Written anyway it would hang on nothing and vanish without a trace,
  and silently losing ink is the one thing a writer must never do.
- `RM_ERR_TOO_MANY_TO_NUMBER` — more items of one kind than a page's identifier ranges can name. Past the limit two items would share a counter, and the
  file cannot tell two meanings of one counter apart.

## Editing

```c
rm_item_id target = page->strokes[3].id;      /* or rm_item_id_parse("1:1234", &target) */
uint8_t *edited = NULL;
size_t edited_size = 0;
if (rm_erase_items(bytes, size, &target, 1, &edited, &edited_size, &error) == RM_OK) {
    /* … */
    rm_bytes_free(edited);
}
```

Every parsed stroke and highlight carries the identifier the page itself calls it by (`rm_item_id`, spelled `author:counter`), and that is the handle an
edit finds it by. `rm_erase_items` erases the way the tablet's own eraser does: the item stays as a tombstone — the same block, identifier, parent and
neighbours, a deleted length of one and no value — because a later item may hang its `left` on it and the tablet merges pages by these identifiers.
Every other byte goes out as it came in. The result is read back before it is handed over, and two refusals mean nothing was written:

- `RM_ERR_ITEMS_NOT_FOUND` — an identifier is not a living stroke or highlight of this page. The edit does not guess which items were meant.
- `RM_ERR_UNVERIFIED` — the edited page did not parse back to what the original held less the erased items.

`rm_highlighter_items` answers every living mark of the highlighter: its strokes on every layer, hidden layers included — showing the layer again would
bring them back — but not on layers taken out of the tree, and every highlight over a text layer. It is what an erase of "everything the marker did" takes, and `rm_page` cannot answer it: the page is what is drawn. A
page with a block that would not read is refused with `RM_ERR_INCOMPLETE_PAGE` (`rm_error.value` blocks lost) rather than answered in part: the block
lost may be a mark, and "every mark" less one is not every mark.

## Diagnostics

```c
static void sink(rm_log_level level, const char *message, void *context) { … }
rm_log_install(sink, NULL);
```

Silence until a host installs a sink, and silence again after `rm_log_silence`. Not called is the supported state — a corpus test wants no log — and the
default is silence rather than `stderr`, because a library that writes to a terminal nobody is watching is a library that has to be worked around. Every
value in every message is a count, a block type or a raw style number: the page's writing, its text and its file names do not reach a sink, so a host is
free to install one that writes a file and keep it.

## rmdump

```sh
rmdump page.rm            # the indented text form
rmdump --json page.rm     # one JSON object per page
rmdump --quiet page.rm    # without the diagnostics sink
```

Every number goes out at full precision and every list in the order the parser answered in, because the point of the tool is to be diffed: against
another build, against another page, or against what [rmlines-swift](https://github.com/altessa-s/rmlines-swift) reads from the same file.

## Design

**Read, do not draw.** `RM_INK_UNITS_PER_RAW_WIDTH` says what a number in the file means (raw width ÷ 4 is a width on the page). Palettes, blend modes,
type metrics and templates belong to a renderer: they are measurements of a picture, and reading needs none of them.

**A page that will not parse is not a broken document.** An unreadable stroke is counted and the rest of the page is kept. A header of version 3 or 5 is
`RM_ERR_UNSUPPORTED_VERSION`, not "unrecognisable bytes": the answers differ, and only a version that was actually read can be named to a user.

**Loss is numbered, deletion is not.** `rm_page.skipped_blocks` is strokes and blocks this version could not read. The three service blocks every page
opens with hold nothing to draw and are not a loss. Erased items keep their place in the CRDT chain and are not a loss either — but an item whose value
is there and will not read (an image on three corners, a group link of the wrong type) is a loss, not an erasure, and is counted and logged as one.

**A place, not a shift.** Handwriting anchored to typed text is fastened to a character. Turning that into coordinates takes a font and a column width,
both of which belong to drawing, so `rm_text_anchor` travels as a paragraph and an offset until then — counted in **Unicode scalars**, the way the file
numbers characters.

**Unknown markup is kept.** A paragraph style this library does not know is kept as its raw value and drawn as plain text rather than dropped —
dropping it loses the indent with the marker.

**A number that is not one does not reach the page.** A stroke whose thickness, width or pressure is NaN or infinite is a counted loss; a speed that is not a number
reads as nought, and an anchor's horizontal that is not one is left unset. A NaN that reached a renderer would draw nothing, and one that reached `rmdump --json` would be
a document no JSON reader takes.

**A declared count is the file's word.** Every length and count is put against the block it sits in before a byte is reserved on it: a `uint32` of points
is three hundred million points, and five gigabytes on the word of a sixty-byte file.

**Not quadratic.** The text's CRDT ordering walks each tie with an index and two marks, in linear time; the scene's chain sorts each tie once,
in O(n log n); and the attachment map is read into a hash on its 16-byte key rather than scanned entry by entry. A `.rm` arrives from a device and is parsed
inside somebody's application, in a task nothing interrupts; a page built to be hostile must not be able to choose how long that takes.

## Layout

| Path | What is in it |
|---|---|
| `include/rmlines/` | The contract: `types.h`, `parser.h`, `writer.h`, `editor.h`, `log.h`, and `rmlines.h` over them |
| `src/rm_bytes.c` | The byte level both ways: little-endian scalars, varuints, tags, subblocks |
| `src/rm_parser.c` | The blocks, the scene tree, the text CRDT, the assembly of a page |
| `src/rm_writer.c` | The blocks a fresh page is made of, and the identifier ranges it hands out |
| `src/rm_editor.c` | Erasing from an existing page, in place, and finding the marker on every layer |
| `src/rm_support.c` | The arena, the buffers, the UTF-8 repair, the identifier map, the grouping |
| `tools/rmdump.c` | The command-line dump |
| `tests/` | The test suites, hand-built pages and a sample corpus |

## See also

- **[rmlines-swift](https://github.com/altessa-s/rmlines-swift)** — the same format as a Swift package for macOS and iOS, with an `RMLines.xcframework`
  for binary consumers.

## Acknowledgements

The `.rm` format is not published by reMarkable. This library is built on openly available information and on the research of third-party
developers who documented the format, among them [rmscene](https://github.com/ricklupton/rmscene) and
[remarkable_rust](https://github.com/hiteshjoshi/remarkable_rust).

## Licence

MIT — see [LICENSE](LICENSE). The sample corpus under `tests/fixtures/v6-samples/` comes from
[remarkable_rust](https://github.com/hiteshjoshi/remarkable_rust) and carries its own licence beside it.

reMarkable is a trademark of reMarkable AS. This project is not affiliated with, endorsed by or sponsored by reMarkable AS.
