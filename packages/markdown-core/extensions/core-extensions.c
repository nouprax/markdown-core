#include <string.h>

#include "markdown-core-extensions.h"
#include "autolink.h"
#include "strikethrough.h"
#include "table.h"
#include "tasklist.h"
#include "formula.h"
#include "directive.h"
#include "cross_reference.h"
#include "extension.h"

// The bundled extensions are immutable compile-time descriptors; lookup is a
// plain scan with no registration step and no process-global state.
#define CORE_EXTENSION_COUNT 8

static markdown_core_extension *core_extension_at(size_t index) {
    switch (index) {
    case 0:
        return markdown_core_table_extension();
    case 1:
        return markdown_core_strikethrough_extension();
    case 2:
        return markdown_core_autolink_extension();
    case 3:
        return markdown_core_tasklist_extension();
    case 4:
        return markdown_core_formula_extension();
    case 5:
        return markdown_core_directive_extension();
    case 6:
        return markdown_core_cross_link_extension();
    case 7:
        return markdown_core_embed_extension();
    default:
        return NULL;
    }
}

markdown_core_extension *markdown_core_extension_find(const char *name) {
    for (size_t i = 0; i < CORE_EXTENSION_COUNT; i++) {
        markdown_core_extension *extension = core_extension_at(i);
        if (!strcmp(extension->name, name)) {
            return extension;
        }
    }
    return NULL;
}
