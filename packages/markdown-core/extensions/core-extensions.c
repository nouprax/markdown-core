#include <stddef.h>
#include "extension.h"

#include "markdown-core-extensions.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "tasklist.h"
#include "formula.h"
#include "directive.h"
#include "cross_link.h"

// THE attach order, and the only place in the library it is written down.
// Before this table there were two of them -- `core/main.c` attached
// `directive` FIRST and the facade attached it LAST -- so the CLI's default
// language was not the language every binding got.  Over 2,744 ordered triples
// of 14 significant lines the two still disagreed on 4 with D8 already fixed,
// and no fixture saw any of them.
//
// `table` is LAST, which is Q9 and a decision rather than an inheritance: a
// table's row matcher claims any line inside an open table, so every narrower
// claim has to get its turn before it.  D8 answers the case where table
// DECLINES; only the order answers the case where it succeeds.
//
// The same list also decides inline match order and postprocess order, so
// `autolink` stays ahead of `directive` -- both claim ':', and a bare ':' far
// more often begins a URL.
//
// The cross-link scanner recognizes complete [[ and ![[ before inherited bracket
// handling. It follows autolinks and formulas, and precedes directives and the
// final table.
//
// Every row is attached by every parse.  There is no mask and no name: the
// dialect has no switches, so a table that could be attached in part would be
// a second language nothing ships, and a name would be a registry nothing
// reads.  A feature is public from the commit that adds its row.
static const markdown_core_extension *const CORE_EXTENSIONS[] = {
    &MARKDOWN_CORE_EXTENSION_STRIKETHROUGH, &MARKDOWN_CORE_EXTENSION_AUTOLINK,   &MARKDOWN_CORE_EXTENSION_TASKLIST,
    &MARKDOWN_CORE_EXTENSION_FORMULA,       &MARKDOWN_CORE_EXTENSION_CROSS_LINK, &MARKDOWN_CORE_EXTENSION_DIRECTIVE,
    &MARKDOWN_CORE_EXTENSION_TABLE};

#define CORE_EXTENSION_COUNT (sizeof(CORE_EXTENSIONS) / sizeof(CORE_EXTENSIONS[0]))

int markdown_core_core_extensions_attach(markdown_core_parser *parser) {
    size_t i;

    if (!parser) {
        return 0;
    }

    for (i = 0; i < CORE_EXTENSION_COUNT; i++) {
        if (!markdown_core_parser_attach_extension(parser, CORE_EXTENSIONS[i])) {
            return 0;
        }
    }

    return 1;
}
