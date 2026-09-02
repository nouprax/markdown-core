#include <stddef.h>
#include "extension.h"
#include <string.h>

#include "markdown-core-extensions.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "tasklist.h"
#include "formula.h"
#include "directive.h"

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
// The order is not in the bit VALUES.  A caller passes a set; only this table
// turns a set into a sequence.
static const struct {
    unsigned bit;
    const markdown_core_extension *extension;
} CORE_EXTENSIONS[] = {{MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH, &MARKDOWN_CORE_EXTENSION_STRIKETHROUGH},
                       {MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK, &MARKDOWN_CORE_EXTENSION_AUTOLINK},
                       {MARKDOWN_CORE_CORE_EXTENSION_TASKLIST, &MARKDOWN_CORE_EXTENSION_TASKLIST},
                       {MARKDOWN_CORE_CORE_EXTENSION_FORMULA, &MARKDOWN_CORE_EXTENSION_FORMULA},
                       {MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE, &MARKDOWN_CORE_EXTENSION_DIRECTIVE},
                       {MARKDOWN_CORE_CORE_EXTENSION_TABLE, &MARKDOWN_CORE_EXTENSION_TABLE}};

#define CORE_EXTENSION_COUNT (sizeof(CORE_EXTENSIONS) / sizeof(CORE_EXTENSIONS[0]))

int markdown_core_core_extensions_attach(markdown_core_parser *parser, unsigned mask) {
    size_t i;

    if (!parser) {
        return 0;
    }

    for (i = 0; i < CORE_EXTENSION_COUNT; i++) {
        if (!(mask & CORE_EXTENSIONS[i].bit)) {
            continue;
        }
        if (!markdown_core_parser_attach_extension(parser, CORE_EXTENSIONS[i].extension)) {
            return 0;
        }
    }

    return 1;
}

unsigned markdown_core_core_extensions_bit(const char *name) {
    size_t i;

    if (!name) {
        return 0;
    }

    for (i = 0; i < CORE_EXTENSION_COUNT; i++) {
        if (strcmp(name, CORE_EXTENSIONS[i].extension->name) == 0) {
            return CORE_EXTENSIONS[i].bit;
        }
    }

    return 0;
}

const char *markdown_core_core_extensions_name_at(size_t index) {
    return index < CORE_EXTENSION_COUNT ? CORE_EXTENSIONS[index].extension->name : NULL;
}
