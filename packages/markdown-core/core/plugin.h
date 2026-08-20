#ifndef MARKDOWN_CORE_PLUGIN_H
#define MARKDOWN_CORE_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"
#include "markdown-core-extension-api.h"

/**
 * markdown_core_plugin:
 *
 * A plugin structure, which should be filled by plugin's
 * init functions.
 */
struct markdown_core_plugin {
    markdown_core_llist *syntax_extensions;
};

markdown_core_llist *markdown_core_plugin_steal_syntax_extensions(markdown_core_plugin *plugin);

markdown_core_plugin *markdown_core_plugin_new(void);

void markdown_core_plugin_free(markdown_core_plugin *plugin);

#ifdef __cplusplus
}
#endif

#endif
