/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

/* What the library read out of a page, printed.
 *
 * Every number goes out at full precision and every list in the order the
 * parser answered in, because the point of this tool is to be diffed: against
 * another build, against another page, against another reader of the same
 * format. A dump that rounds or reorders proves nothing. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmlines/rmlines.h"

static void usage(FILE *to) {
    fprintf(to,
            "usage: rmdump [--json] [--quiet] <page.rm> [more.rm ...]\n"
            "\n"
            "  --json   print JSON instead of the indented text form\n"
            "  --quiet  do not install a diagnostics sink\n"
            "\n"
            "Reads reMarkable .rm v6 pages and prints what was parsed.\n");
}

static void sink(rm_log_level level, const char *message, void *context) {
    (void)context;
    fprintf(stderr, "[%s] %s\n", level == RM_LOG_ERROR ? "error" : "debug", message);
}

static const char *tool_name(rm_pen_tool tool) {
    switch (tool) {
        case RM_TOOL_UNKNOWN: return "unknown";
        case RM_TOOL_PAINTBRUSH_1: return "paintbrush1";
        case RM_TOOL_PENCIL_1: return "pencil1";
        case RM_TOOL_BALLPOINT_1: return "ballpoint1";
        case RM_TOOL_MARKER_1: return "marker1";
        case RM_TOOL_FINELINER_1: return "fineliner1";
        case RM_TOOL_HIGHLIGHTER_1: return "highlighter1";
        case RM_TOOL_ERASER: return "eraser";
        case RM_TOOL_MECHANICAL_PENCIL_1: return "mechanicalPencil1";
        case RM_TOOL_ERASER_AREA: return "eraserArea";
        case RM_TOOL_PAINTBRUSH_2: return "paintbrush2";
        case RM_TOOL_MECHANICAL_PENCIL_2: return "mechanicalPencil2";
        case RM_TOOL_PENCIL_2: return "pencil2";
        case RM_TOOL_BALLPOINT_2: return "ballpoint2";
        case RM_TOOL_MARKER_2: return "marker2";
        case RM_TOOL_FINELINER_2: return "fineliner2";
        case RM_TOOL_HIGHLIGHTER_2: return "highlighter2";
        case RM_TOOL_CALIGRAPHY: return "caligraphy";
        case RM_TOOL_SHADER: return "shader";
    }
    return "unknown";
}

/* The text of a page reaches a terminal and a JSON reader alike, and neither
 * wants a raw control byte. Escaped the way JSON spells it, in both forms, so
 * that the two dumps differ only in their punctuation. */
static void print_escaped(FILE *to, rm_str text) {
    for (size_t i = 0; i < text.length; i += 1) {
        unsigned char byte = (unsigned char)text.bytes[i];
        switch (byte) {
            case '"': fputs("\\\"", to); break;
            case '\\': fputs("\\\\", to); break;
            case '\n': fputs("\\n", to); break;
            case '\r': fputs("\\r", to); break;
            case '\t': fputs("\\t", to); break;
            default:
                if (byte < 0x20u) fprintf(to, "\\u%04x", byte);
                else fputc((int)byte, to);
        }
    }
}

static void print_anchor_text(const rm_text_anchor *anchor) {
    printf("paragraph=%" PRId32 " offset=%" PRId32, anchor->paragraph, anchor->offset);
    if (anchor->has_origin_x) printf(" origin-x=%.17g", anchor->origin_x);
    else printf(" origin-x=-");
}

static void dump_text(const rm_page *page, const char *name) {
    printf("page %s\n", name);
    printf("  skipped-blocks %zu\n", page->skipped_blocks);
    printf("  anchored-strokes %zu\n", page->anchored_strokes);
    if (page->has_paper_size) {
        printf("  paper-size %.17g %.17g\n", page->paper_size.width, page->paper_size.height);
    } else {
        printf("  paper-size -\n");
    }
    printf("  background-visible %d\n", page->background_visible ? 1 : 0);
    printf("  root-document-visible %d\n", page->root_document_visible ? 1 : 0);

    printf("  strokes %zu\n", page->stroke_count);
    for (size_t i = 0; i < page->stroke_count; i += 1) {
        const rm_stroke *stroke = &page->strokes[i];
        printf("    stroke %zu tool=%s color=%d thickness=%.17g points=%zu", i,
               tool_name(stroke->tool), (int)stroke->color, stroke->thickness_scale,
               stroke->point_count);
        if (stroke->has_argb) printf(" argb=%08" PRIx32, stroke->argb);
        else printf(" argb=-");
        if (stroke->has_anchor) {
            printf(" anchor=");
            print_anchor_text(&stroke->anchor);
        } else {
            printf(" anchor=-");
        }
        if (stroke->has_source_id) {
            printf(" source=%u:%" PRIu64, (unsigned)stroke->source_id.author,
                   stroke->source_id.counter);
        } else {
            printf(" source=-");
        }
        printf("\n");
        for (size_t p = 0; p < stroke->point_count; p += 1) {
            const rm_stroke_point *point = &stroke->points[p];
            printf("      point %zu x=%.9g y=%.9g width=%.9g pressure=%.9g speed=%.9g\n", p,
                   (double)point->x, (double)point->y, (double)point->width,
                   (double)point->pressure, (double)point->speed);
        }
    }

    printf("  highlights %zu\n", page->highlight_count);
    for (size_t i = 0; i < page->highlight_count; i += 1) {
        const rm_highlight *highlight = &page->highlights[i];
        printf("    highlight %zu color=%d rectangles=%zu", i, (int)highlight->color,
               highlight->rectangle_count);
        if (highlight->has_argb) printf(" argb=%08" PRIx32, highlight->argb);
        else printf(" argb=-");
        printf(" text=");
        if (highlight->has_text) {
            printf("\"");
            print_escaped(stdout, highlight->text);
            printf("\"");
        } else {
            printf("-");
        }
        printf("\n");
        for (size_t r = 0; r < highlight->rectangle_count; r += 1) {
            const rm_rect *rectangle = &highlight->rectangles[r];
            printf("      rect %zu x=%.17g y=%.17g width=%.17g height=%.17g\n", r, rectangle->x,
                   rectangle->y, rectangle->width, rectangle->height);
        }
    }

    printf("  images %zu\n", page->image_count);
    for (size_t i = 0; i < page->image_count; i += 1) {
        const rm_page_image *image = &page->images[i];
        printf("    image %zu file=\"", i);
        print_escaped(stdout, image->file);
        printf("\" strokes-before=%zu", image->strokes_before);
        if (image->has_anchor) {
            printf(" anchor=");
            print_anchor_text(&image->anchor);
        } else {
            printf(" anchor=-");
        }
        printf("\n");
        for (size_t v = 0; v < 4; v += 1) {
            printf("      vertex %zu x=%.17g y=%.17g u=%.17g v=%.17g\n", v, image->vertices[v].x,
                   image->vertices[v].y, image->vertices[v].u, image->vertices[v].v);
        }
    }

    if (!page->has_text) {
        printf("  text -\n");
        return;
    }
    printf("  text origin-x=%.17g origin-y=%.17g", page->text.origin_x, page->text.origin_y);
    if (page->text.has_column_width) printf(" column-width=%.17g\n", page->text.column_width);
    else printf(" column-width=-\n");
    printf("    paragraphs %zu\n", page->text.paragraph_count);
    for (size_t i = 0; i < page->text.paragraph_count; i += 1) {
        const rm_paragraph *paragraph = &page->text.paragraphs[i];
        printf("      paragraph %zu \"", i);
        print_escaped(stdout, paragraph->text);
        printf("\" format=");
        if (paragraph->has_format) {
            printf("%u/", (unsigned)paragraph->format.raw);
            if (paragraph->format.has_second) printf("%u/", (unsigned)paragraph->format.second);
            else printf("-/");
            if (paragraph->format.has_third) printf("%" PRIu32, paragraph->format.third);
            else printf("-");
        } else {
            printf("-");
        }
        printf(" faces=%zu\n", paragraph->face_count);
        for (size_t f = 0; f < paragraph->face_count; f += 1) {
            const rm_face_run *run = &paragraph->faces[f];
            printf("        face %zu range=%" PRId32 "..<%" PRId32 " bold=%d italic=%d\n", f,
                   run->start, run->end, run->bold ? 1 : 0, run->italic ? 1 : 0);
        }
    }
}

static void dump_json(const rm_page *page, const char *name) {
    printf("{\"name\":\"");
    print_escaped(stdout, (rm_str){ .bytes = name, .length = strlen(name) });
    printf("\",\"skippedBlocks\":%zu,\"anchoredStrokes\":%zu", page->skipped_blocks,
           page->anchored_strokes);
    if (page->has_paper_size) {
        printf(",\"paperSize\":{\"width\":%.17g,\"height\":%.17g}", page->paper_size.width,
               page->paper_size.height);
    } else {
        printf(",\"paperSize\":null");
    }
    printf(",\"backgroundVisible\":%s,\"rootDocumentVisible\":%s",
           page->background_visible ? "true" : "false",
           page->root_document_visible ? "true" : "false");

    printf(",\"strokes\":[");
    for (size_t i = 0; i < page->stroke_count; i += 1) {
        const rm_stroke *stroke = &page->strokes[i];
        printf("%s{\"tool\":\"%s\",\"color\":%d,\"thickness\":%.17g", i == 0 ? "" : ",",
               tool_name(stroke->tool), (int)stroke->color, stroke->thickness_scale);
        if (stroke->has_argb) printf(",\"argb\":%" PRIu32, stroke->argb);
        else printf(",\"argb\":null");
        if (stroke->has_anchor) {
            printf(",\"anchor\":{\"paragraph\":%" PRId32 ",\"offset\":%" PRId32,
                   stroke->anchor.paragraph, stroke->anchor.offset);
            if (stroke->anchor.has_origin_x) printf(",\"originX\":%.17g", stroke->anchor.origin_x);
            else printf(",\"originX\":null");
            printf("}");
        } else {
            printf(",\"anchor\":null");
        }
        if (stroke->has_source_id) {
            printf(",\"sourceId\":\"%u:%" PRIu64 "\"", (unsigned)stroke->source_id.author,
                   stroke->source_id.counter);
        } else {
            printf(",\"sourceId\":null");
        }
        printf(",\"points\":[");
        for (size_t p = 0; p < stroke->point_count; p += 1) {
            const rm_stroke_point *point = &stroke->points[p];
            printf("%s{\"x\":%.9g,\"y\":%.9g,\"width\":%.9g,\"pressure\":%.9g,\"speed\":%.9g}",
                   p == 0 ? "" : ",", (double)point->x, (double)point->y, (double)point->width,
                   (double)point->pressure, (double)point->speed);
        }
        printf("]}");
    }
    printf("]");

    printf(",\"highlights\":[");
    for (size_t i = 0; i < page->highlight_count; i += 1) {
        const rm_highlight *highlight = &page->highlights[i];
        printf("%s{\"color\":%d", i == 0 ? "" : ",", (int)highlight->color);
        if (highlight->has_argb) printf(",\"argb\":%" PRIu32, highlight->argb);
        else printf(",\"argb\":null");
        printf(",\"text\":");
        if (highlight->has_text) {
            printf("\"");
            print_escaped(stdout, highlight->text);
            printf("\"");
        } else {
            printf("null");
        }
        printf(",\"rectangles\":[");
        for (size_t r = 0; r < highlight->rectangle_count; r += 1) {
            const rm_rect *rectangle = &highlight->rectangles[r];
            printf("%s{\"x\":%.17g,\"y\":%.17g,\"width\":%.17g,\"height\":%.17g}",
                   r == 0 ? "" : ",", rectangle->x, rectangle->y, rectangle->width,
                   rectangle->height);
        }
        printf("]}");
    }
    printf("]");

    printf(",\"images\":[");
    for (size_t i = 0; i < page->image_count; i += 1) {
        const rm_page_image *image = &page->images[i];
        printf("%s{\"file\":\"", i == 0 ? "" : ",");
        print_escaped(stdout, image->file);
        printf("\",\"strokesBefore\":%zu,\"vertices\":[", image->strokes_before);
        for (size_t v = 0; v < 4; v += 1) {
            printf("%s{\"x\":%.17g,\"y\":%.17g,\"u\":%.17g,\"v\":%.17g}", v == 0 ? "" : ",",
                   image->vertices[v].x, image->vertices[v].y, image->vertices[v].u,
                   image->vertices[v].v);
        }
        printf("]}");
    }
    printf("]");

    if (!page->has_text) {
        printf(",\"text\":null}\n");
        return;
    }
    printf(",\"text\":{\"originX\":%.17g,\"originY\":%.17g", page->text.origin_x,
           page->text.origin_y);
    if (page->text.has_column_width) printf(",\"columnWidth\":%.17g", page->text.column_width);
    else printf(",\"columnWidth\":null");
    printf(",\"paragraphs\":[");
    for (size_t i = 0; i < page->text.paragraph_count; i += 1) {
        const rm_paragraph *paragraph = &page->text.paragraphs[i];
        printf("%s{\"text\":\"", i == 0 ? "" : ",");
        print_escaped(stdout, paragraph->text);
        printf("\",\"format\":");
        if (paragraph->has_format) {
            printf("{\"raw\":%u,", (unsigned)paragraph->format.raw);
            if (paragraph->format.has_second) printf("\"second\":%u,", (unsigned)paragraph->format.second);
            else printf("\"second\":null,");
            if (paragraph->format.has_third) printf("\"third\":%" PRIu32 "}", paragraph->format.third);
            else printf("\"third\":null}");
        } else {
            printf("null");
        }
        printf(",\"faces\":[");
        for (size_t f = 0; f < paragraph->face_count; f += 1) {
            const rm_face_run *run = &paragraph->faces[f];
            printf("%s{\"start\":%" PRId32 ",\"end\":%" PRId32 ",\"bold\":%s,\"italic\":%s}",
                   f == 0 ? "" : ",", run->start, run->end, run->bold ? "true" : "false",
                   run->italic ? "true" : "false");
        }
        printf("]}");
    }
    printf("]}}\n");
}

static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return nullptr;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return nullptr; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return nullptr; }
    rewind(file);
    unsigned char *bytes = malloc((size_t)size + 1u);
    if (bytes == nullptr) { fclose(file); return nullptr; }
    size_t read = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) { free(bytes); return nullptr; }
    *out_size = read;
    return bytes;
}

/* Both separators: the tool builds on Windows, where a path is spelled with
 * backslashes and the name would otherwise come out as the whole path. */
static const char *basename_of(const char *path) {
    const char *name = path;
    for (const char *at = path; *at != '\0'; at += 1) {
        if (*at == '/' || *at == '\\') name = at + 1;
    }
    return name;
}

int main(int argc, char **argv) {
    bool json = false;
    bool quiet = false;
    int first = 1;
    for (; first < argc; first += 1) {
        if (strcmp(argv[first], "--json") == 0) json = true;
        else if (strcmp(argv[first], "--quiet") == 0) quiet = true;
        else if (strcmp(argv[first], "--help") == 0 || strcmp(argv[first], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (argv[first][0] == '-' && argv[first][1] != '\0') {
            fprintf(stderr, "rmdump: unknown option %s\n", argv[first]);
            usage(stderr);
            return 2;
        } else {
            break;
        }
    }
    if (first >= argc) {
        usage(stderr);
        return 2;
    }
    if (!quiet) rm_log_install(sink, nullptr);

    int failures = 0;
    for (int i = first; i < argc; i += 1) {
        size_t size = 0;
        unsigned char *bytes = read_file(argv[i], &size);
        if (bytes == nullptr) {
            fprintf(stderr, "rmdump: cannot read %s\n", argv[i]);
            failures += 1;
            continue;
        }
        rm_page *page = nullptr;
        rm_error error;
        rm_status status = rm_parse_page(bytes, size, &page, &error);
        free(bytes);
        if (status != RM_OK) {
            fprintf(stderr, "rmdump: %s: %s", argv[i], rm_status_string(status));
            if (status == RM_ERR_UNSUPPORTED_VERSION) fprintf(stderr, " (version %d)", error.version);
            else if (error.message[0] != '\0') fprintf(stderr, " (%s)", error.message);
            fprintf(stderr, "\n");
            failures += 1;
            continue;
        }
        if (json) dump_json(page, basename_of(argv[i]));
        else dump_text(page, basename_of(argv[i]));
        rm_page_free(page);
    }
    return failures == 0 ? 0 : 1;
}
