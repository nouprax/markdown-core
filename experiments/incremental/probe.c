/* Diagnostic adapter only; never linked into a product. See README.md.
 * Measurements are thread-local and cover native requested allocation sizes,
 * excluding allocator headers, source storage, and binding projections. */
#include "markdown_core.h"
#include "parser.h"
#include "element.h"
#include "map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t allocations, requested_bytes, live_bytes, peak_bytes;
    size_t attributes, lookahead, tables, anchors, delimiters, brackets;
    size_t node_size, parser_size;
} probe_stats;

typedef union {
    size_t size;
    long double alignment;
    void *pointer;
} allocation_header;

static _Thread_local probe_stats *active;

static void account(size_t old_size, size_t new_size) {
    active->allocations++;
    active->requested_bytes += new_size;
    active->live_bytes = active->live_bytes - old_size + new_size;
    if (active->live_bytes > active->peak_bytes) {
        active->peak_bytes = active->live_bytes;
    }
}

static void *tracked_calloc(size_t count, size_t width) {
    if (count && width > (SIZE_MAX - sizeof(allocation_header)) / count) {
        return NULL;
    }
    size_t size = count * width;
    allocation_header *header = calloc(1, sizeof(*header) + size);
    if (!header) {
        return NULL;
    }
    header->size = size;
    account(0, size);
    return header + 1;
}

static void tracked_free(void *pointer) {
    if (pointer) {
        allocation_header *header = (allocation_header *)pointer - 1;
        active->live_bytes -= header->size;
        free(header);
    }
}

static void *tracked_realloc(void *pointer, size_t size) {
    if (size > SIZE_MAX - sizeof(allocation_header)) {
        return NULL;
    }
    allocation_header *old = pointer ? (allocation_header *)pointer - 1 : NULL;
    size_t old_size = old ? old->size : 0;
    allocation_header *header = realloc(old, sizeof(*header) + size);
    if (!header) {
        return NULL;
    }
    header->size = size;
    account(old_size, size);
    return header + 1;
}

static markdown_core_node *record(const markdown_core_element *element, markdown_core_parser *parser,
                                  markdown_core_node *root) {
    (void)element;
    active->attributes = parser->attribute_work;
    active->lookahead = parser->block_lookahead_work;
    active->tables = parser->table_scan_work;
    active->anchors = parser->anchor_work;
    active->delimiters = parser->delimiter_work;
    active->brackets = parser->bracket_work;
    return root;
}

static const markdown_core_element RECORDER = {.name = "experiment-recorder", .postprocess_func = record};

static bool setup(markdown_core_parser *parser, void *context) {
    (void)context;
    return markdown_core_parser_attach_element(parser, &RECORDER);
}

int mc_probe(const char *source, size_t length, probe_stats *stats) {
    markdown_core_mem mem = {tracked_calloc, tracked_realloc, tracked_free};
    memset(stats, 0, sizeof(*stats));
    active = stats;
    stats->node_size = sizeof(markdown_core_node);
    stats->parser_size = sizeof(markdown_core_parser);
    markdown_core_node *root = markdown_core_parse_document_with_mem(source, length, &mem, setup, NULL);
    int success = root != NULL;
    markdown_core_node_free(root);
    active = NULL;
    return success && stats->live_bytes == 0;
}

/* Retains the 5eca3bc1 hash to replay the original collision workload against
 * the replacement radix index. Lookup branch visits are measured structurally. */
static uint64_t input_hash(const unsigned char *key, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= key[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash ? hash : 1;
}

int mc_collisions(size_t count, char *keys, size_t *candidates, size_t *lookup_branches) {
    markdown_core_mem *mem = markdown_core_get_default_mem_allocator();
    markdown_core_key_index index;
    if (!markdown_core_key_index_init(&index, mem, count)) {
        return 0;
    }
    size_t found = 0;
    *candidates = 0;
    *lookup_branches = 0;
    size_t old_capacity = 16;
    while (old_capacity < count * 2) {
        old_capacity *= 2;
    }
    while (found < count) {
        char key[18];
        snprintf(key, sizeof(key), "x%016llx", (unsigned long long)(*candidates)++);
        uint64_t hash = input_hash((const unsigned char *)key, 17);
        if ((hash & (old_capacity - 1)) != 0) {
            continue;
        }
        char *stored = keys + found * 18;
        memcpy(stored, key, 18);
        markdown_core_key_index_slot *slot = markdown_core_key_index_entry(&index, (const unsigned char *)stored, 17);
        if (!slot || slot->key) {
            markdown_core_key_index_free(&index);
            return 0;
        }
        markdown_core_key_index_commit(&index, slot, (const unsigned char *)stored);
        slot->value.pointer = stored;
        size_t ref = index.root;
        while (!(ref & 1)) {
            markdown_core_key_index_node *node = &index.nodes[(ref >> 1) - 1];
            unsigned direction = node->byte < 17 && (node->mask == 256 || (stored[node->byte] & node->mask));
            ref = node->children[direction];
            (*lookup_branches)++;
        }
        if (markdown_core_key_index_lookup(&index, (const unsigned char *)stored, 17) != stored) {
            markdown_core_key_index_free(&index);
            return 0;
        }
        found++;
    }
    markdown_core_key_index_free(&index);
    return 1;
}
