/* Delimiter-extension attachment-order invariants.
 *
 * Extension attachment enables a grammar; it does not define grammar
 * precedence. Parse the same shared-closer corpus under every permutation of
 * the bundled delimiter extensions and require a byte-identical canonical
 * AST.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ast_internal.h"
#include "extension.h"
#include "markdown-core-extension-api.h"
#include "markdown-core-extensions.h"
#include "markdown-core.h"

enum { DELIMITER_EXTENSION_COUNT = 5 };

static const char *const DELIMITER_EXTENSION_NAMES[DELIMITER_EXTENSION_COUNT] = {
    "strikethrough",
    "formula",
    "directive",
    "cross_link",
    "embed",
};

/* The reviewed extension fixtures independently lock each construct's AST.
 * This corpus composes every semantic claimant of `]` so only attachment
 * order varies:
 *
 * - Directive, CrossLink, Embed, Link, and Image nest in both directions.
 * - A newer `[[`/`![[` opener must decline a lone `]`, allowing the older
 *   directive/link/image claimant to consume it without discarding the newer
 *   opener.
 * - Formula and Strikethrough participate so every bundled delimiter
 *   extension is enabled in every permutation. */
static const char SHARED_CLOSE_CORPUS[] = ":note[outer [[inner]] and ![[asset]] tail]{k=v}\n"
                                          "[[outer :note[label]]]\n"
                                          "![[outer :note[label]]]\n"
                                          "[outer :note[label]](/link)\n"
                                          "![outer :note[label]](/image)\n"
                                          ":note[before [[dangling] tail\n"
                                          "[before [[dangling](/fallback)\n"
                                          "![before ![[dangling](/fallback.png)\n"
                                          "~~strike :note[label]~~ and $x + [[opaque]]$ and \\(y\\)\n";

static markdown_core_extension *extensions[DELIMITER_EXTENSION_COUNT];

static int load_extensions(void) {
    size_t i;

    for (i = 0; i < DELIMITER_EXTENSION_COUNT; i++) {
        extensions[i] = markdown_core_extension_find(DELIMITER_EXTENSION_NAMES[i]);
        if (!extensions[i]) {
            fprintf(stderr, "bundled extension '%s' is missing\n", DELIMITER_EXTENSION_NAMES[i]);
            return 0;
        }
        if (!extensions[i]->delimiter_rule_count) {
            fprintf(stderr, "bundled extension '%s' no longer owns a delimiter rule\n", DELIMITER_EXTENSION_NAMES[i]);
            return 0;
        }
    }
    return 1;
}

static int dump_permutation(const size_t order[DELIMITER_EXTENSION_COUNT], uint8_t **dump, size_t *length) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    markdown_core_document document;
    markdown_core_error *error = NULL;
    size_t i;
    int ok;

    parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DIRECTIVE);
    if (!parser) {
        fputs("could not allocate parser\n", stderr);
        return 0;
    }
    for (i = 0; i < DELIMITER_EXTENSION_COUNT; i++) {
        if (!markdown_core_parser_attach_extension(parser, extensions[order[i]])) {
            fprintf(stderr, "could not attach extension '%s'\n", DELIMITER_EXTENSION_NAMES[order[i]]);
            markdown_core_parser_free(parser);
            return 0;
        }
    }

    markdown_core_parser_feed(parser, SHARED_CLOSE_CORPUS, sizeof(SHARED_CLOSE_CORPUS) - 1);
    root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!root) {
        fputs("permutation parse failed\n", stderr);
        return 0;
    }

    document.root = root;
    ok = markdown_core_document_dump(&document, dump, length, &error);
    markdown_core_error_free(error);
    markdown_core_node_free(root);
    if (!ok) {
        fputs("permutation AST dump failed\n", stderr);
    }
    return ok;
}

static int next_permutation(size_t values[DELIMITER_EXTENSION_COUNT]) {
    size_t pivot = DELIMITER_EXTENSION_COUNT - 1;
    size_t successor;
    size_t left;
    size_t right;

    while (pivot > 0 && values[pivot - 1] >= values[pivot]) {
        pivot--;
    }
    if (pivot == 0) {
        return 0;
    }
    pivot--;

    successor = DELIMITER_EXTENSION_COUNT - 1;
    while (values[successor] <= values[pivot]) {
        successor--;
    }
    {
        size_t swap = values[pivot];
        values[pivot] = values[successor];
        values[successor] = swap;
    }

    left = pivot + 1;
    right = DELIMITER_EXTENSION_COUNT - 1;
    while (left < right) {
        size_t swap = values[left];
        values[left++] = values[right];
        values[right--] = swap;
    }
    return 1;
}

static void print_order(const size_t order[DELIMITER_EXTENSION_COUNT]) {
    size_t i;

    fputs("attachment order:", stderr);
    for (i = 0; i < DELIMITER_EXTENSION_COUNT; i++) {
        fprintf(stderr, " %s", DELIMITER_EXTENSION_NAMES[order[i]]);
    }
    fputc('\n', stderr);
}

int main(void) {
    size_t order[DELIMITER_EXTENSION_COUNT] = {0, 1, 2, 3, 4};
    uint8_t *baseline = NULL;
    size_t baseline_length = 0;
    size_t permutations = 0;

    if (!load_extensions() || !dump_permutation(order, &baseline, &baseline_length)) {
        return 1;
    }

    do {
        uint8_t *actual = NULL;
        size_t actual_length = 0;
        size_t difference = 0;

        permutations++;
        if (!dump_permutation(order, &actual, &actual_length)) {
            print_order(order);
            markdown_core_dump_free(baseline);
            return 1;
        }
        while (difference < baseline_length && difference < actual_length &&
               baseline[difference] == actual[difference]) {
            difference++;
        }
        if (baseline_length != actual_length || difference != baseline_length) {
            fprintf(stderr, "canonical AST changed at byte %zu\n", difference);
            print_order(order);
            markdown_core_dump_free(actual);
            markdown_core_dump_free(baseline);
            return 1;
        }
        markdown_core_dump_free(actual);
    } while (next_permutation(order));

    markdown_core_dump_free(baseline);
    printf("%zu delimiter-extension attachment permutations produced one canonical AST\n", permutations);
    return permutations == 120 ? 0 : 1;
}
