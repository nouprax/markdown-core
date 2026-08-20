/* Deterministic fuzz smoke suite.
 *
 * Feeds fixed corpora and seeded pseudo-random byte streams through the
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

static int traverse(const markdown_core_node *node) {
    const markdown_core_node *child;
    markdown_core_scope scope;
    markdown_core_string_view view;
    markdown_core_optional_bool checked;
    int32_t level;
    bool flag;

    if (!node)
        return 0;
    nodes_visited++;
    (void)markdown_core_node_get_kind(node);
    (void)markdown_core_node_kind_name(markdown_core_node_get_kind(node));
    scope = markdown_core_node_scope(node);
    if (scope.start.line < 0 || scope.end.line < 0)
        return -1;
    (void)markdown_core_node_literal(node, &view);
    (void)markdown_core_node_heading_level(node, &level);
    (void)markdown_core_node_list_item_checked(node, &checked);
    (void)markdown_core_node_table_row_is_header(node, &flag);
    for (child = markdown_core_node_get_first_child(node); child; child = markdown_core_node_get_next_sibling(child))
        if (traverse(child) != 0)
            return -1;
    return 0;
}

static int smoke(const uint8_t *bytes, size_t length, const char *label) {
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    uint8_t *first = NULL;
    uint8_t *second = NULL;
    size_t first_length = 0;
    size_t second_length = 0;
    int result = -1;

    document = markdown_core_document_parse(bytes, length, NULL, &error);
    if (!document) {
        /* Parse failures must still produce a well-formed error object. */
        if (!error) {
            fprintf(stderr, "%s: parse failed without an error\n", label);
            return -1;
        }
        if (markdown_core_error_get_message(error).length == 0) {
            fprintf(stderr, "%s: parse error carries no diagnostic\n", label);
            markdown_core_error_free(error);
            return -1;
        }
        markdown_core_error_free(error);
        return 0;
    }

    if (traverse(markdown_core_document_root(document)) != 0) {
        fprintf(stderr, "%s: traversal produced an invalid scope\n", label);
        goto done;
    }
    if (!markdown_core_document_dump(document, &first, &first_length, &error) ||
        !markdown_core_document_dump(document, &second, &second_length, &error)) {
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
    markdown_core_error_free(error);
    return result;
}

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
            if (smoke(bytes, length, path) != 0)
                failures++;
            free(bytes);
        } else if (strcmp(argv[i], "--generated") == 0 && i + 1 < argc) {
            generated = (size_t)atoi(argv[++i]);
        } else {
            fputs("usage: fuzz_smoke_runner [--corpus FILE]... [--generated COUNT]\n", stderr);
            return 2;
        }
    }

    ts_prng_seed(&prng, UINT64_C(0x6D61726B646F776E)); /* "markdown" */
    for (i = 0; (size_t)i < generated; i++) {
        char label[64];
        size_t length = (size_t)(ts_prng_next(&prng) % 8192);
        uint8_t *bytes = (uint8_t *)malloc(length + 1);
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
        bytes[length] = 0;
        snprintf(label, sizeof(label), "generated[%d]", i);
        if (smoke(bytes, length, label) != 0)
            failures++;
        free(bytes);
    }

    if (failures) {
        fprintf(stderr, "%zu fuzz smoke input(s) failed\n", failures);
        return 1;
    }
    printf("fuzz smoke passed; %zu nodes traversed across all inputs\n", nodes_visited);
    return 0;
}
