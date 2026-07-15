#ifndef MARKDOWN_CORE_REGISTRY_H
#define MARKDOWN_CORE_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"
#include "plugin.h"

// Registration is an initialization-time operation: it must complete before
// any concurrent parsing starts (the facade runs it under a process-level
// once).  Registered extensions stay immutable and live for the remainder of
// the process; there is deliberately no release/unregister path, so an
// "initialized" state can never point at a freed registry.
MARKDOWN_CORE_EXPORT
void markdown_core_register_plugin(markdown_core_plugin_init_func reg_fn);

MARKDOWN_CORE_EXPORT
markdown_core_llist *markdown_core_list_syntax_extensions(markdown_core_mem *mem);

#ifdef __cplusplus
}
#endif

#endif
