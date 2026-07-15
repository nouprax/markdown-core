#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

static void reference_free(markdown_core_map *map, markdown_core_map_entry *_ref) {
    markdown_core_reference *ref = (markdown_core_reference *)_ref;
    markdown_core_mem *mem = map->mem;
    if (ref != NULL) {
        mem->free(ref->entry.label);
        markdown_core_chunk_free(mem, &ref->url);
        markdown_core_chunk_free(mem, &ref->title);
        mem->free(ref);
    }
}

void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label, markdown_core_chunk *url,
                                    markdown_core_chunk *title) {
    markdown_core_reference *ref;
    unsigned char *reflabel;
    int lost = 0;

    /* The parser tolerates a missing reference map (map_new failure under a
     * NULL-returning allocator); definitions are then dropped. */
    if (map == NULL)
        return;

    reflabel = normalize_map_label(map->mem, label, &lost);

    /* empty reference name, or composed from only whitespace */
    if (reflabel == NULL) {
        if (lost)
            map->oom = 1;
        return;
    }

    assert(!map->prepared);

    ref = (markdown_core_reference *)map->mem->calloc(1, sizeof(*ref));
    if (!ref) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    ref->entry.label = reflabel;
    lost = 0;
    ref->url = markdown_core_clean_url(map->mem, url, &lost);
    ref->title = markdown_core_clean_title(map->mem, title, &lost);
    if (lost)
        map->oom = 1;
    ref->entry.age = map->size;
    ref->entry.next = map->refs;
    ref->entry.size = ref->url.len + ref->title.len;

    map->refs = (markdown_core_map_entry *)ref;
    map->size++;
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, reference_free);
}
