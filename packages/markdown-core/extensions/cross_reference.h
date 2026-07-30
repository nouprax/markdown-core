#ifndef MARKDOWN_CORE_CROSS_REFERENCE_H
#define MARKDOWN_CORE_CROSS_REFERENCE_H

#include "markdown-core-extensions.h"

#include <chunk.h>

// Compile-time extension node types; values continue the bundled inline
// extension range after directive and directive-label.
#define MARKDOWN_CORE_NODE_CROSS_LINK ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x000f))
#define MARKDOWN_CORE_NODE_EMBED ((markdown_core_node_type)(MARKDOWN_CORE_NODE_TYPE_INLINE | 0x0010))

markdown_core_extension *markdown_core_cross_link_extension(void);
markdown_core_extension *markdown_core_embed_extension(void);

/** Borrows the source-faithful reference, or NULL for any other node. */
const markdown_core_chunk *markdown_core_cross_reference_value(markdown_core_node *node);

#endif
