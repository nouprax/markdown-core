#ifndef MARKDOWN_CORE_AST_INTERNAL_H
#define MARKDOWN_CORE_AST_INTERNAL_H

#include "../include/markdown_core.h"
#include <markdown-core.h>

/** Dumps a bare parser tree. A `markdown_core_document` owns a whole
 * committed document now, so a caller holding only a root — the CLI, and the
 * extension-order runner — can no longer wrap one in an aggregate. */
bool markdown_core_ast_dump_root(
    const markdown_core_node *root,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
);

void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message);

/** Document.concrete (incremental-canonical-ast.md 0, 14.1.9): the unified
 * CST owner this document's semantic projection resolves from. There is one
 * physical tree — the concrete owner IS the tree the semantic root views, its
 * marker material hanging off each region node (concrete_records.h) — so the
 * accessor returns the retained owner, never a reconstruction: it allocates
 * nothing and advances no trace. Reachable from a document's committed view
 * exactly as from a one-shot parse; internal until M7 shapes the public
 * concrete interface. */
const markdown_core_node *markdown_core_document_concrete(const markdown_core_document *document);

#endif
