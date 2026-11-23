#ifndef MARKDOWN_CORE_ELEMENT_ANCHOR_H
#define MARKDOWN_CORE_ELEMENT_ANCHOR_H
#include "element.h"
#include "names.h"

/* THE ANCHOR ELEMENT: how the names service spells the anchors it names
 * (anchors.md): a node whose text projects to nothing takes the base
 * `section`, and a name taken already takes the base, a hyphen and a
 * number. It reads no source; the names its owners ask for (a heading's,
 * a block identifier's) are spelled as it says. */
extern const markdown_core_element MARKDOWN_CORE_ELEMENT_ANCHOR;
extern const markdown_core_name_spelling MARKDOWN_CORE_ANCHOR_SPELLING;

#endif
