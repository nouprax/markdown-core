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

// The CLI is a diagnostic dump tool: it always parses with the canonical
// default options and all bundled extensions, exactly like the platform
// bindings' default ParseOptions, and prints the canonical AST dump.
void print_usage(void) {
    printf("Usage:   markdown-core [FILE*]\n");
    printf("Parses Markdown from FILE arguments (or stdin) with the canonical\n");
    printf("default options and prints the canonical AST dump.\n");
    printf("Options:\n");
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
                  MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS | MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS |
                  MARKDOWN_CORE_OPT_DIRECTIVE | MARKDOWN_CORE_OPT_VALIDATE_UTF8;
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
        } else if (*argv[i] == '-') {
            print_usage();
            goto failure;
        } else { // treat as file argument
            files[numfps++] = i;
        }
    }

    parser = markdown_core_parser_new(options);

    if (!attach_extension(parser, "table") || !attach_extension(parser, "strikethrough") ||
        !attach_extension(parser, "autolink") || !attach_extension(parser, "tasklist") ||
        !attach_extension(parser, "formula") || !attach_extension(parser, "directive")) {
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
