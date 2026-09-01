/* CRITERION 1 AS A GATE: stream it, finish, and the document equals the
 * one-shot.
 *
 * §3's acceptance for Stage 1 is that feeding a document in pieces and calling
 * `finish` produces the tree a one-shot parse produces. That is testable today
 * — `feed` and `finish` already exist — and it needs NO NEW EXPECTATIONS: this
 * compares the two feeds against each other, over the same fixture corpus the
 * goldens already pin, so a fixture example is a streaming case for free.
 *
 * Both sides go through the RAW PARSER rather than the facade, because the
 * facade has no chunked entry point (H1: `finish` is the only exit). That is
 * also why this does not compare against the expected block: `spec_runner`
 * already owns that comparison, and duplicating the facade's option-to-
 * extension mapping here would put a second copy of engine logic in a test.
 * What this asserts is the property the facade cannot yet express — that the
 * partition does not matter.
 *
 *   stream_runner --spec FILE [--bytes]
 *
 * `--bytes` feeds one byte at a time instead of one line at a time, which is
 * stronger than §3 asks: it partitions INSIDE a line, which is Stage 2's
 * territory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#include <markdown_core.h>

#include "markdown-core.h"
#include "markdown-core-extensions.h"

#include "ast_internal.h"

enum feed_mode { FEED_WHOLE, FEED_BY_LINE, FEED_BY_BYTE };

static uint8_t *dump_fed(const char *text, size_t length, enum feed_mode mode, size_t *dump_length) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *root;
    markdown_core_document facade;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;

    if (!parser) {
        return NULL;
    }
    /* Every extension the fixtures use, attached for both feeds alike. The
     * point of comparison is the partition, so the option set only has to be
     * the same on both sides. */
    if (!markdown_core_core_extensions_attach(
            parser,
            MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST
        )) {
        markdown_core_parser_free(parser);
        return NULL;
    }

    if (mode == FEED_BY_BYTE) {
        size_t i;
        for (i = 0; i < length; i++) {
            markdown_core_parser_feed(parser, text + i, 1);
        }
    } else if (mode == FEED_BY_LINE) {
        size_t start = 0;
        size_t i;
        for (i = 0; i < length; i++) {
            if (text[i] == '\n') {
                markdown_core_parser_feed(parser, text + start, i - start + 1);
                start = i + 1;
            }
        }
        if (start < length) {
            markdown_core_parser_feed(parser, text + start, length - start);
        }
    } else {
        markdown_core_parser_feed(parser, text, length);
    }

    root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!root) {
        return NULL;
    }
    /* The CLI's own `print_document` does exactly this: the dump reads the
     * root, and the concrete view is printed elsewhere. */
    memset(&facade, 0, sizeof(facade));
    facade.root = root;
    if (!markdown_core_document_dump(&facade, &dump, dump_length, &error)) {
        markdown_core_error_free(error);
        markdown_core_node_free(root);
        return NULL;
    }
    markdown_core_node_free(root);
    return dump;
}

int main(int argc, char **argv) {
    const char *spec = NULL;
    enum feed_mode mode = FEED_BY_LINE;
    const char *label = "line";
    ts_spec_file file;
    size_t index;
    size_t agreed = 0;
    int failures = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spec") == 0 && i + 1 < argc) {
            spec = argv[++i];
        } else if (strcmp(argv[i], "--bytes") == 0) {
            mode = FEED_BY_BYTE;
            label = "byte";
        } else {
            fprintf(stderr, "usage: stream_runner --spec FILE [--bytes]\n");
            return 2;
        }
    }
    if (!spec || ts_spec_load(spec, &file) != 0) {
        fprintf(stderr, "stream_runner: cannot load %s\n", spec ? spec : "(no --spec)");
        return 2;
    }

    for (index = 0; index < file.count; index++) {
        const ts_spec_case *test_case = &file.cases[index];
        size_t whole_length = 0;
        size_t fed_length = 0;
        uint8_t *whole = dump_fed(test_case->markdown, test_case->markdown_length, FEED_WHOLE, &whole_length);
        uint8_t *fed = dump_fed(test_case->markdown, test_case->markdown_length, mode, &fed_length);

        if (!whole || !fed) {
            fprintf(stderr, "example %d: parse or dump failed\n", test_case->example);
            failures++;
        } else if (whole_length != fed_length || memcmp(whole, fed, whole_length) != 0) {
            fprintf(
                stderr,
                "example %d (line %d): one-%s feed differs from one-shot\n",
                test_case->example,
                test_case->start_line,
                label
            );
            fprintf(stderr, "  one-shot:\n%.*s", (int)whole_length, (const char *)whole);
            fprintf(stderr, "  one-%s:\n%.*s", label, (int)fed_length, (const char *)fed);
            failures++;
        } else {
            agreed++;
        }
        markdown_core_dump_free(whole);
        markdown_core_dump_free(fed);
    }

    {
        /* The count is read BEFORE the free, which the first cut of this got
         * wrong and printed `669/0`. */
        size_t total = file.count;
        ts_spec_free(&file);
        printf("stream equals one-shot [one-%s]: %zu/%zu examples agree\n", label, agreed, total);
    }
    return failures ? 1 : 0;
}
