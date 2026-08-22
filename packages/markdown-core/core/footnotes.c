#include "markdown-core.h"
#include "parser.h"
#include "footnotes.h"
#include "inlines.h"
#include "chunk.h"

static void footnote_free(markdown_core_map *map, markdown_core_map_entry *_ref) {
    markdown_core_footnote *ref = (markdown_core_footnote *)_ref;
    markdown_core_mem *mem = map->mem;
    if (ref != NULL) {
        mem->free(ref->entry.label);
        if (ref->node) {
            markdown_core_node_free(ref->node);
        }
        mem->free(ref);
    }
}

void markdown_core_footnote_create(markdown_core_map *map, markdown_core_node *node) {
    markdown_core_footnote *ref;
    unsigned char *reflabel;
    int lost = 0;

    if (map == NULL) {
        return;
    }

    reflabel = normalize_map_label(map->mem, &node->as.literal, &lost);

    /* empty footnote name, or composed from only whitespace */
    if (reflabel == NULL) {
        if (lost) {
            map->oom = 1;
        }
        return;
    }

    assert(!map->prepared);

    ref = (markdown_core_footnote *)map->mem->calloc(1, sizeof(*ref));
    if (!ref) {
        map->oom = 1;
        map->mem->free(reflabel);
        return;
    }
    ref->entry.label = reflabel;
    ref->node = node;
    ref->entry.age = map->size;
    ref->entry.next = map->refs;

    map->refs = (markdown_core_map_entry *)ref;
    map->size++;
}

markdown_core_map *markdown_core_footnote_map_new(markdown_core_mem *mem) {
    return markdown_core_map_new(mem, footnote_free);
}

// Before calling `markdown_core_map_free` on a map with `markdown_core_footnotes`, first
// unlink all of the footnote nodes before freeing their memory.
//
// Sometimes, two (unused) footnote nodes can end up referencing each other,
// which as they get freed up by calling `markdown_core_map_free` -> `footnote_free` ->
// etc, can lead to a use-after-free error.
//
// Better to `unlink` every footnote node first, setting their next, prev, and
// parent pointers to NULL, and only then walk thru & free them up.
void markdown_core_unlink_footnotes_map(markdown_core_map *map) {
    markdown_core_map_entry *ref;
    markdown_core_map_entry *next;

    ref = map->refs;
    while (ref) {
        next = ref->next;
        if (((markdown_core_footnote *)ref)->node) {
            markdown_core_node_unlink(((markdown_core_footnote *)ref)->node);
        }
        ref = next;
    }
}
