/* CRITERION 1, EXACTLY AS STATED: stream it, finish, and the document equals
 * the one-shot.
 *
 * Feed the same bytes two ways through the same parser API — once whole, once
 * one line at a time — call `finish` on both, dump both, and compare. The
 * CLI's own `print_document` wraps a parser-finished node in a stack
 * `markdown_core_document` to dump it; this does the same, so the two dumps are
 * the same text a golden pins.
 *
 * No engine change is needed to run this: `feed` and `finish` already exist.
 * What Stage 1 adds is reading the tree WITHOUT finishing, which is a different
 * thing and is not what the golden test asserts.
 *
 *   cc -o stream-eq stage1-stream-equals-oneshot.c -I... -lmarkdown-core ...
 *   stream-eq FILE...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <markdown_core.h>

#include "markdown-core.h"
#include "markdown-core-extensions.h"
#include "ast_internal.h"

/* `mode`: 0 whole, 1 one line at a time, 2 one BYTE at a time. Byte-at-a-time
 * is stronger than the criterion asks — it partitions inside a line, which is
 * Stage 2's territory — and it is here because it costs five lines and says
 * whether the property is about line boundaries or about feeding at all. */
static char *dump_of(const char *text, size_t length, int mode) {
    markdown_core_parser *parser = markdown_core_parser_new(MARKDOWN_CORE_OPT_DEFAULT);
    markdown_core_node *root;
    markdown_core_document facade;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t dump_length = 0;
    char *copy = NULL;

    if (!parser) {
        return NULL;
    }
    if (!markdown_core_core_extensions_attach(parser, MARKDOWN_CORE_CORE_EXTENSION_TABLE |
                                                          MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                                                          MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK |
                                                          MARKDOWN_CORE_CORE_EXTENSION_TASKLIST)) {
        markdown_core_parser_free(parser);
        return NULL;
    }

    if (mode == 2) {
        size_t i;
        for (i = 0; i < length; i++) {
            markdown_core_parser_feed(parser, text + i, 1);
        }
    } else if (mode == 1) {
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
    memset(&facade, 0, sizeof(facade));
    facade.root = root;
    if (markdown_core_document_dump(&facade, &dump, &dump_length, &error)) {
        copy = (char *)malloc(dump_length + 1);
        if (copy) {
            memcpy(copy, dump, dump_length);
            copy[dump_length] = '\0';
        }
        markdown_core_dump_free(dump);
    }
    markdown_core_error_free(error);
    markdown_core_node_free(root);
    return copy;
}

int main(int argc, char **argv) {
    int file;
    size_t examples = 0;
    size_t differing = 0;
    size_t differing_bytes = 0;
    for (file = 1; file < argc; file++) {
        FILE *fp = fopen(argv[file], "rb");
        char *text;
        long size;
        char *whole;
        char *streamed;
        char *bytewise;
        if (!fp) {
            fprintf(stderr, "cannot open %s\n", argv[file]);
            return 2;
        }
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        text = (char *)malloc((size_t)size + 1);
        if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size) {
            fclose(fp);
            free(text);
            return 2;
        }
        text[size] = '\0';
        fclose(fp);

        whole = dump_of(text, (size_t)size, 0);
        streamed = dump_of(text, (size_t)size, 1);
        bytewise = dump_of(text, (size_t)size, 2);
        examples++;
        if (!whole || !streamed || strcmp(whole, streamed) != 0) {
            differing++;
            printf("DIFFERS by line: %s\n", argv[file]);
        }
        if (!whole || !bytewise || strcmp(whole, bytewise) != 0) {
            differing_bytes++;
            printf("DIFFERS by byte: %s\n", argv[file]);
        }
        free(whole);
        free(streamed);
        free(bytewise);
        free(text);
    }
    printf("stream-equals-oneshot: line partitions %zu/%zu agree, byte partitions %zu/%zu agree\n",
           examples - differing, examples, examples - differing_bytes, examples);
    return differing || differing_bytes ? 1 : 0;
}
