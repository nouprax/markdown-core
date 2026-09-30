/* Markdown Core's side of the stage comparison.
 *
 * The engine has ONE parse entry and no feed/finish lifecycle, and this
 * benchmark does not give it one: it calls the engine's internal parse entry,
 * `markdown_core_parse_document`, the one the public facade wraps. That entry
 * stays the same while the public API changes, so one harness builds against
 * both revisions the stage gate compares. The stage split is read afterwards
 * out of the call graph, at the two boundaries the transaction already has
 * internally (`S_parse_source` and `S_finish_parse` inside
 * `markdown_core_parser_parse`). Nothing about the product build is
 * special-cased for measurement; only the profiling flavour's inlining flag
 * keeps those two boundaries from being folded into their caller.
 */
#include <markdown_core.h>

#include "markdown-core-elements.h"
#include "node.h"

#include "stage_runner.h"

const char *bench_engine_name(void) { return "markdown-core"; }

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    markdown_core_node *root = markdown_core_parse_document(source, length);

    if (!root) {
        return 1;
    }
    receipt->bytes = length;
    receipt->root_children = markdown_core_node_child_count(root);
    markdown_core_node_free(root);
    return 0;
}
