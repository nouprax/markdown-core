#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "markdown-core.h"
#include "node.h"
#include "markdown-core-extension-api.h"
#include "syntax_extension.h"
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
    printf("  --profile PROFILE named option set: default | gfm | gfm-smart | gfm-extended\n");
    printf("  --smart           Use smart punctuation\n");
    printf("  --validate-utf8   Replace UTF-8 invalid sequences with U+FFFD\n");
    printf("  --strip-html-comments Strip HTML comment nodes from the parsed AST\n");
    printf("  --extension, -e EXTENSION_NAME  Specify an extension name to use\n");
    printf("  --list-extensions               List available extensions and quit\n");
    printf("  --strikethrough-double-tilde    Only parse strikethrough (if enabled)\n");
    printf("                                  with two tildes\n");
    printf("  --help, -h       Print usage information\n");
    printf("  --version        Print version\n");
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
    size_t i;
    const char *name;

    printf("Available extensions:\nfootnotes\n");
    for (i = 0; (name = markdown_core_core_extensions_name_at(i)) != NULL; i++) {
        printf("%s\n", name);
    }
}

int main(int argc, char *argv[]) {
    int i, numfps = 0;
    int *files;
    char buffer[4096];
    markdown_core_parser *parser = NULL;
    size_t bytes;
    markdown_core_node *document = NULL;
    int options = MARKDOWN_CORE_OPT_SMART | MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS |
                  MARKDOWN_CORE_OPT_VALIDATE_UTF8;
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

    bool gfm_profile = false;
    unsigned requested_extensions = 0;
    unsigned extensions;

    files = (int *)calloc(argc, sizeof(*files));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("markdown-core %s", MARKDOWN_CORE_VERSION_STRING);
            printf(" - CommonMark with GitHub Flavored Markdown converter\n(C) 2014-2016 John "
                   "MacFarlane\n");
            goto success;
        } else if (strcmp(argv[i], "--profile") == 0) {
            /* A NAMED OPTION SET, so a comparison harness can ask for exactly
             * one language without knowing which flags spell it. `gfm` is the
             * subset shared with upstream cmark-gfm — this repository's own
             * extensions off, so a parity run compares one language and not
             * two. `gfm-extended` is that plus this repository's own.
             *
             * Every existing invocation is unaffected: without this flag the
             * parser is built exactly as before. */
            if (i + 1 >= argc) {
                print_usage();
                goto failure;
            }
            i++;
            if (strcmp(argv[i], "gfm") == 0) {
                gfm_profile = true;
                options = MARKDOWN_CORE_OPT_FOOTNOTES;
            } else if (strcmp(argv[i], "gfm-smart") == 0) {
                gfm_profile = true;
                options = MARKDOWN_CORE_OPT_FOOTNOTES | MARKDOWN_CORE_OPT_SMART;
            } else if (strcmp(argv[i], "gfm-extended") == 0) {
                /* `gfm_profile` stays false, and that is what attaches this
                 * repository's own two extensions below -- there is no option
                 * bit left to spell it with (Q14). */
                options = MARKDOWN_CORE_OPT_FOOTNOTES;
            } else if (strcmp(argv[i], "default") != 0) {
                fprintf(stderr, "Unknown profile %s\n", argv[i]);
                goto failure;
            }
        } else if (strcmp(argv[i], "--list-extensions") == 0) {
            print_extensions();
            goto success;
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
            i += 1;

            /* `-e NAME` is a request for an extension, not for a position in
             * the attach order. Turning the name into a BIT is what keeps that
             * true: the extension attaches where `core-extensions.c` puts it,
             * not where the flag appeared, and this file has no way to say
             * otherwise -- which is the whole of D15's fix. It used to attach
             * by name in a second pass, i.e. after everything else, so
             * `-e formula` under `--profile gfm` bought a different language
             * from the one the same set gives the facade. */
            if (i >= argc) {
                fprintf(stderr, "No argument provided for %s\n", argv[i - 1]);
                goto failure;
            }
            if (strcmp(argv[i], "footnotes") == 0) {
                options |= MARKDOWN_CORE_OPT_FOOTNOTES;
            } else {
                unsigned bit = markdown_core_core_extensions_bit(argv[i]);
                if (!bit) {
                    fprintf(stderr, "Unknown extension %s\n", argv[i]);
                    goto failure;
                }
                requested_extensions |= bit;
            }
        } else if (*argv[i] == '-') {
            print_usage();
            goto failure;
        } else { // treat as file argument
            files[numfps++] = i;
        }
    }

    parser = markdown_core_parser_new(options);

    /* The CLI says WHICH extensions and cannot say in what order; the order is
     * `core-extensions.c`'s, and it is the facade's too. Before D15 was fixed
     * these were two different orders and therefore two different languages --
     * the CLI attached `directive` FIRST, the facade attached it LAST, and
     * every binding goes through the facade.
     *
     * This repository's own two are off under the gfm profiles so a parity run
     * against upstream compares one language; the condition is exactly the one
     * the two old attach sites spelled between them. */
    extensions = MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                 MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST;
    if (!gfm_profile) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE;
    }
    extensions |= requested_extensions;

    if (!markdown_core_core_extensions_attach(parser, extensions)) {
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

    // Registered extensions are process-lifetime by contract; the OS reclaims
    // them at exit.

    free(files);

    return res;
}
