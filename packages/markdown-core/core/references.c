#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

/* Owns `resource` from the call on: it is kept on the new record or released
 * on every path that makes none. */
static markdown_core_map_record *definition_create(markdown_core_mem *mem, markdown_core_map *map,
                                                   markdown_core_chunk *label, markdown_core_resource *resource) {
    markdown_core_map_record *record;
    markdown_core_strbuf *reflabel;

    /* A missing map means parser construction has already poisoned the parse;
     * keep cleanup paths null-safe while the transaction unwinds. */
    if (map == NULL || map->oom) {
        markdown_core_resource_release(mem, resource);
        return NULL;
    }
    /* All declarations precede lookup, including virtual heading records. */
    assert(!map->prepared);

    reflabel = &map->label_buffer;
    /* Every declaration keeps its own normalized label, including duplicates.
     * Scratch is reused; the final spelling is owned with the record. */
    MARKDOWN_CORE_DIAGNOSTIC(map->fold_work += (size_t)label->len;)
    if (!normalize_map_label_into(reflabel, label)) {
        map->oom = reflabel->oom;
        markdown_core_resource_release(mem, resource);
        return NULL;
    }
    map->first_bytes[reflabel->ptr[0] >> 6] |= (uint64_t)1 << (reflabel->ptr[0] & 63);

    record = markdown_core_map_allocate(map, sizeof(*record) + (size_t)reflabel->size + 1);
    if (!record) {
        map->oom = 1;
        markdown_core_resource_release(mem, resource);
        return NULL;
    }
    memcpy(record->label, reflabel->ptr, (size_t)reflabel->size + 1);
    record->resource = resource;
    record->source_key = map->size;
    record->implicit = false;
    record->next = map->records;

    map->records = record;
    map->size++;
    return record;
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) { return markdown_core_map_new(mem); }

markdown_core_map_record *markdown_core_reference_create(markdown_core_mem *mem, markdown_core_map *map,
                                                         markdown_core_chunk *label, markdown_core_resource *resource) {
    return definition_create(mem, map, label, resource);
}

markdown_core_resource *markdown_core_reference_resource(markdown_core_map *map, markdown_core_map_record *record) {
    if (!record->resource) {
        /* A declaration without a resource of its own (a heading) gets one
         * when the first reference resolves to it: the anchor it will name
         * is written into the resource once the anchors are final. */
        record->resource = markdown_core_resource_new(map->mem, markdown_core_chunk_literal(""),
                                                      markdown_core_optional_chunk_absent());
        if (!record->resource) {
            map->oom = 1;
        }
    }
    return record->resource;
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem);
}

markdown_core_map_record *markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    return definition_create(map ? map->mem : NULL, map, label, NULL);
}
