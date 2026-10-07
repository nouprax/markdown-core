#ifndef MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#define MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#include "markdown-core-element-api.h"
#include "metadata.h"
#include "markdown-core-element-api.h"

/* THE PROPERTIES GRAMMAR'S WORK: the value bytes decoded, each
 * source range once at its owning boundary; the key bytes classified; and the
 * bytes the closing-fence search examined. Physical line
 * geometry is the engine's (`input_line_work`). The grammar is the document
 * prefix's, so the document element's parse record holds it (document.h). */
typedef struct {
    size_t decoded_bytes, key_work, line_work;
} markdown_core_properties_work;

/* Read the document prefix's properties envelope, counting into `work`. */
void markdown_core_properties_parse(markdown_core_properties_work *work, markdown_core_parser *parser);

#endif
