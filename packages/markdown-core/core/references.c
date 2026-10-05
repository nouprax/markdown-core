#include "alloc.h"
#include "buffer.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

/* Registers a record under `key`, which is already in the map's normal form. */
static markdown_core_map_record *record_create(markdown_core_map *map, const unsigned char *key, bufsize_t key_len) {
    markdown_core_map_record *record;

    /* A missing map means parser construction has already poisoned the parse;
     * keep cleanup paths null-safe while the transaction unwinds. */
    if (map == NULL || map->oom) {
        return NULL;
    }
    /* All declarations precede lookup, including virtual heading records. */
    assert(!map->prepared);

    record = markdown_core_map_carve(map, sizeof(*record) + (size_t)key_len + 1);
    if (!record) {
        map->oom = 1;
        return NULL;
    }
    memcpy(record->label, key, (size_t)key_len);
    record->label[key_len] = '\0';
    record->label_len = key_len;
    record->next = map->records;

    map->records = record;
    map->size++;
    return record;
}

markdown_core_map *markdown_core_reference_map_new(void) { return markdown_core_map_new(); }

markdown_core_map *markdown_core_footnote_definition_map_new(void) { return markdown_core_map_new(); }

void markdown_core_label_declare(markdown_core_map *map, const markdown_core_chunk *label) {
    if (label->len > 0) {
        record_create(map, label->data, label->len);
    }
}
