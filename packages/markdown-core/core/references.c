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
    unsigned char *reflabel = normalize_map_label(map->mem, label);

    /* empty reference name, or composed from only whitespace */
    if (reflabel == NULL)
        return;

    assert(!map->prepared);

    ref = (markdown_core_reference *)map->mem->calloc(1, sizeof(*ref));
    ref->entry.label = reflabel;
    ref->url = markdown_core_clean_url(map->mem, url);
    ref->title = markdown_core_clean_title(map->mem, title);
    ref->entry.age = map->size;
    ref->entry.next = map->refs;
    ref->entry.size = ref->url.len + ref->title.len;

    map->refs = (markdown_core_map_entry *)ref;
    map->size++;
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, reference_free);
}
