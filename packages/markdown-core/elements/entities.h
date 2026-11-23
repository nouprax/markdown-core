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

/* It provides MARKDOWN_CORE_SERVICE_REFERENCES (services.h): a decoder
 * that reads one reference after its `&` and decodes every reference of a
 * slice. */

#endif
