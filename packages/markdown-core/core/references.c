#include "markdown-core.h"
#include "parser.h"
#include "references.h"
#include "inlines.h"
#include "chunk.h"

int markdown_core_association_init(markdown_core_mem *mem, markdown_core_association *out,
                                   const markdown_core_chunk *label, unsigned char prefix) {
    markdown_core_chunk raw = *label;
    unsigned char *key;
    bufsize_t length;
    int lost = 0;

    out->label.data = NULL;
    out->label.len = 0;
    out->label.alloc = 0;
    out->identifier = out->label;

    /* The label OWNS its bytes: it is read out of a block's content buffer,
     * which a harvest may drop, and it outlives the parse. */
    out->label = markdown_core_chunk_dup(label, 0, label->len);
    if (!markdown_core_chunk_to_cstr(mem, &out->label)) {
        return 0;
    }

    key = normalize_map_label(mem, &raw, &lost);
    if (key == NULL) {
        markdown_core_chunk_free(mem, &out->label);
        return 0;
    }
    length = (bufsize_t)strlen((char *)key);
    if (prefix) {
        unsigned char *prefixed = (unsigned char *)mem->calloc((size_t)length + 2, 1);
        if (!prefixed) {
            mem->free(key);
            markdown_core_chunk_free(mem, &out->label);
            return 0;
        }
        prefixed[0] = prefix;
        memcpy(prefixed + 1, key, (size_t)length);
        mem->free(key);
        key = prefixed;
        length += 1;
    }
    out->identifier.data = key;
    out->identifier.len = length;
    out->identifier.alloc = 1;
    return 1;
}

void markdown_core_association_free(markdown_core_mem *mem, markdown_core_association *association) {
    markdown_core_chunk_free(mem, &association->label);
    markdown_core_chunk_free(mem, &association->identifier);
}

static void definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_record *record;
    unsigned char *reflabel;
    int lost = 0;

    /* A missing map means parser construction has already poisoned the parse;
     * keep cleanup paths null-safe while the transaction unwinds. */
    if (map == NULL) {
        return;
    }

    reflabel = normalize_map_label(map->mem, label, &lost);
    /* An empty label, or one that is all whitespace, defines nothing. */
    if (reflabel == NULL) {
        if (lost) {
            map->oom = 1;
        }
        return;
    }

    record = (markdown_core_map_record *)map->mem->calloc(1, sizeof(*record));
    if (!record) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    record->label = reflabel;
    record->next = map->records;

    map->records = record;
    map->size++;
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) { return markdown_core_map_new(mem); }

void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label) {
    definition_create(map, label);
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem);
}

void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    definition_create(map, label);
}
