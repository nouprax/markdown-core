#ifndef MARKDOWN_CORE_REFERENCES_H
#define MARKDOWN_CORE_REFERENCES_H

#include "map.h"
#include "node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* THE DEFINITION SETS: the normalized labels a parse declares. The reference
 * map holds every link reference definition's label and every label a
 * heading's text declares, which is what the inline phase asks while it runs:
 * whether a label is defined, so an occurrence naming it resolves. What it
 * resolves to the published document answers (markdown_core_document_
 * reference_for). Neither map holds a node. The footnote map holds the
 * footnote labels. */
markdown_core_map *markdown_core_reference_map_new(void);
markdown_core_map *markdown_core_footnote_definition_map_new(void);
/* Declares the label whose normal form is `label` in `map`: the declaring
 * node has already normalized it to name itself, and the set is keyed by that
 * value. An empty label declares nothing. When the record cannot be made the
 * map turns `oom`, and a NULL map, which parser construction left poisoned,
 * declares nothing. */
void markdown_core_label_declare(markdown_core_map *map, const markdown_core_chunk *label);

#ifdef __cplusplus
}
#endif

#endif
