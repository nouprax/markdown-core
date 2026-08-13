#ifndef MARKDOWN_CORE_REFERENCES_H
#define MARKDOWN_CORE_REFERENCES_H

#include "map.h"

#ifdef __cplusplus
extern "C" {
#endif

struct markdown_core_reference {
    markdown_core_map_entry entry;
    markdown_core_chunk url;
    markdown_core_chunk title;
};

typedef struct markdown_core_reference markdown_core_reference;

void markdown_core_reference_create(
    markdown_core_map *map,
    markdown_core_chunk *label,
    markdown_core_chunk *url,
    markdown_core_chunk *title
);
markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem);

/* Footnote definitions live in a map of their own, never alongside link
 * reference definitions. Sharing one map would put `[x]:` and `[^x]:` in the
 * same label bucket, where a single winner has to stand for two independent
 * definedness answers, so one kind's presence could hide behind the other's.
 * Two maps make the collision unrepresentable rather than filtered.
 *
 * The entry type stays markdown_core_reference with an empty url and title. A
 * footnote reference resolves to a node, not to a destination, so there is no
 * payload to carry — but keeping the shape means the label normalization, the
 * winner election, and the free function run over either map unchanged. One
 * mechanism, two instances; a leaner entry would have bought a second one.
 *
 * Does nothing when the label normalizes to nothing (such a footnote names
 * nothing and can never be referenced) or the entry was lost to allocation
 * failure, which is reported through map->oom. A NULL map is tolerated on the
 * same terms as the reference map's. */
void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label);
markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem);

#ifdef __cplusplus
}
#endif

#endif
