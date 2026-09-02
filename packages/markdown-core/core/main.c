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
    printf("  --profile PROFILE named option set: commonmark | commonmark-smart | default | gfm | "
           "gfm-smart | gfm-extended\n");
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
    /* The CLI owns the native tree directly, so the facade handle is only a
     * stack wrapper used by the canonical dump operation. */
    markdown_core_document facade_document = {.root = document};
    markdown_core_error *error = NULL;
    uint8_t *dump = NULL;
    size_t length = 0;
    markdown_core_string message;

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

typedef struct cli_parse_setup {
    unsigned extensions;
} cli_parse_setup;

static bool configure_cli_parse(markdown_core_parser *parser, void *context) {
    cli_parse_setup *setup = (cli_parse_setup *)context;

    if (!markdown_core_core_extensions_attach(parser, setup->extensions)) {
        return false;
    }
    return true;
}

static bool read_all(markdown_core_strbuf *source, FILE *input) {
    unsigned char buffer[4096];
    size_t bytes;

    while ((bytes = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        markdown_core_strbuf_put(source, buffer, (bufsize_t)bytes);
        if (source->oom) {
            return false;
        }
    }
    return ferror(input) == 0;
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
    markdown_core_strbuf source;
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
    bool commonmark_profile = false;
    unsigned requested_extensions = 0;
    unsigned extensions;
    cli_parse_setup setup = {0};

    markdown_core_strbuf_init(markdown_core_get_default_mem_allocator(), &source, 0);
    files = (int *)calloc(argc, sizeof(*files));
    if (!files) {
        goto failure;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("markdown-core %s", MARKDOWN_CORE_VERSION_STRING);
            printf(" - CommonMark with GitHub Flavored Markdown converter\n(C) 2014-2016 John "
                   "MacFarlane\n");
            goto success;
        } else if (strcmp(argv[i], "--profile") == 0) {
            /* A NAMED OPTION SET, so a comparison harness can ask for exactly
             * one language without knowing which flags spell it. `commonmark`
             * turns every syntax extension off for the reference cmark
             * oracle. `gfm` adds only the cmark-gfm extension set, and
             * `gfm-extended` adds this repository's own syntax.
             *
             * Every existing invocation is unaffected: without this flag the
             * parser is built exactly as before. */
            if (i + 1 >= argc) {
                print_usage();
                goto failure;
            }
            i++;
            /* A profile is a complete named set. If more than one is supplied,
             * the last one replaces the previous set rather than leaving a
             * hidden extension-selection bit behind. */
            commonmark_profile = false;
            gfm_profile = false;
            requested_extensions = 0;
            if (strcmp(argv[i], "commonmark") == 0) {
                commonmark_profile = true;
                gfm_profile = true;
                options = 0;
            } else if (strcmp(argv[i], "commonmark-smart") == 0) {
                commonmark_profile = true;
                gfm_profile = true;
                options = MARKDOWN_CORE_OPT_SMART;
            } else if (strcmp(argv[i], "gfm") == 0) {
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
            } else if (strcmp(argv[i], "default") == 0) {
                options = MARKDOWN_CORE_OPT_SMART | MARKDOWN_CORE_OPT_FOOTNOTES |
                          MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS | MARKDOWN_CORE_OPT_VALIDATE_UTF8;
            } else {
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

    /* The CLI says WHICH extensions and cannot say in what order; the order is
     * `core-extensions.c`'s, and it is the facade's too. Before D15 was fixed
     * these were two different orders and therefore two different languages --
     * the CLI attached `directive` FIRST, the facade attached it LAST, and
     * every binding goes through the facade.
     *
     * A CommonMark profile has no syntax extensions. This repository's own two
     * are off under the GFM profiles, leaving exactly the extension set shared
     * with cmark-gfm. */
    extensions = commonmark_profile ? 0
                                    : MARKDOWN_CORE_CORE_EXTENSION_TABLE | MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH |
                                          MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK | MARKDOWN_CORE_CORE_EXTENSION_TASKLIST;
    if (!gfm_profile) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_FORMULA | MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE;
    }
    extensions |= requested_extensions;

    for (i = 0; i < numfps; i++) {
        FILE *fp = fopen(argv[files[i]], "rb");
        if (fp == NULL) {
            fprintf(stderr, "Error opening file %s: %s\n", argv[files[i]], strerror(errno));
            goto failure;
        }

        if (!read_all(&source, fp)) {
            fprintf(stderr, "Error reading file %s\n", argv[files[i]]);
            fclose(fp);
            goto failure;
        }
        fclose(fp);
    }

    if (numfps == 0 && !read_all(&source, stdin)) {
        fputs("Error reading standard input\n", stderr);
        goto failure;
    }

#ifdef USE_PLEDGE
    if (pledge("stdio", NULL) != 0) {
        perror("pledge");
        return 1;
    }
#endif

    setup.extensions = extensions;
    document = markdown_core_parse_document_with_mem(source.size ? (const char *)source.ptr : NULL, (size_t)source.size,
                                                     options, markdown_core_get_default_mem_allocator(),
                                                     configure_cli_parse, &setup);

    if (!document || !print_document(document)) {
        goto failure;
    }

success:
    res = 0;

failure:

    if (document) {
        markdown_core_node_free(document);
    }

    free(files);
    markdown_core_strbuf_free(&source);

    return res;
}
