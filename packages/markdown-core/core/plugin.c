#include <stdlib.h>

#include "plugin.h"

extern markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;

int markdown_core_plugin_register_syntax_extension(markdown_core_plugin *plugin,
                                                   markdown_core_syntax_extension *extension) {
    plugin->syntax_extensions =
        markdown_core_llist_append(&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR, plugin->syntax_extensions, extension);
    return 1;
}

markdown_core_plugin *markdown_core_plugin_new(void) {
    markdown_core_plugin *res =
        (markdown_core_plugin *)MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR.calloc(1, sizeof(markdown_core_plugin));

    res->syntax_extensions = NULL;

    return res;
}

void markdown_core_plugin_free(markdown_core_plugin *plugin) {
    markdown_core_llist_free_full(&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR, plugin->syntax_extensions,
                                  (markdown_core_free_func)markdown_core_syntax_extension_free);
    MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR.free(plugin);
}

markdown_core_llist *markdown_core_plugin_steal_syntax_extensions(markdown_core_plugin *plugin) {
    markdown_core_llist *res = plugin->syntax_extensions;

    plugin->syntax_extensions = NULL;
    return res;
}
