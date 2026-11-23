#ifndef MARKDOWN_CORE_ELEMENT_ENTITIES_H
#define MARKDOWN_CORE_ELEMENT_ENTITIES_H
#include "parser.h"
#include "element.h"
#include "buffer.h"

/* THE ENTITY AND NUMERIC CHARACTER REFERENCES (CommonMark 0.31.2, 2.5): an
 * `&` that begins a named reference of the HTML5 table, or a decimal or
 * hexadecimal one, is the character it names. The table (entities.inc) and
 * its decoder (houdini.h) are this element's and nobody else's. */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ENTITIES;

/* The decoders of the references in a slice another element decodes (an
 * attribute value, a link's destination and title, a fence's info string,
 * an autolink), for an element that holds the entities element as a peer.
 * A NULL `entities`, a dialect without the element, decodes nothing.
 *
 * `read`: the reference whose name, after its `&`, begins `src`: appends
 * its character to `out` and returns the bytes it spans after the `&`, or
 * 0 when none begins there.
 * `decode`: `src` with every reference decoded, appended to `out`; 0, and
 * nothing appended, when `src` holds no `&` to decode.
 * `copy`: `decode`, appending `src` as it is when it returns 0. */
bufsize_t markdown_core_entities_read(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                      const uint8_t *src, bufsize_t size);
int markdown_core_entities_decode(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                  const uint8_t *src, bufsize_t size);
void markdown_core_entities_copy(const markdown_core_element_instance *entities, markdown_core_strbuf *out,
                                 const uint8_t *src, bufsize_t size);

#endif
