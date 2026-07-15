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
