#ifndef MARKDOWN_CORE_AST_INTERNAL_H
#define MARKDOWN_CORE_AST_INTERNAL_H

#include "../include/markdown_core.h"
#include <markdown-core.h>

struct markdown_core_document {
    markdown_core_node *root;
};

void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message);

/** Document.concrete (incremental-canonical-ast.md 0, 14.1.9): the unified
 * CST owner this document's semantic projection resolves from. There is one
 * physical tree — the concrete owner IS the tree the semantic root views, its
 * marker material hanging off each region node (concrete_records.h) — so the
 * accessor returns the retained owner, never a reconstruction: it allocates
 * nothing and advances no trace. Reachable from a session's committed view
 * exactly as from a one-shot parse; internal until M7 shapes the public
 * concrete interface. */
const markdown_core_node *markdown_core_document_concrete(const markdown_core_document *document);

#endif
