#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "markdown-core.h"
#include "node.h"
#include "extension.h"
#include "parser.h"

#include "../extensions/markdown-core-extensions.h"
#include "../extensions/ast_internal.h"

#if defined(__OpenBSD__)
#include <sys/param.h>
#if OpenBSD >= 201605
#define USE_PLEDGE
#include <unistd.h>
#endif
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <io.h>
#include <fcntl.h>
#endif

// The CLI is a diagnostic dump tool: it parses with a named option profile and
// prints the canonical AST dump.
//
// `default` is the canonical default options and every bundled extension,
// exactly like the platform bindings' default ParseOptions. `gfm` is the subset
// Markdown Core shares with upstream cmark-gfm — this repository's own
// extensions off, and smart punctuation and HTML-comment stripping off because
// upstream defaults them off too. It exists so a reader can reproduce what the
// upstream-parity check compares (scripts/check-upstream-parity.mjs), and so
// any question about "is this ours or GFM's?" can be answered by running both.
// `gfm-smart` is that same subset with smart punctuation on, which upstream
// also offers as an opt-in flag; it exists so the smart-punctuation fixture has
// an upstream to be compared against rather than only its own golden dump.
void print_usage(void) {
    printf("Usage:   markdown-core [--profile NAME] [FILE*]\n");
    printf("Parses Markdown from FILE arguments (or stdin) and prints the\n");
    printf("canonical AST dump.\n");
    printf("Options:\n");
    printf("  --profile NAME   Option profile:\n");
    printf("                     default       every option and extension\n");
    printf("                     gfm           only what upstream cmark-gfm parses\n");
    printf("                     gfm-smart     gfm plus smart punctuation\n");
    printf("                     gfm-extended  gfm plus this repository's own extensions\n");
    printf("  --help, -h       Print usage information\n");
    printf("  --version        Print version\n");
}

static bool attach_extension(markdown_core_parser *parser, const char *name) {
    markdown_core_extension *extension = markdown_core_extension_find(name);

    if (!extension) {
        fprintf(stderr, "Unknown extension %s\n", name);
        return false;
    }

    return markdown_core_parser_attach_extension(parser, extension) != 0;
}

static bool print_document(markdown_core_node *document) {
    markdown_core_document facade_document = {document};
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_string_view message;

    if (!markdown_core_document_dump(&facade_document, &dump, &length, &error)) {
        message = markdown_core_error_get_message(error);
        fprintf(
            stderr,
            "AST dump failed: %.*s\n",
            (int)message.length,
            message.data ? (const char *)message.data : "unknown error"
        );
        markdown_core_error_free(error);
        return false;
    }
    fwrite(dump, 1, length, stdout);
    markdown_core_dump_free(dump);
    return true;
}

int main(int argc, char *argv[]) {
    int i, numfps = 0;
    int *files;
    char buffer[4096];
    markdown_core_parser *parser = NULL;
    size_t bytes;
    markdown_core_node *document = NULL;
    int options = MARKDOWN_CORE_OPT_SMART | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS |
                  MARKDOWN_CORE_OPT_DIRECTIVE | MARKDOWN_CORE_OPT_VALIDATE_UTF8;
    bool gfm_profile = false;
    int res = 1;

#ifdef USE_PLEDGE
    if (pledge("stdio rpath", NULL) != 0) {
        perror("pledge");
        return 1;
    }
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    files = (int *)calloc(argc, sizeof(*files));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("markdown-core %s\n", MARKDOWN_CORE_VERSION_STRING);
            goto success;
        } else if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage();
            goto success;
        } else if (strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                print_usage();
                goto failure;
            }
            i++;
            if (strcmp(argv[i], "gfm") == 0) {
                gfm_profile = true;
                options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_VALIDATE_UTF8;
            } else if (strcmp(argv[i], "gfm-smart") == 0) {
                gfm_profile = true;
                options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_VALIDATE_UTF8 | MARKDOWN_CORE_OPT_SMART;
            } else if (strcmp(argv[i], "gfm-extended") == 0) {
                options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_VALIDATE_UTF8 | MARKDOWN_CORE_OPT_DIRECTIVE;
            } else if (strcmp(argv[i], "default") != 0) {
                fprintf(stderr, "Unknown profile %s\n", argv[i]);
                goto failure;
            }
        } else if (*argv[i] == '-') {
            print_usage();
            goto failure;
        } else { // treat as file argument
            files[numfps++] = i;
        }
    }

    parser = markdown_core_parser_new(options);

    // Attachment order is priority, and `table` goes last for the reason given
    // in extensions/session.c: its row opener matches any line inside an open
    // table, so every narrower claim has to come first. The others here are
    // cmark-gfm's own; the repository's own extensions are off under the gfm
    // profile so a comparison against upstream compares the same language.
    if (!attach_extension(parser, "strikethrough") || !attach_extension(parser, "autolink") ||
        !attach_extension(parser, "tasklist")) {
        goto failure;
    }
    if ((!gfm_profile && (!attach_extension(parser, "formula") || !attach_extension(parser, "directive") ||
                          !attach_extension(parser, "cross_link") || !attach_extension(parser, "embed"))) ||
        !attach_extension(parser, "table")) {
        goto failure;
    }

    for (i = 0; i < numfps; i++) {
        FILE *fp = fopen(argv[files[i]], "rb");
        if (fp == NULL) {
            fprintf(stderr, "Error opening file %s: %s\n", argv[files[i]], strerror(errno));
            goto failure;
        }

        while ((bytes = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
            markdown_core_parser_feed(parser, buffer, bytes);
            if (bytes < sizeof(buffer)) {
                break;
            }
        }

        fclose(fp);
    }

    if (numfps == 0) {
        while ((bytes = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
            markdown_core_parser_feed(parser, buffer, bytes);
            if (bytes < sizeof(buffer)) {
                break;
            }
        }
    }

#ifdef USE_PLEDGE
    if (pledge("stdio", NULL) != 0) {
        perror("pledge");
        return 1;
    }
#endif

    document = markdown_core_parser_finish(parser);

    if (!document || !print_document(document)) {
        goto failure;
    }

success:
    res = 0;

failure:

    if (parser) {
        markdown_core_parser_free(parser);
    }

    if (document) {
        markdown_core_node_free(document);
    }

    free(files);

    return res;
}
