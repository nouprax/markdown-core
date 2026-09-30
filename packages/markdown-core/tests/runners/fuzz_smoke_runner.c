/* Deterministic fuzz smoke suite.
 *
 * Runs fixed corpora and seeded pseudo-random byte sequences through the
 * read-only facade: parse, traverse every node and accessor, dump twice
 * (checking dump determinism), and free.  No renderer is involved and no
 * network or random device is read; the same inputs are generated on every
 * run.  Long-running fuzz campaigns stay in the explicit AFL/libFuzzer
 * maintenance tasks, which reuse the corpus under tests/core/afl_test_cases.
 *
 *   fuzz_smoke_runner [--corpus FILE]... [--generated COUNT]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "test_support.h"

static size_t nodes_visited;

static int inspect_node(const markdown_core_node *node, ts_ast_range range, void *context) {
    int64_t rowspan, colspan;
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);

    nodes_visited++;
    (void)markdown_core_node_kind_name(kind);
    if (range.start < 0 || range.end < range.start) {
        return -1;
    }
    switch (kind) {
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_COMMENT:
        (void)markdown_core_node_literal(node);
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        (void)markdown_core_node_heading_level(node);
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        (void)markdown_core_node_list_item_marker(node);
        break;
    case MARKDOWN_CORE_KIND_TABLE_CELL:
        markdown_core_node_table_cell_spans(node, &rowspan, &colspan);
        break;
    case MARKDOWN_CORE_KIND_DIRECTIVE:
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        (void)markdown_core_node_directive_properties(node);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION:
        (void)markdown_core_node_definition_compact(node);
        break;
    default:
        break;
    }
    return 0;
}

static int smoke(const uint8_t *bytes, size_t length, const char *label) {
    markdown_core_document *document;
    markdown_core_error *error;
    uint8_t *first = NULL;
    uint8_t *second = NULL;
    size_t first_length = 0;
    size_t second_length = 0;
    int result = -1;

    document = markdown_core_document_parse(bytes, length, &error);
    if (!document) {
        /* A parse failure carries its reason. */
        if (markdown_core_error_get_message(error).length == 0) {
            fprintf(stderr, "%s: parse error carries no message\n", label);
            markdown_core_error_free(error);
            return -1;
        }
        markdown_core_error_free(error);
        return 0;
    }

    if (ts_ast_walk(markdown_core_document_root(document), inspect_node, NULL) != 0) {
        fprintf(stderr, "%s: traversal produced an invalid range\n", label);
        goto done;
    }
    if (ts_ast_range_outside(markdown_core_document_root(document), length)) {
        fprintf(stderr, "%s: a range lies outside the source\n", label);
        goto done;
    }
    if (!markdown_core_document_dump(document, markdown_core_document_root(document), bytes, length, &first,
                                     &first_length, &error) ||
        !markdown_core_document_dump(document, markdown_core_document_root(document), bytes, length, &second,
                                     &second_length, &error)) {
        fprintf(stderr, "%s: dump failed\n", label);
        goto done;
    }
    if (first_length != second_length || memcmp(first, second, first_length) != 0) {
        fprintf(stderr, "%s: dump is not deterministic\n", label);
        goto done;
    }
    result = 0;

done:
    markdown_core_dump_free(first);
    markdown_core_dump_free(second);
    markdown_core_document_free(document);
    return result;
}

/* ONE PROBE FOR EVERY WALK THAT DECODES A CHARACTER: a stray continuation
 * byte or a lead byte cut off by its line or by the input, where that walk
 * meets it. What such input parses to is unspecified; that each walk stays
 * inside its range is not. Each probe is parsed from a heap block of exactly
 * its length, so a read past the input is a sanitizer error, and a read past
 * a line that stays inside the parser's own buffer shows as a scope outside
 * the source. */
static const struct {
    const char *bytes;
    size_t length;
} probes[] = {
#define PROBE(literal) {literal, sizeof(literal) - 1}
    PROBE("\xe4**a** b\n"),                     /* flanking, before */
    PROBE("**a**\xe4 b\n"),                     /* flanking, after */
    PROBE("a **\xe4"),                          /* flanking, cut at the end */
    PROBE("http://\xf0"),                       /* URL host, cut at the end */
    PROBE("# http://\xf0\n\nbody\n"),           /* URL host, cut at the line */
    PROBE("see www.a\xe4\xb8\n"),               /* domain walk, cut */
    PROBE("see www.\x80\x80.com x\n"),          /* domain walk, stray */
    PROBE("# heading \xe4\xb8\n"),              /* anchor, cut */
    PROBE("# \x80\xbf\n"),                      /* anchor, stray */
    PROBE("[\xe4]\n\n[\xe4]: /u\n"),            /* label normal form */
    PROBE("x @\xe4 y\n"),                       /* citation key */
    PROBE("\xe4@key y\n"),                      /* citation opener */
    PROBE("(@\xe4) item\n"),                    /* specimen label */
    PROBE("| a |\n| - |\n| b |\n:\xe4\n"),      /* table caption */
    PROBE("| \xe4\xb8 |\n| - |\n"),             /* pipe table cell */
    PROBE("+---+\n| \xe4 |\n+---+\n"),          /* grid table columns */
    PROBE("- [\xe4] task\n"),                   /* task marker */
    PROBE("---\ntitle: \xe4\xb8\n---\nbody\n"), /* front-matter printable */
#undef PROBE
};

int main(int argc, char **argv) {
    int i;
    size_t generated = 256;
    size_t failures = 0;
    ts_prng prng;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            const char *path = argv[++i];
            size_t length = 0;
            uint8_t *bytes = ts_read_file(path, &length);
            if (!bytes) {
                fprintf(stderr, "cannot read corpus file: %s\n", path);
                failures++;
                continue;
            }
            if (smoke(bytes, length, path) != 0) {
                failures++;
            }
            free(bytes);
        } else if (strcmp(argv[i], "--generated") == 0 && i + 1 < argc) {
            generated = (size_t)atoi(argv[++i]);
        } else {
            fputs("usage: fuzz_smoke_runner [--corpus FILE]... [--generated COUNT]\n", stderr);
            return 2;
        }
    }

    for (i = 0; (size_t)i < sizeof(probes) / sizeof(*probes); i++) {
        char label[64];
        uint8_t *bytes = (uint8_t *)malloc(probes[i].length);
        if (!bytes) {
            failures++;
            break;
        }
        memcpy(bytes, probes[i].bytes, probes[i].length);
        snprintf(label, sizeof(label), "probe[%d]", i);
        if (smoke(bytes, probes[i].length, label) != 0) {
            failures++;
        }
        free(bytes);
    }

    ts_prng_seed(&prng, UINT64_C(0x6D61726B646F776E)); /* "markdown" */
    for (i = 0; (size_t)i < generated; i++) {
        char label[64];
        size_t length = (size_t)(ts_prng_next(&prng) % 8192);
        /* Exactly `length` bytes: a read past the input is a sanitizer
         * error, not a read of a terminator nobody promised. */
        uint8_t *bytes = (uint8_t *)malloc(length ? length : 1);
        size_t offset;
        if (!bytes) {
            failures++;
            break;
        }
        for (offset = 0; offset < length; offset += 8) {
            uint64_t word = ts_prng_next(&prng);
            size_t remaining = length - offset < 8 ? length - offset : 8;
            memcpy(bytes + offset, &word, remaining);
        }
        snprintf(label, sizeof(label), "generated[%d]", i);
        if (smoke(bytes, length, label) != 0) {
            failures++;
        }
        free(bytes);
    }

    if (failures) {
        fprintf(stderr, "%zu fuzz smoke input(s) failed\n", failures);
        return 1;
    }
    printf("fuzz smoke passed; %zu nodes traversed across all inputs\n", nodes_visited);
    return 0;
}
