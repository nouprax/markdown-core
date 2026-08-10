#include <string.h>

#include "document_internal.h"

#include <map.h>
#include <node.h>

// Reference answers.
//
// The tree is source-faithful — a ReferenceDefinition states its destination
// once, where it was written, and a LinkReference carries only its label — so
// which definition a reference resolves to holds *between* nodes and lives in
// neither. It is answered here.
//
// There is no second index. The session's reference map already elects a
// winner per label, already matches labels case-folded with collapsed
// whitespace, and is already maintained incrementally by the commit
// reconciler; a definition now also records the node it was written as, so the
// map answers this question directly. Building a parallel node-id table was
// tried first and was wrong twice over: it duplicated the winner election, and
// rebuilding it per commit made the commit cost scale with the document, which
// is exactly the bound the incremental contract exists to hold.

bool markdown_core_document_reference_info(
    const markdown_core_document *session,
    const markdown_core_node *node,
    markdown_core_reference_info *info
) {
    markdown_core_chunk *label;
    markdown_core_map_entry *winner;

    if (info) {
        memset(info, 0, sizeof(*info));
    }
    // The reference table only: a footnote reference resolves to a definition
    // node, which the session's footnote index answers.
    if (!session || !info || !node || !session->definitions[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map) {
        return false;
    }
    switch (node->type) {
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        label = &((markdown_core_node *)node)->as.definition.label;
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        label = &((markdown_core_node *)node)->as.reference.label;
        break;
    default:
        return false;
    }
    winner = markdown_core_map_lookup(session->definitions[MARKDOWN_CORE_DEFINITIONS_REFERENCES].map, label);
    info->definition = winner ? winner->definition_node : 0;
    return true;
}
