#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

static void reference_free(markdown_core_map *map, markdown_core_map_entry *_ref) {
    markdown_core_reference *ref = (markdown_core_reference *)_ref;
    markdown_core_mem *mem = map->mem;
    if (ref != NULL) {
        mem->free(mem, ref->entry.label);
        markdown_core_chunk_free(mem, &ref->url);
        markdown_core_chunk_free(mem, &ref->title);
        mem->free(mem, ref);
    }
}

/* The half both definition kinds share: a normalized, non-empty label in a
 * zeroed entry, ready for the caller's payload and markdown_core_map_add.
 * Returns NULL when the label names nothing or the entry was lost, with the
 * loss reported through the map. */
static markdown_core_reference *definition_entry_new(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_reference *ref;
    unsigned char *reflabel;
    int lost = 0;

    /* The parser tolerates a missing definition map (map_new failure under a
     * NULL-returning allocator); definitions are then dropped. */
    if (map == NULL) {
        return NULL;
    }

    reflabel = markdown_core_map_normalize_label(map->mem, label, &lost);

    /* empty reference name, or composed from only whitespace */
    if (reflabel == NULL) {
        if (lost) {
            map->oom = 1;
        }
        return NULL;
    }

    ref = (markdown_core_reference *)map->mem->calloc(map->mem, 1, sizeof(*ref));
    if (!ref) {
        map->oom = 1;
        map->mem->free(map->mem, reflabel);
        return NULL;
    }
    ref->entry.label = reflabel;
    return ref;
}

void markdown_core_reference_create(
    markdown_core_map *map,
    markdown_core_chunk *label,
    markdown_core_chunk *url,
    markdown_core_chunk *title
) {
    markdown_core_reference *ref = definition_entry_new(map, label);
    int lost = 0;

    if (!ref) {
        return;
    }
    ref->url = markdown_core_clean_url(map->mem, url, &lost);
    ref->title = markdown_core_clean_title(map->mem, title, &lost);
    if (lost) {
        map->oom = 1;
    }
    ref->entry.size = ref->url.len + ref->title.len;

    markdown_core_map_add(map, &ref->entry);
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, reference_free);
}

void markdown_core_footnote_definition_create(
    markdown_core_map *map,
    markdown_core_chunk *label,
    uint64_t owner,
    int start_line,
    uint64_t definition_node
) {
    markdown_core_reference *ref = definition_entry_new(map, label);

    if (!ref) {
        return;
    }
    /* url, title, and therefore `size`, stay zero: a footnote reference
     * expands to nothing at the reference site, so these definitions never
     * draw on the reference expansion budget. The empty chunks also make the
     * shared payload comparison a tautology, which is the right answer —
     * identical footnote labels *are* identical definitions as far as any
     * reference can tell. */
    map->pending_owner = owner;
    map->pending_line = start_line;
    markdown_core_map_add(map, &ref->entry);
    ref->entry.definition_node = definition_node;
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, reference_free);
}
