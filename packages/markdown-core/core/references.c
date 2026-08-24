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

/* One free function for both sets: an entry is a label and nothing else. */
static void definition_free(markdown_core_map *map, markdown_core_map_entry *entry) {
    if (entry != NULL) {
        map->mem->free(entry->label);
        map->mem->free(entry);
    }
}

static void definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_entry *entry;
    unsigned char *reflabel;
    int lost = 0;

    /* The parser tolerates a missing map (map_new failure under a
     * NULL-returning allocator); definitions are then dropped. */
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

    assert(!map->prepared);

    entry = (markdown_core_map_entry *)map->mem->calloc(1, sizeof(*entry));
    if (!entry) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    entry->label = reflabel;
    entry->age = map->size;
    entry->next = map->refs;

    map->refs = entry;
    map->size++;
}

markdown_core_map *markdown_core_reference_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, definition_free);
}

void markdown_core_reference_create(markdown_core_map *map, markdown_core_chunk *label) {
    definition_create(map, label);
}

markdown_core_map *markdown_core_footnote_definition_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, definition_free);
}

void markdown_core_footnote_definition_create(markdown_core_map *map, markdown_core_chunk *label) {
    definition_create(map, label);
}
