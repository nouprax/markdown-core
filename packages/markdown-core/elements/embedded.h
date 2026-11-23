#ifndef MARKDOWN_CORE_ELEMENT_EMBEDDED_H
#define MARKDOWN_CORE_ELEMENT_EMBEDDED_H
#include "inlines.h"
#include "bracket.h"
/* It provides MARKDOWN_CORE_SERVICE_DIMENSIONS (services.h). */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_EMBEDDED;
/* THE DIMENSION GRAMMAR'S WORK (the embedded element's parse record):
 * ordinary image-label bytes and bounded dimension work. A cross-link embed
 * shares the grammar and counts its work in its own record. */
typedef struct {
    size_t dimensions;
} markdown_core_embedded_work;

#endif
