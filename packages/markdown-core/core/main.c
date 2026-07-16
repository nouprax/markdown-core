#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "markdown-core.h"
#include "node.h"
#include "markdown-core-extension-api.h"
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

void print_usage(void) {
    printf("Usage:   markdown-core [FILE*]\n");
    printf("Options:\n");
    printf("  --smart           Use smart punctuation\n");
    printf("  --validate-utf8   Replace UTF-8 invalid sequences with U+FFFD\n");
    printf("  --strip-html-comments Strip HTML comment nodes from the parsed AST\n");
    printf("  --extension, -e EXTENSION_NAME  Specify an extension name to use\n");
    printf("  --list-extensions               List available extensions and quit\n");
    printf("  --strikethrough-double-tilde    Only parse strikethrough (if enabled)\n");
    printf("                                  with two tildes\n");
    printf("  --dollar-formula-delimiters     Enable formula $...$ and $$...$$\n"
           "                                  delimiters when formula is enabled.\n");
    printf("  --latex-formula-delimiters         Enable LaTeX formula \\\\(...\\\\) and\n"
           "                                  \\\\[...\\\\] delimiters when formula is enabled.\n");
    printf("  --directive                    Enable directive syntax.\n");
    printf("  --help, -h       Print usage information\n");
    printf("  --version        Print version\n");
}

static bool parser_has_extension(markdown_core_parser *parser, const char *name) {
    markdown_core_llist *tmp;

    for (tmp = parser->extensions; tmp; tmp = tmp->next) {
        markdown_core_extension *ext = (markdown_core_extension *)tmp->data;
        if (strcmp(ext->name, name) == 0) {
            return true;
        }
    }

    return false;
}

static bool attach_extension(markdown_core_parser *parser, const char *name) {
    markdown_core_extension *extension;

    if (parser_has_extension(parser, name)) {
        return true;
    }

    extension = markdown_core_find_extension(name);
    if (!extension) {
        fprintf(stderr, "Unknown extension %s\n", name);
        return false;
    }

    return markdown_core_parser_attach_extension(parser, extension) != 0;
}

static bool attach_option_extensions(markdown_core_parser *parser, int options) {
    if ((options & MARKDOWN_CORE_OPT_DIRECTIVE) && !attach_extension(parser, "directive")) {
        return false;
    }

    return true;
}

static bool print_document(markdown_core_node *document) {
    markdown_core_document facade_document = {document};
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_string_view message;

    if (!markdown_core_document_dump(&facade_document, &dump, &length, &error)) {
        message = markdown_core_error_get_message(error);
        fprintf(stderr, "AST dump failed: %.*s\n", (int)message.length,
                message.data ? (const char *)message.data : "unknown error");
        markdown_core_error_free(error);
        return false;
    }
    fwrite(dump, 1, length, stdout);
    markdown_core_dump_free(dump);
    return true;
}

static void print_extensions(void) {
    markdown_core_llist *extensions;
    markdown_core_llist *tmp;

    printf("Available extensions:\nfootnotes\n");

    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    extensions = markdown_core_list_extensions(mem);
    for (tmp = extensions; tmp; tmp = tmp->next) {
        markdown_core_extension *ext = (markdown_core_extension *)tmp->data;
        printf("%s\n", ext->name);
    }

    markdown_core_llist_free(mem, extensions);
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
            printf("markdown-core %s", MARKDOWN_CORE_VERSION_STRING);
            printf(" - CommonMark with GitHub Flavored Markdown converter\n(C) 2014-2016 John "
                   "MacFarlane\n");
            goto success;
        } else if (strcmp(argv[i], "--list-extensions") == 0) {
            print_extensions();
            goto success;
        } else if (strcmp(argv[i], "--dollar-formula-delimiters") == 0) {
            options |= MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS;
        } else if (strcmp(argv[i], "--latex-formula-delimiters") == 0) {
            options |= MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
        } else if (strcmp(argv[i], "--directive") == 0) {
            options |= MARKDOWN_CORE_OPT_DIRECTIVE;
        } else if (strcmp(argv[i], "--strikethrough-double-tilde") == 0) {
            options |= MARKDOWN_CORE_OPT_STRIKETHROUGH_DOUBLE_TILDE;
        } else if (strcmp(argv[i], "--smart") == 0) {
            options |= MARKDOWN_CORE_OPT_SMART;
        } else if (strcmp(argv[i], "--strip-html-comments") == 0) {
            options |= MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
        } else if (strcmp(argv[i], "--validate-utf8") == 0) {
            options |= MARKDOWN_CORE_OPT_VALIDATE_UTF8;
        } else if (strcmp(argv[i], "--liberal-html-tag") == 0) {
            options |= MARKDOWN_CORE_OPT_LIBERAL_HTML_TAG;
        } else if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage();
            goto success;
        } else if ((strcmp(argv[i], "-e") == 0) || (strcmp(argv[i], "--extension") == 0)) {
            i += 1; // Simpler to handle extensions in a second pass, as we can directly register
                    // them with the parser.

            if (i < argc && strcmp(argv[i], "footnotes") == 0) {
                options |= MARKDOWN_CORE_OPT_FOOTNOTES;
            }
        } else if (*argv[i] == '-') {
            print_usage();
            goto failure;
        } else { // treat as file argument
            files[numfps++] = i;
        }
    }

    parser = markdown_core_parser_new(options);

    if (!attach_option_extensions(parser, options)) {
        goto failure;
    }

    if (!attach_extension(parser, "table") || !attach_extension(parser, "strikethrough") ||
        !attach_extension(parser, "autolink") || !attach_extension(parser, "tasklist") ||
        !attach_extension(parser, "formula") || !attach_extension(parser, "directive")) {
        goto failure;
    }

    for (i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-e") == 0) || (strcmp(argv[i], "--extension") == 0)) {
            i += 1;
            if (i < argc) {
                if (strcmp(argv[i], "footnotes") == 0) {
                    continue;
                }
                if (!attach_extension(parser, argv[i])) {
                    goto failure;
                }
            } else {
                fprintf(stderr, "No argument provided for %s\n", argv[i - 1]);
                goto failure;
            }
        }
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
