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
 * definedness answers; the session's commit reconciler compares that winner
 * before and after, so one kind's flip can hide behind the other's presence
 * and leave the units that read it un-reparsed. Two maps make the collision
 * unrepresentable rather than filtered.
 *
 * The entry type stays markdown_core_reference with an empty url and title. A
 * footnote reference resolves to a node, not to a destination, so there is no
 * payload to carry — but keeping the shape means the label normalization, the
 * winner election, the free function, and the session's whole definition
 * coordination chain run over either map unchanged. One mechanism, two
 * instances; a leaner entry would have bought a second one.
 *
 * The anchor, line, and defining node come in as arguments rather than being
 * stamped onto the map by the caller, because a footnote definition is a
 * single block whose three facts are all known at the call. A paragraph's
 * link-definition harvest cannot do that: it stamps once and then adds
 * however many definitions the paragraph turns out to hold.
 *
 * Does nothing when the label normalizes to nothing (such a footnote names
 * nothing and can never be referenced) or the entry was lost to allocation
 * failure, which is reported through map->oom. A NULL map is tolerated on the
 * same terms as the reference map's. */
void markdown_core_footnote_definition_create(
    markdown_core_map *map,
    markdown_core_chunk *label,
    uint64_t owner,
    int start_line,
    uint64_t definition_node
);
markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem);

#ifdef __cplusplus
}
#endif

#endif
