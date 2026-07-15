#ifndef MARKDOWN_CORE_ITERATOR_H
#define MARKDOWN_CORE_ITERATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "markdown-core.h"

typedef struct {
    markdown_core_event_type ev_type;
    markdown_core_node *node;
} markdown_core_iter_state;

struct markdown_core_iter {
    markdown_core_mem *mem;
    markdown_core_node *root;
    markdown_core_iter_state cur;
    markdown_core_iter_state next;
};

#ifdef __cplusplus
}
#endif

#endif
