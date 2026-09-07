#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

/* Owns `resource` from the call on: it is kept on the new record or released
 * on every path that makes none. */
static void definition_create(markdown_core_mem *mem, markdown_core_map *map, markdown_core_chunk *label,
                              markdown_core_resource *resource) {
    markdown_core_map_record *record;
    unsigned char *reflabel;
    int lost = 0;

    /* A missing map means parser construction has already poisoned the parse;
     * keep cleanup paths null-safe while the transaction unwinds. */
    if (map == NULL) {
        markdown_core_resource_release(mem, resource);
        return;
    }

    reflabel = normalize_map_label(map->mem, label, &lost);
    /* An empty label, or one that is all whitespace, defines nothing. */
    if (reflabel == NULL) {
        if (lost) {
            map->oom = 1;
        }
        markdown_core_resource_release(mem, resource);
        return;
    }

    record = (markdown_core_map_record *)map->mem->calloc(1, sizeof(*record));
    if (!record) {
        map->oom = 1;
        map->mem->free(reflabel);
        markdown_core_resource_release(mem, resource);
        return;
    }
    record->label = reflabel;
    record->resource = resource;
    record->next = map->records;

    map->records = record;
    map->size++;
    /* A lookup may have indexed the map already -- the block phase asks
     * whether a label is defined as each footnote definition opens (M4) --
     * so a record added afterwards joins the index now. The slot keeps its
     * first record, which is the source-order rule. */
    if (map->prepared &&
        !markdown_core_key_index_insert(&map->index, reflabel, (bufsize_t)strlen((char *)reflabel), record, 0, NULL)) {
        map->oom = 1;
    }
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) { return markdown_core_map_new(mem); }

void markdown_core_reference_create(markdown_core_mem *mem, markdown_core_map *map, markdown_core_chunk *label,
                                    markdown_core_resource *resource) {
    definition_create(mem, map, label, resource);
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem);
}

void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    definition_create(map ? map->mem : NULL, map, label, NULL);
}
