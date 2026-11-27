#ifndef MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#define MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#include "markdown-core-element-api.h"
#include "metadata.h"
#include "yaml.h"
#include "element.h"
#include "markdown-core-element-api.h"

/* THE PROPERTIES GRAMMAR'S WORK: the value bytes decoded, each
 * source range once at its owning boundary; the key bytes classified; and the
 * bytes the closing-fence search examined. Physical line
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
 * the bytes it takes. An envelope its text leaves open notes its opener
 * for `owner` (blocks.c, AN OPENER READ TO THE END). */
bool markdown_core_properties_closed(markdown_core_properties_work *work, markdown_core_parser *parser,
                                     markdown_core_reader *reader, const markdown_core_element_instance *owner,
                                     size_t *fence);
/* THE STOP LINES of the envelope's opener (element.h, `stops`): a line that
 * begins with "---", which takes in every fence. */
bool markdown_core_properties_stops(const markdown_core_element_instance *self, const unsigned char *line,
                                    size_t length);
size_t markdown_core_properties_parse(markdown_core_properties_work *work, markdown_core_parser *parser,
                                      markdown_core_reader *reader, size_t fence);

/* A PLACE of an envelope's payload, as the places' reader finds it: a
 * member's name, of slot kind FIELD, or one of its values, of slot kind
 * VALUE, each a scalar written in `style` whose bytes run from `from` to
 * `to`, without its quotes and from a literal's first text to its last
 * line's end, an empty one where the parser read it; or bytes no field
 * holds, of slot kind NONE: a member skipped as a whole, a comment, or a
 * line between members. A member's name comes before its values. */
typedef struct {
    markdown_core_slot_kind slot;
    markdown_core_yaml_style style;
    size_t from, to;
} markdown_core_metadata_place;

/* The places the parse records, in order. */
typedef struct {
    markdown_core_metadata_place *items;
    size_t count, capacity;
} markdown_core_metadata_places;

/* The facets of the document element's Metadata. */
extern const markdown_core_facet MARKDOWN_CORE_PROPERTIES_FACETS[];

#endif
