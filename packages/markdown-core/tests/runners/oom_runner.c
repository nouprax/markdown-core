/* Strict allocation-failure contract for the consumer-facing parse.
 *
 * The injected allocator refuses each allocation in turn. Every refusal must
 * produce no document and a non-allocating ALLOCATION_FAILED error; a
 * byte-identical document is still a contract violation because OOM has no
 * fallback path.
 *
 *   oom_runner --case strict_oom
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast_internal.h"

static unsigned long allocation_count;
static unsigned long fail_at;
static int failure_fired;

static void *sweep_calloc(size_t count, size_t size) {
    if (++allocation_count == fail_at) {
        failure_fired = 1;
        return NULL;
    }
    return calloc(count, size);
}

static void *sweep_realloc(void *pointer, size_t size) {
    if (++allocation_count == fail_at) {
        failure_fired = 1;
        return NULL;
    }
    return realloc(pointer, size);
}

static void sweep_free(void *pointer) { free(pointer); }

static markdown_core_mem sweep_mem = {sweep_calloc, sweep_realloc, sweep_free};

static const char OOM_PROPERTIES_CORPUS[] =
    "---\n# ignored\nname: \"large\\u0020text\"\nunknown: *a\nauthors: [1, \"two\", three]\n"
    "keywords: [true]\nkeywords: [\"\\uD83D\\uDE80\"]\nname: duplicate\n...\nnot YAML\n"
    "abstract: |\n  one\n\n  two\ncomment: &a [true]\ncomment: |\n  prose\nstate: true\n---\nbody\n";
static const char OOM_PROPERTIES_ARRAY_CORPUS[] = "---\nname: x[\nauthors: [\"two\", 3]\nabstract: {nested: 1}\n"
                                                  "date: 4\ncomment: \"bad\\q\"\nkeywords: []\nstate: ready\n---\n";

static const char OOM_CORPUS[] = "[[Note]] [[Note|]] ![[#^block|alias]] [[a#Heading|label]]\n\n"
                                 "Setext heading\n---\n\n"
                                 "# Heading *one*\n"
                                 "\n"
                                 "Paragraph with **strong**, _em_, `code`, [link](/url \"title\"), ![img](/i.png),\n"
                                 "a [ref][label], an <https://example.com/auto> autolink, www.example.com,\n"
                                 "mail@example.com, https://example.com/bare, ~~gone~~, &amp; entity.\n"
                                 "\n"
                                 "[label]: /dest \"tt\"\n"
                                 "[label]: /dup\n"
                                 "\n"
                                 "> quote with footnote[^fn] and $x+y$ inline formula\n"
                                 "\n"
                                 "- [ ] task open\n"
                                 "- [x] task done\n"
                                 "\n"
                                 "lead text\n"
                                 "| a | b |\n"
                                 "| - | - |\n"
                                 "| 1 | 2 |\n"
                                 "| x\\|y &amp; user@example.com | `\\|` *after* |\n"
                                 "\n"
                                 "```formula\n"
                                 "x^2\n"
                                 "```\n"
                                 "\n"
                                 ":::note[Label]{k=1 k=2 other=\"v\"}\n"
                                 "directive body\n"
                                 ":::\n"
                                 "\n"
                                 ":inline{#first id=last class=\"a b a\" .c a=1 b=2 a=3 k=\"&amp;\"}\n"
                                 "\n"
                                 "[^fn]: footnote *body*\n"
                                 "\n"
                                 /* Exercises one-shot embedded-NUL normalization. */
                                 "text before \0 text after\n"
                                 "\n"
                                 "<!-- comment -->\n";

static const char OOM_LINE_AND_CORE_CORPUS[] = "\xef\xbb\xbf# bom\r\n"
                                               "> quote\r"
                                               "\tindented\n"
                                               "<div>\nraw\n</div>\n"
                                               "bad utf8: \xff\xfe\n"
                                               "nul a\0b\0c\r\n"
                                               "***nested _delimiters_*** and [shortcut]\n"
                                               "\n"
                                               "[shortcut]: <https://example.com/a(b)> 'title'\n";

static const char OOM_EXTENSION_EDGE_CORPUS[] = ":inline[label]{.a class=\"\" .b id=x id=y entity=&amp;}\n"
                                                "\n"
                                                ":::outer[lab]{empty=\"\" key='value'}\n"
                                                ":::inner\n"
                                                "body \\(x+y\\) and $`z`$\n"
                                                ":::\n"
                                                ":::\n"
                                                "\n"
                                                "| escaped \\| pipe | second |\n"
                                                "| :--- | ---: |\n"
                                                "| a | b |\n";

/* Nested values, field owners, all reserved suffixes, and unsuccessful
 * candidates must remain one all-or-nothing ownership transaction. */
static const char OOM_INLINE_FOOTNOTE_CORPUS[] =
    "^[a ^[b [^x]] www.example.com] :d[^[label]] ^[] ^[ \t ] ^[open\n\n"
    "[outer ^[[inner](u)]](v) ^[==mark== %%comment%%]\n\n"
    "[^inline-1]: reserved\n[^inline-1-1]: suffix\n[^inline-1-2]: suffix\n[^x]: ^[nested [^x]]\n\n"
    "^[http://example.com] :d[^[www.example.com]] ^[outer :d[^[inner]]]\n";

static const char OOM_TASK_CORPUS[] = "- [é] two\n- [✓] three\n- [🚀] four\n"
                                      "> 1. [?] # heading\n>    - [-] nested\n"
                                      "- [✓] \t\v\f\noutside\n"
                                      "- [] empty\n- [é] multiple\n- [?]none\n";

static const char OOM_MARK_CORPUS[] = "===a *b*=== ==c====d== ==[link](/u) <!--c-->==\n\n"
                                      "| ==h== |\n| --- |\n| ==x\\|y== |\n\ncall[^n]\n\n[^n]: ==note==\n";

/* O3: both `%%` forms, the lookahead's chain and cache in nested containers, a
 * candidate that fails at its container's end, the last line without a line
 * ending, and comments beside every earlier opaque construct. */
static const char OOM_COMMENT_CORPUS[] = "a %%b%% %%%% %%%c%%% \\%%d%% %%x\ny%% `%%` $%%$ <!--%%--> [[%%]] ==%%m%%==\n"
                                         "\n"
                                         "%%\n"
                                         "block *a*\n"
                                         "%%\n"
                                         "\n"
                                         "> - %%\n"
                                         ">   q\n"
                                         ">\n"
                                         ">   %%\n"
                                         "> - %%\n"
                                         ">   open\n"
                                         "\n"
                                         "| %%c\\|d%% | %%%% |\n"
                                         "| - | - |\n"
                                         "\n"
                                         "[^n]: %%\n"
                                         "    f\n"
                                         "    %%\n"
                                         "\n"
                                         "- %%\n"
                                         "\n"
                                         "  %%";

typedef struct oom_case {
    const char *name;
    const char *source;
    size_t length;
} oom_case;

static const char OOM_BLOCK_IDENTIFIER_CORPUS[] =
    "text #paragraph#\n\n- [✓] item #item#\n- second\n  line #p#\n\n#list#\n\n"
    "> - nested\n>\n> #nested#\n>\n> after\n\n#quote#\n\n"
    "lead\n   #lead#\n| h |\n| - |\n| c |\n\n#table#\n\n#duplicate#\n\n"
    "[a]: /a\n[b]: /b \"title\"\n| [a] |\n| - |\n\n[b]\n\n"
    "[c]: /c\nlead [c] #lead-ref#\n| h |\n| - |\n\n"
    "- blocked\n\n[gap]: /x\n\n#gap#\n\n[gap]\n\n"
    "> - nested\n>\n> [gap]: /x\n>\n> #gap#\n>\n\n"
    "[^n]: text #note#\n\ntext \\#escape#\n\n$$x$$ #formula#\n\n> last\n\n#end#";

static const oom_case OOM_CASES[] = {
    {"properties", OOM_PROPERTIES_CORPUS, sizeof(OOM_PROPERTIES_CORPUS) - 1},
    {"properties flow", OOM_PROPERTIES_ARRAY_CORPUS, sizeof(OOM_PROPERTIES_ARRAY_CORPUS) - 1},
    {"block identifiers", OOM_BLOCK_IDENTIFIER_CORPUS, sizeof(OOM_BLOCK_IDENTIFIER_CORPUS) - 1},
    {"task markers", OOM_TASK_CORPUS, sizeof(OOM_TASK_CORPUS) - 1},
    {"inline footnotes", OOM_INLINE_FOOTNOTE_CORPUS, sizeof(OOM_INLINE_FOOTNOTE_CORPUS) - 1},
    {"comments", OOM_COMMENT_CORPUS, sizeof(OOM_COMMENT_CORPUS) - 1},
    {"marks", OOM_MARK_CORPUS, sizeof(OOM_MARK_CORPUS) - 1},
    {"full-feature", OOM_CORPUS, sizeof(OOM_CORPUS) - 1},
    {"line-and-core", OOM_LINE_AND_CORE_CORPUS, sizeof(OOM_LINE_AND_CORE_CORPUS) - 1},
    {"extension-edges", OOM_EXTENSION_EDGE_CORPUS, sizeof(OOM_EXTENSION_EDGE_CORPUS) - 1},
};

static markdown_core_document *parse_with_sweep(const oom_case *test, markdown_core_error **error) {
    return markdown_core_document_parse_with_mem((const uint8_t *)test->source, test->length, &sweep_mem, error);
}

static int sweep_case(const oom_case *test) {
    markdown_core_document *document;
    markdown_core_error *error = NULL;
    unsigned long total;
    unsigned long allocation;

    allocation_count = 0;
    fail_at = 0;
    document = parse_with_sweep(test, &error);
    if (!document || error) {
        fprintf(stderr, "%s: counting parse failed\n", test->name);
        markdown_core_document_free(document);
        return -1;
    }
    total = allocation_count;
    markdown_core_document_free(document);
    if (total == 0 || total > 200000UL) {
        fprintf(stderr, "implausible allocation count %lu\n", total);
        return -1;
    }

    for (allocation = 1; allocation <= total; allocation++) {
        allocation_count = 0;
        fail_at = allocation;
        failure_fired = 0;
        error = NULL;
        document = parse_with_sweep(test, &error);
        fail_at = 0;
        if (!failure_fired) {
            fprintf(stderr, "%s: allocation %lu / %lu was not reached\n", test->name, allocation, total);
            markdown_core_document_free(document);
            return -1;
        }
        if (document) {
            fprintf(stderr, "%s: allocation %lu / %lu: OOM returned a document\n", test->name, allocation, total);
            markdown_core_document_free(document);
            return -1;
        }
        if (!error || markdown_core_error_get_code(error) != MARKDOWN_CORE_ERROR_ALLOCATION_FAILED ||
            markdown_core_error_get_message(error).length == 0) {
            fprintf(stderr, "%s: allocation %lu / %lu: OOM was not reported to the consumer\n", test->name, allocation,
                    total);
            return -1;
        }
        markdown_core_error_free(error);
    }

    return 0;
}

static int case_strict_oom(void) {
    size_t index;
    for (index = 0; index < sizeof(OOM_CASES) / sizeof(OOM_CASES[0]); index++) {
        if (sweep_case(&OOM_CASES[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "--case") != 0 || strcmp(argv[2], "strict_oom") != 0) {
        fputs("usage: oom_runner --case strict_oom\n", stderr);
        return 2;
    }
    return case_strict_oom() == 0 ? 0 : 1;
}
