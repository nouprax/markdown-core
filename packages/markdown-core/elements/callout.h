#ifndef MARKDOWN_CORE_ELEMENT_CALLOUT_H
#define MARKDOWN_CORE_ELEMENT_CALLOUT_H
#include "inlines.h"
#include "block_internal.h"
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_CALLOUT;

/* THE CALLOUT MARKER SCANNER'S WORK (the callout element's parse
 * record): the bytes it examined, for its complexity gate. */
typedef struct {
    size_t scan;
} markdown_core_callout_work;

#endif
