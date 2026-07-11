#include "map.h"
#include "utf8.h"
#include "parser.h"

// normalize map label:  collapse internal whitespace to single space,
// remove leading/trailing whitespace, case fold
// Return NULL if the label is actually empty (i.e. composed solely from
// whitespace)
unsigned char *normalize_map_label(markdown_core_mem *mem, markdown_core_chunk *ref) {
    markdown_core_strbuf normalized = MARKDOWN_CORE_BUF_INIT(mem);
    unsigned char *result;

    if (ref == NULL)
        return NULL;

    if (ref->len == 0)
        return NULL;

    markdown_core_utf8proc_case_fold(&normalized, ref->data, ref->len);
    markdown_core_strbuf_trim(&normalized);
    markdown_core_strbuf_normalize_whitespace(&normalized);

    result = markdown_core_strbuf_detach(&normalized);
    assert(result);

    if (result[0] == '\0') {
        mem->free(result);
        return NULL;
    }

    return result;
}

static int labelcmp(const unsigned char *a, const unsigned char *b) { return strcmp((const char *)a, (const char *)b); }

static int refcmp(const void *p1, const void *p2) {
    markdown_core_map_entry *r1 = *(markdown_core_map_entry **)p1;
    markdown_core_map_entry *r2 = *(markdown_core_map_entry **)p2;
    int res = labelcmp(r1->label, r2->label);
    return res ? res : ((int)r1->age - (int)r2->age);
}

static int refsearch(const void *label, const void *p2) {
    markdown_core_map_entry *ref = *(markdown_core_map_entry **)p2;
    return labelcmp((const unsigned char *)label, ref->label);
}

static void sort_map(markdown_core_map *map) {
    size_t i = 0, last = 0, size = map->size;
    markdown_core_map_entry *r = map->refs, **sorted = NULL;

    sorted = (markdown_core_map_entry **)map->mem->calloc(size, sizeof(markdown_core_map_entry *));
    while (r) {
        sorted[i++] = r;
        r = r->next;
    }

    qsort(sorted, size, sizeof(markdown_core_map_entry *), refcmp);

    for (i = 1; i < size; i++) {
        if (labelcmp(sorted[i]->label, sorted[last]->label) != 0)
            sorted[++last] = sorted[i];
    }

    map->sorted = sorted;
    map->size = last + 1;
}

markdown_core_map_entry *markdown_core_map_lookup(markdown_core_map *map, markdown_core_chunk *label) {
    markdown_core_map_entry **ref = NULL;
    markdown_core_map_entry *r = NULL;
    unsigned char *norm;

    if (label->len < 1 || label->len > MAX_LINK_LABEL_LENGTH)
        return NULL;

    if (map == NULL || !map->size)
        return NULL;

    norm = normalize_map_label(map->mem, label);
    if (norm == NULL)
        return NULL;

    if (!map->sorted)
        sort_map(map);

    ref =
        (markdown_core_map_entry **)bsearch(norm, map->sorted, map->size, sizeof(markdown_core_map_entry *), refsearch);
    map->mem->free(norm);

    if (ref != NULL) {
        r = ref[0];
        /* Check for expansion limit */
        if (r->size > map->max_ref_size - map->ref_size)
            return NULL;
        map->ref_size += r->size;
    }

    return r;
}

void markdown_core_map_free(markdown_core_map *map) {
    markdown_core_map_entry *ref;

    if (map == NULL)
        return;

    ref = map->refs;
    while (ref) {
        markdown_core_map_entry *next = ref->next;
        map->free(map, ref);
        ref = next;
    }

    map->mem->free(map->sorted);
    map->mem->free(map);
}

markdown_core_map *markdown_core_map_new(markdown_core_mem *mem, markdown_core_map_free_f free) {
    markdown_core_map *map = (markdown_core_map *)mem->calloc(1, sizeof(markdown_core_map));
    map->mem = mem;
    map->free = free;
    map->max_ref_size = UINT_MAX;
    return map;
}
