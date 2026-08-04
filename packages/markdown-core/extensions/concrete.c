#include "concrete.h"

#include "cross_reference.h"
#include "directive.h"
#include "formula.h"
#include "strikethrough.h"
#include "table.h"

// The 11.1 partition as one exhaustive switch over the engine's node
// types. Extension types are compile-time constants (each extension
// header defines its value), so the classification is total at build time
// and needs no registry. A new node kind fails the partition gate until a
// case is added here — which is the point: the classification is part of
// the kind's definition, not an afterthought.

markdown_core_region_class markdown_core_region_classify(const markdown_core_node *node) {
    switch (node->type) {
    case MARKDOWN_CORE_NODE_PARAGRAPH:
    case MARKDOWN_CORE_NODE_HEADING:
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        return MARKDOWN_CORE_REGION_LEAF;
    case MARKDOWN_CORE_NODE_DOCUMENT:
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
    case MARKDOWN_CORE_NODE_LIST:
    case MARKDOWN_CORE_NODE_LIST_ITEM:
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return MARKDOWN_CORE_REGION_CONTAINER;
    default:
        break;
    }
    // Extension types are not compile-time constants in a switch-safe form
    // on every compiler (their macros cast), so they chain here.
    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        return MARKDOWN_CORE_REGION_LEAF;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE || node->type == MARKDOWN_CORE_NODE_TABLE_ROW ||
        node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return MARKDOWN_CORE_REGION_CONTAINER;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return MARKDOWN_CORE_REGION_INLINE_SEQUENCE;
    }
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        // The one contextual kind: a block directive's label owns its
        // inline sequence; an inline directive's label lives inside the
        // enclosing leaf's region (11.1).
        if (node->parent && node->parent->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
            return MARKDOWN_CORE_REGION_INLINE_SEQUENCE;
        }
        return MARKDOWN_CORE_REGION_NONE;
    }
    return MARKDOWN_CORE_REGION_NONE;
}

const markdown_core_node *markdown_core_region_of(const markdown_core_node *node) {
    while (node && markdown_core_region_classify(node) == MARKDOWN_CORE_REGION_NONE) {
        node = node->parent;
    }
    return node;
}
