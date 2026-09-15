/* cmark's side of the stage comparison.
 *
 * cmark publishes the two stages as API, so this side needs no call-graph
 * archaeology: `cmark_parser_feed` is the source-to-buffer path and
 * `cmark_parser_finish` is the buffer-to-AST path. The whole document is fed
 * in one call, because a chunked feed would measure cmark's chunk handling
 * against Markdown Core's single-buffer transaction instead of the parse.
 *
 * The version measured is whatever `.tools/cmark/<pinned version>` holds; the
 * driver records it in the report, and the pin itself lives in
 * scripts/init-environment.sh next to the parity oracles.
 */
#include <cmark.h>

#include "stage_runner.h"

const char *bench_engine_name(void) { return "cmark"; }

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
    cmark_node *document;
    cmark_node *child;
    size_t children = 0;

    if (!parser) {
        return 1;
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
