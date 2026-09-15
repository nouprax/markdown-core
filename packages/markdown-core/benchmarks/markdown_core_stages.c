/* Markdown Core's side of the stage comparison.
 *
 * The engine has ONE parse entry and no feed/finish lifecycle, and this
 * benchmark does not give it one: it calls the same public facade a consumer
 * calls, and the stage split is read afterwards out of the call graph, at the
 * two boundaries the transaction already has internally
 * (`S_parse_source` and `S_finish_parse` inside
 * `markdown_core_parse_document_with_mem`). Nothing about the product build is
 * special-cased for measurement; only the profiling flavour's inlining flag
 * keeps those two boundaries from being folded into their caller.
 */
#include <markdown_core.h>

#include "stage_runner.h"

const char *bench_engine_name(void) { return "markdown-core"; }

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    markdown_core_error *error = NULL;
    markdown_core_document *document = markdown_core_document_parse((const uint8_t *)source, length, &error);
    const markdown_core_node *root;

    if (!document) {
        markdown_core_error_free(error);
        return 1;
    }
    root = markdown_core_document_root(document);
    receipt->bytes = length;
    receipt->root_children = root ? markdown_core_node_child_count(root) : 0;
    markdown_core_document_free(document);
    return 0;
}
