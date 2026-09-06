/* The conformance harness's command-line adapter, built by the test tree and
 * never installed.
 *
 *   markdown-core-harness [--profile NAME] [--feature NAME | -e NAME]...
 *                         [--list-features] [FILE*]
 *
 * The installed `markdown-core` parses the one dialect and takes no flags.
 * The oracle gates need something the product deliberately does not offer:
 * the base layer alone for cmark, and the GFM layer alone for cmark-gfm. This
 * executable is that lever. It starts from the base layer, `--profile` replaces
 * the selection with a layer of the feature registry (`commonmark`, `gfm`,
 * `gfm-extended`, `default`), and `--feature` or its short form `-e` adds one
 * registered feature; a later `--profile` replaces everything selected before
 * it, so a script that names a profile gets exactly that layer. The names are
 * the registry's, so an unregistered name fails instead of parsing another
 * language. Input is the concatenation of the files, or standard input, and
 * the output is the canonical AST dump.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#endif

static void usage(FILE *stream) {
    fputs("usage: markdown-core-harness [--profile commonmark|gfm|gfm-extended|default]\n"
          "                             [--feature NAME | -e NAME]... [--list-features] [FILE*]\n",
          stream);
}

typedef struct source_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} source_buffer;

static int read_all(source_buffer *source, FILE *input) {
    uint8_t chunk[4096];
    size_t bytes;
    while ((bytes = fread(chunk, 1, sizeof(chunk), input)) > 0) {
        if (source->size + bytes > source->capacity) {
            size_t capacity = source->capacity ? source->capacity : 4096;
            uint8_t *grown;
            while (capacity < source->size + bytes) {
                capacity *= 2;
            }
            grown = (uint8_t *)realloc(source->data, capacity);
            if (!grown) {
                return -1;
            }
            source->data = grown;
            source->capacity = capacity;
        }
        memcpy(source->data + source->size, chunk, bytes);
        source->size += bytes;
    }
    return ferror(input) ? -1 : 0;
}

int main(int argc, char **argv) {
    markdown_core_feature_set features;
    source_buffer source = {NULL, 0, 0};
    markdown_core_document *document = NULL;
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    int file_count = 0;
    int result = 1;
    int i;

#if defined(_WIN32) && !defined(__CYGWIN__)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    ts_ast_features_none(&features);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            if (ts_ast_profile(&features, argv[++i]) != 0) {
                fprintf(stderr, "markdown-core-harness: unknown profile %s\n", argv[i]);
                goto done;
            }
        } else if ((strcmp(argv[i], "--feature") == 0 || strcmp(argv[i], "-e") == 0) && i + 1 < argc) {
            if (ts_ast_feature_enable(&features, argv[++i]) != 0) {
                fprintf(stderr, "markdown-core-harness: unknown feature %s\n", argv[i]);
                goto done;
            }
        } else if (strcmp(argv[i], "--list-features") == 0) {
            const markdown_core_feature *feature;
            size_t index;
            for (index = 0; (feature = markdown_core_feature_at(index)) != NULL; index++) {
                printf("%s\n", feature->name);
            }
            result = 0;
            goto done;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            result = 0;
            goto done;
        } else if (argv[i][0] == '-' && argv[i][1] != 0) {
            usage(stderr);
            goto done;
        } else {
            FILE *file = fopen(argv[i], "rb");
            if (!file) {
                fprintf(stderr, "markdown-core-harness: cannot open %s: %s\n", argv[i], strerror(errno));
                goto done;
            }
            if (read_all(&source, file) != 0) {
                fprintf(stderr, "markdown-core-harness: cannot read %s\n", argv[i]);
                fclose(file);
                goto done;
            }
            fclose(file);
            file_count++;
        }
    }
    if (file_count == 0 && read_all(&source, stdin) != 0) {
        fputs("markdown-core-harness: cannot read standard input\n", stderr);
        goto done;
    }

    document = ts_ast_parse(source.data, source.size, features);
    if (!document) {
        goto done;
    }
    if (!markdown_core_document_dump(document, &dump, &length, &error)) {
        markdown_core_string message = markdown_core_error_get_message(error);
        fprintf(stderr, "markdown-core-harness: dump failed: %.*s\n", (int)message.length,
                message.data ? (const char *)message.data : "unknown error");
        markdown_core_error_free(error);
        goto done;
    }
    fwrite(dump, 1, length, stdout);
    result = 0;

done:
    markdown_core_dump_free(dump);
    markdown_core_document_free(document);
    free(source.data);
    return result;
}
