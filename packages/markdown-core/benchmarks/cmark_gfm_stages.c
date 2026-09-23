/* cmark-gfm's side of the stage comparison.
 *
 * THE POINT OF A THIRD ENGINE is that a ratio only compares when both parsers
 * did the same job. Against plain cmark, a pipe table case measures this parser
 * building a table against cmark reading a paragraph: the number bounds what
 * the construct costs but says nothing about whether it is slow. cmark-gfm
 * implements tables, strikethrough, bare autolinks, task lists and footnotes,
 * so for those constructs it is a reference doing the same work in the same
 * language, from the same codebase cmark is.
 *
 * It is pinned already, as the GFM extension oracle in
 * scripts/tooling/setup-environment.sh; this reads the same build.
 *
 * The stage split is cmark's, unchanged: `cmark_parser_feed` is the
 * source-to-buffer path and `cmark_parser_finish` is the buffer-to-AST path,
 * fed in one call so the comparison is the parse and not chunk handling.
 *
 * EXTENSIONS ARE ATTACHED, NOT ASSUMED. cmark-gfm parses plain CommonMark until
 * an extension is attached to the parser, so measuring it without attaching
 * them would produce a second copy of the cmark measurement under a name that
 * claims otherwise. Every extension the corpus exercises is attached, and one
 * that cannot be found fails the run rather than quietly measuring less.
 */
#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>

#include "stage_runner.h"

/* `tagfilter` is deliberately absent: it rewrites raw HTML on output and has no
 * bearing on the parse this measures. Footnotes are an option rather than an
 * extension in this release. */
static const char *const EXTENSIONS[] = {"table", "strikethrough", "autolink", "tasklist"};

const char *bench_engine_name(void) { return "cmark-gfm"; }

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    cmark_parser *parser;
    cmark_node *document;
    cmark_node *child;
    size_t children = 0;
    size_t i;

    cmark_gfm_core_extensions_ensure_registered();
    parser = cmark_parser_new(CMARK_OPT_DEFAULT | CMARK_OPT_FOOTNOTES);
    if (!parser) {
        return 1;
    }
    for (i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
        cmark_syntax_extension *extension = cmark_find_syntax_extension(EXTENSIONS[i]);
        if (!extension || !cmark_parser_attach_syntax_extension(parser, extension)) {
            cmark_parser_free(parser);
            return 1;
        }
    }
    cmark_parser_feed(parser, source, length);
    document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    if (!document) {
        return 1;
    }
    for (child = cmark_node_first_child(document); child; child = cmark_node_next(child)) {
        children++;
    }
    receipt->bytes = length;
    receipt->root_children = children;
    cmark_node_free(document);
    return 0;
}
