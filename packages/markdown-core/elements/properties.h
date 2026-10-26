#ifndef MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#define MARKDOWN_CORE_ELEMENT_PROPERTIES_H
#include "markdown-core-element-api.h"
#include "metadata.h"
#include "yaml.h"
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

typedef struct {
    markdown_core_metadata_place *items;
    size_t count, capacity;
} markdown_core_metadata_places;

/* Reads the places of the envelope `text`, `length` bytes from its opening
 * fence through its closing one, in order, into `places`, whose items the
 * caller frees; false when an allocation failed. */
bool markdown_core_metadata_places_read(const unsigned char *text, size_t length,
                                        markdown_core_metadata_places *places);
/* Whether a plain scalar of `size` bytes of `text` resolves to text, not
 * to null, a boolean or a number. */
bool markdown_core_metadata_text(const unsigned char *text, size_t size);

#endif
