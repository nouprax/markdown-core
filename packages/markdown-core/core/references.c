#include "alloc.h"
#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

/* Registers a record under `key`, which is already in the map's normal form.
 * Owns `resource` from the call on: it is kept on the new record or released
 * on every path that makes none. */
static markdown_core_map_record *record_create(markdown_core_map *map, const unsigned char *key, bufsize_t key_len,
                                               markdown_core_resource *resource) {
    markdown_core_map_record *record;

    /* A missing map means parser construction has already poisoned the parse;
     * keep cleanup paths null-safe while the transaction unwinds. */
    if (map == NULL || map->oom) {
        markdown_core_resource_release(resource);
        return NULL;
    }
    /* All declarations precede lookup, including virtual heading records. */
    assert(!map->prepared);

    record = markdown_core_map_carve(map, sizeof(*record) + (size_t)key_len + 1);
    if (!record) {
        map->oom = 1;
        markdown_core_resource_release(resource);
        return NULL;
    }
    memcpy(record->label, key, (size_t)key_len);
    record->label[key_len] = '\0';
    record->label_len = key_len;
    record->resource = resource;
    record->source_key = map->size;
    record->implicit = false;
    record->next = map->records;

    map->records = record;
    map->size++;
    return record;
}

/* Every declaration keeps its own normalized label, including duplicates.
 * Scratch is reused; the final spelling is owned with the record. */
static markdown_core_map_record *definition_create(markdown_core_map *map, markdown_core_chunk *label,
                                                   markdown_core_resource *resource) {
    if (map == NULL || map->oom) {
        markdown_core_resource_release(resource);
        return NULL;
    }
    markdown_core_strbuf *reflabel = &map->label_buffer;
    if (!normalize_map_label_into(reflabel, label)) {
        map->oom = reflabel->oom;
        markdown_core_resource_release(resource);
        return NULL;
    }
    return record_create(map, reflabel->ptr, reflabel->size, resource);
}

markdown_core_map *markdown_core_reference_map_new(void) { return markdown_core_map_new(); }

markdown_core_map_record *markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label,
                                                         markdown_core_resource *resource) {
    return definition_create(map, label, resource);
}

markdown_core_map *markdown_core_footnote_definition_map_new(void) { return markdown_core_map_new(); }

void markdown_core_footnote_definition_create(markdown_core_map *map, const markdown_core_chunk *id) {
    if (id->len > 0) {
        record_create(map, id->data, id->len, NULL);
    }
}
