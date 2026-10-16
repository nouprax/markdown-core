/* Markdown Core's side of the stage comparison.
 *
 * The benchmark calls `markdown_core_document_parse`, the public entry every
 * consumer calls, which is a session that reads its source once and is then
 * discarded (docs/plans/2026-09-29-incremental-parsing.md, 4.4). The entry and
 * the facade calls it needs exist in both revisions the stage gate compares,
 * so one harness builds against both. The stage split is read afterwards out
 * of the call graph, at the two boundaries the transaction has internally
 * (`S_parse_source` inside its commit and `S_finish_parse` inside its
 * finalize). Nothing about the product build is special-cased for
 * measurement; only the profiling flavour's inlining flag keeps those two
 * boundaries from being folded into their callers.
 */
#include <markdown_core.h>

#include "markdown_core_receipt.h"
#include "stage_runner.h"

const char *bench_engine_name(void) { return "markdown-core"; }

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    markdown_core_document *document = NULL;
    if (markdown_core_document_parse((const uint8_t *)source, length, &document) != MARKDOWN_CORE_OK) {
        return 1;
    }
    receipt->bytes = length;
    bool counted = bench_root_children(document, (const uint8_t *)source, length, &receipt->root_children);
    markdown_core_document_free(document);
    return !counted;
}
