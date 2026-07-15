#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "markdown-core.h"
#include "syntax_extension.h"
#include "registry.h"
#include "plugin.h"

extern markdown_core_mem MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR;

static markdown_core_llist *syntax_extensions = NULL;

void markdown_core_register_plugin(markdown_core_plugin_init_func reg_fn) {
    markdown_core_plugin *plugin = markdown_core_plugin_new();

    if (!reg_fn(plugin)) {
        markdown_core_plugin_free(plugin);
        return;
    }

    markdown_core_llist *syntax_extensions_list = markdown_core_plugin_steal_syntax_extensions(plugin), *it;

    for (it = syntax_extensions_list; it; it = it->next) {
        syntax_extensions =
            markdown_core_llist_append(&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR, syntax_extensions, it->data);
    }

    markdown_core_llist_free(&MARKDOWN_CORE_DEFAULT_MEM_ALLOCATOR, syntax_extensions_list);
    markdown_core_plugin_free(plugin);
}

markdown_core_llist *markdown_core_list_syntax_extensions(markdown_core_mem *mem) {
    markdown_core_llist *it;
    markdown_core_llist *res = NULL;

    for (it = syntax_extensions; it; it = it->next) {
        res = markdown_core_llist_append(mem, res, it->data);
    }
    return res;
}

markdown_core_syntax_extension *markdown_core_find_syntax_extension(const char *name) {
    markdown_core_llist *tmp;

    for (tmp = syntax_extensions; tmp; tmp = tmp->next) {
        markdown_core_syntax_extension *ext = (markdown_core_syntax_extension *)tmp->data;
        if (!strcmp(ext->name, name))
            return ext;
    }
    return NULL;
}
