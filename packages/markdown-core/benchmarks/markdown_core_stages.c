/* Markdown Core's side of the stage comparison.
 *
 * The engine has ONE parse entry and no feed/finish lifecycle, and this
 * benchmark does not give it one: it calls `markdown_core_document_parse`,
 * the public entry every consumer calls, which is a session that reads its
 * source once and is then discarded (docs/plans/2026-09-29-incremental-parsing.md,
 * 4.4). The entry and the facade calls it needs exist in both revisions the
 * stage gate compares, so one harness builds against both. The stage split is
 * read afterwards out of the call graph, at the two boundaries the transaction
 * already has internally (`S_parse_source` and `S_finish_parse` inside
 * `markdown_core_parser_parse`). Nothing about the product build is
 * special-cased for measurement; only the profiling flavour's inlining flag
 * keeps those two boundaries from being folded into their caller.
 */
#include <markdown_core.h>

#include <stdlib.h>
#include <string.h>

#include "stage_runner.h"

const char *bench_engine_name(void) { return "markdown-core"; }

/* The root's child count, as the canonical dump writes it on the root's
 * line: the dump is a facade call both revisions have, read after the
 * measured stages. */
static int root_children(const markdown_core_document *document, const char *source, size_t length, size_t *children) {
    uint8_t *dump;
    size_t size;
    if (markdown_core_document_dump(document, markdown_core_document_root(document), (const uint8_t *)source, length,
                                    &dump, &size) != MARKDOWN_CORE_OK) {
        return 1;
    }
    const char *line = (const char *)dump, *end = memchr(line, '\n', size);
    const char *field = NULL;
    for (const char *at = line; end && at + 9 <= end; at++) {
        if (!memcmp(at, "children=", 9)) {
            field = at + 9;
        }
    }
    *children = field ? strtoul(field, NULL, 10) : 0;
    markdown_core_dump_free(dump);
    return field ? 0 : 1;
}

int bench_parse_document(const char *source, size_t length, bench_receipt *receipt) {
    markdown_core_document *document = NULL;
    if (markdown_core_document_parse((const uint8_t *)source, length, &document) != MARKDOWN_CORE_OK) {
        return 1;
    }
    receipt->bytes = length;
    int failed = root_children(document, source, length, &receipt->root_children);
    markdown_core_document_free(document);
    return failed;
}
