#ifndef MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#define MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#include "markdown-core-element-api.h"
#include "metadata.h"
#include "markdown-core-element-api.h"

/* THE PROPERTIES GRAMMAR'S WORK: the value bytes decoded, each
 * source range once at its owning boundary; the key bytes classified; and the
 * bytes the allocation-free closing-fence search examined. Physical line
 * geometry is the engine's (`input_line_work`). The grammar is the document
 * prefix's, so the document element's parse record holds it (document.h). */
typedef struct {
    size_t decoded_bytes, key_work, line_work;
} markdown_core_properties_work;

/* THE ENVELOPE opens the input's text (markdown_core_parser_input_text) with
 * a "---" line and closes at the next line that is "---", counting into
 * `work`. `closed` answers whether the
 * input opens with one that closes, with where the closing fence starts in
 * `*fence`; `parse` reads the envelope that closes at `fence` and answers
 * the bytes it takes. `fence` answers how far the line `data`, through its
 * line ending, is a fence: 3 for "---", the dashes it begins with for a
 * line whose bytes could still become one, and -1 for any other line. */
bool markdown_core_properties_closed(markdown_core_properties_work *work, markdown_core_parser *parser, size_t *fence);
size_t markdown_core_properties_parse(markdown_core_properties_work *work, markdown_core_parser *parser, size_t fence);
int markdown_core_properties_fence(const unsigned char *data);

#endif
