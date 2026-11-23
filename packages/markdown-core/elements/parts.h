#ifndef MARKDOWN_CORE_PARTS_H
#define MARKDOWN_CORE_PARTS_H

#include <stdbool.h>
#include <stddef.h>

#include "../include/markdown_core.h"
#include "buffer.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The source ranges of `node`'s fields of slot kind `slot`, in the tree
 * `root` parsed from `length` bytes of `source`, which a walk of `root`
 * within `window` reaches, or of all of it when that is NULL: one per run
 * of each field, in source order, an array the caller frees with
 * markdown_core_free. */
struct markdown_core_window;
markdown_core_status markdown_core_parts_fields(const markdown_core_node *root,
                                                const struct markdown_core_window *window,
                                                const markdown_core_node *node, markdown_core_slot_kind slot,
                                                const uint8_t *source, size_t length, markdown_core_place **places,
                                                size_t *count);

#ifdef __cplusplus
}
#endif

#endif
