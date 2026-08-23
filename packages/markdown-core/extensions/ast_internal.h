#ifndef MARKDOWN_CORE_AST_INTERNAL_H
#define MARKDOWN_CORE_AST_INTERNAL_H

#include "../include/markdown_core.h"
#include <markdown-core.h>
#include <parser.h>

struct markdown_core_document {
    markdown_core_node *root;
    /* Requirement 12's other view. Moved out of the parser at `finish` and
     * released with the tree it names. */
    markdown_core_concrete concrete;
    /* Requirement 13's list. Moved out of the parser at the same moment and on
     * the same terms, and released with the document. Its messages borrow from
     * its own pool, which is why it outlives nothing. */
    markdown_core_diagnostics diagnostics;
};

#endif
