#include <stddef.h>
#include <string.h>

#include "markdown-core-extensions.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "tasklist.h"
#include "formula.h"
#include "directive.h"
#include "once.h"
#include "registry.h"
#include "plugin.h"

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
    const char *name;
} CORE_EXTENSIONS[] = {
    {MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH, "strikethrough"}, {MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK, "autolink"},
    {MARKDOWN_CORE_CORE_EXTENSION_TASKLIST, "tasklist"},           {MARKDOWN_CORE_CORE_EXTENSION_FORMULA, "formula"},
    {MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE, "directive"},         {MARKDOWN_CORE_CORE_EXTENSION_TABLE, "table"}};

#define CORE_EXTENSION_COUNT (sizeof(CORE_EXTENSIONS) / sizeof(CORE_EXTENSIONS[0]))

int markdown_core_core_extensions_attach(markdown_core_parser *parser, unsigned mask) {
    size_t i;

    if (!parser) {
        return 0;
    }

    for (i = 0; i < CORE_EXTENSION_COUNT; i++) {
        markdown_core_syntax_extension *extension;

        if (!(mask & CORE_EXTENSIONS[i].bit)) {
            continue;
        }

        extension = markdown_core_find_syntax_extension(CORE_EXTENSIONS[i].name);
        if (!extension || !markdown_core_parser_attach_syntax_extension(parser, extension)) {
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
        if (strcmp(name, CORE_EXTENSIONS[i].name) == 0) {
            return CORE_EXTENSIONS[i].bit;
        }
    }

    return 0;
}

static int core_extensions_registration(markdown_core_plugin *plugin) {
    markdown_core_plugin_register_syntax_extension(plugin, create_table_extension());
    markdown_core_plugin_register_syntax_extension(plugin, create_strikethrough_extension());
    markdown_core_plugin_register_syntax_extension(plugin, create_autolink_extension());
    markdown_core_plugin_register_syntax_extension(plugin, create_tasklist_extension());
    markdown_core_plugin_register_syntax_extension(plugin, create_formula_extension());
    markdown_core_plugin_register_syntax_extension(plugin, create_directive_extension());
    return 1;
}

static void core_extensions_register_once(void) { markdown_core_register_plugin(core_extensions_registration); }

// The whole registration transaction — extension node-type allocation, node
// flag registration, and the registry append — mutates process-global state,
// so it runs under a process-level once.  After it completes the registry is
// immutable for the lifetime of the process; there is no release path.
void markdown_core_core_extensions_ensure_registered(void) {
    static markdown_core_once once = MARKDOWN_CORE_ONCE_INIT;
    markdown_core_once_run(&once, core_extensions_register_once);
}
