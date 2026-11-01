/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* The parser against bytes nobody wrote.
 *
 * Every `.rm` this library is handed came off a device and through a sync, so
 * the input is untrusted in the only sense that matters: it is not this
 * library's own writer that produced it. The suites beside this one prove
 * what the parser reads; this one proves what it does with what it cannot —
 * that a stream ending inside a field, a count declaring more than the file
 * holds, or a length that runs past its block all end in a status rather than
 * in a read past the end of the buffer.
 *
 * Built only with `-DRMLINES_BUILD_FUZZERS=ON` under clang, and run with the
 * sample corpus as its seeds. */

#include <stddef.h>
#include <stdint.h>

#include "rmlines/parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    rm_page *page = nullptr;
    rm_error error;
    if (rm_parse_page(data, size, &page, &error) == RM_OK) {
        /* Touched rather than merely freed: a page that parses has to be a
         * page that can be read, and the arrays hang off counts the file
         * chose. */
        volatile size_t seen = 0;
        for (size_t i = 0; i < page->stroke_count; i += 1) seen += page->strokes[i].point_count;
        for (size_t i = 0; i < page->highlight_count; i += 1) {
            seen += page->highlights[i].rectangle_count;
        }
        for (size_t i = 0; i < page->image_count; i += 1) seen += page->images[i].file.length;
        if (page->has_text) {
            for (size_t i = 0; i < page->text.paragraph_count; i += 1) {
                seen += page->text.paragraphs[i].text.length;
                seen += page->text.paragraphs[i].face_count;
            }
        }
        (void)seen;
        rm_page_free(page);
    }
    return 0;
}
