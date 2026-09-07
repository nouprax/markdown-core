#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "markdown_core.h"

/*
 * ES receives one immutable result in WebAssembly linear memory. The ABI is
 * deliberately table-shaped rather than a recursive byte stream: every node
 * is a fixed-width record, relationships are indexes, and strings occupy one
 * trailing UTF-8 blob. JavaScript can therefore build the value tree without
 * another Wasm call, a native object handle, or recursion in the decoder.
 */

enum { ES_HEADER_SIZE = 64, ES_NODE_SIZE = 136, ES_ATTRIBUTE_SIZE = 16, ES_COLUMN_SIZE = 16 };
static const uint32_t ES_NO_INDEX = UINT32_MAX;

enum es_header_offset {
    ES_HEADER_TOTAL_SIZE = 4,
    ES_HEADER_STATUS = 8,
    ES_HEADER_ERROR_CODE = 12,
    ES_HEADER_ERROR_OFFSET = 16,
    ES_HEADER_ERROR_LENGTH = 20,
    ES_HEADER_NODE_COUNT = 24,
    ES_HEADER_EDGE_COUNT = 28,
    ES_HEADER_ATTRIBUTE_COUNT = 32,
    ES_HEADER_COLUMN_COUNT = 36,
    ES_HEADER_NODES_OFFSET = 40,
    ES_HEADER_EDGES_OFFSET = 44,
    ES_HEADER_ATTRIBUTES_OFFSET = 48,
    ES_HEADER_COLUMNS_OFFSET = 52,
    ES_HEADER_STRINGS_OFFSET = 56,
    ES_HEADER_STRINGS_LENGTH = 60
};

enum es_node_offset {
    ES_NODE_KIND = 0,
    ES_NODE_FLAGS = 4,
    ES_NODE_SCOPE = 8,
    ES_NODE_CHILD_START = 24,
    ES_NODE_CHILD_COUNT = 28,
    ES_NODE_LABEL_INDEX = 32,
    ES_NODE_AUX_START = 36,
    ES_NODE_AUX_COUNT = 40,
    ES_NODE_SCALAR0 = 44,
    ES_NODE_INTEGER2 = 48,
    ES_NODE_I64 = 56,
    ES_NODE_STRINGS = 64,
    ES_NODE_ANCHOR = 96,
    ES_NODE_CLASSES_START = 104,
    ES_NODE_CLASSES_COUNT = 108,
    ES_NODE_RECORDS_START = 112,
    ES_NODE_RECORDS_COUNT = 116,
    ES_NODE_METADATA = 120,
    ES_NODE_WIDTH = 124,
    ES_NODE_HEIGHT = 128
};

/* The wire kinds of the scoped values (M4), above the node-kind space
 * so that the decoder tells a value record from a node record by its kind. */
enum {
    ES_KIND_CITATION = 0x100,
    ES_KIND_FOOTNOTE = 0x101,
    ES_KIND_SPECIMEN = 0x102,
    ES_KIND_METADATA = 0x103,
    ES_KIND_METADATA_RECORD = 0x104
};

typedef struct es_source_node {
    /* A node record names its node; a value record names its handle instead
     * and leaves `node` NULL (M4). */
    const markdown_core_node *node;
    const markdown_core_citation *citation;
    const markdown_core_footnote *footnote;
    const markdown_core_specimen *specimen;
    const markdown_core_metadata *metadata;
    const markdown_core_metadata_record *metadata_record;
    markdown_core_optional_string anchor;
    uint32_t class_start, class_count, record_start, record_count, metadata_index;
    uint32_t width, height;
    uint32_t wire_kind;
    uint32_t child_start;
    uint32_t child_count;
    uint32_t label_index;
    uint32_t aux_start;
    uint32_t aux_count;
    uint32_t flags;
    int32_t scalar0;
    int64_t integer;
    int64_t integer2;
    markdown_core_optional_string strings[4];
    /* For a link or image, the index of the first node reading through the
     * same resource; ES_NO_INDEX otherwise. */
    uint32_t resource_first;
} es_source_node;

/* Every occurrence of one reference definition reads through one resource in
 * the C tree. This table remembers, per resource identity, the first node
 * that read through it, so a destination and title cross the boundary once
 * however often the definition is used. */
typedef struct es_resource_slot {
    const markdown_core_resource *resource;
    uint32_t first;
} es_resource_slot;

typedef struct es_source_attribute {
    markdown_core_string name;
    markdown_core_string value;
} es_source_attribute;

typedef enum es_build_failure { ES_BUILD_OK = 0, ES_BUILD_ALLOCATION, ES_BUILD_INTERNAL } es_build_failure;

typedef struct es_build {
    es_source_node *nodes;
    size_t node_count;
    size_t node_capacity;
    uint32_t *edges;
    size_t edge_count;
    size_t edge_capacity;
    es_source_attribute *attributes;
    size_t attribute_count;
    size_t attribute_capacity;
    markdown_core_table_column *columns;
    size_t column_count;
    size_t column_capacity;
    es_resource_slot *resources;
    size_t resource_count;
    size_t resource_capacity;
    size_t strings_length;
    es_build_failure failure;
} es_build;

static void put_u32(uint8_t *output, size_t offset, uint32_t value) {
    size_t index;
    for (index = 0; index < 4; ++index) {
        output[offset + index] = (uint8_t)(value >> (index * 8));
    }
}

static void put_i32(uint8_t *output, size_t offset, int32_t value) { put_u32(output, offset, (uint32_t)value); }

static void put_i64(uint8_t *output, size_t offset, int64_t value) {
    uint64_t bits = (uint64_t)value;
    size_t index;
    for (index = 0; index < 8; ++index) {
        output[offset + index] = (uint8_t)(bits >> (index * 8));
    }
}

static bool add_size(size_t *value, size_t additional) {
    if (additional > UINT32_MAX - *value) {
        return false;
    }
    *value += additional;
    return true;
}

static bool section_end(size_t start, size_t count, size_t width, size_t *end) {
    if (count > UINT32_MAX / width || count * width > UINT32_MAX - start) {
        return false;
    }
    *end = start + count * width;
    return true;
}

static bool reserve_vector(void **values, size_t *capacity, size_t required, size_t width) {
    size_t next;
    void *grown;
    if (required <= *capacity) {
        return true;
    }
    if (required > UINT32_MAX || required > SIZE_MAX / width) {
        return false;
    }
    next = *capacity == 0 ? 64 : *capacity;
    while (next < required) {
        if (next > SIZE_MAX / 2) {
            next = required;
            break;
        }
        next *= 2;
    }
    if (next > SIZE_MAX / width) {
        return false;
    }
    grown = realloc(*values, next * width);
    if (grown == NULL) {
        return false;
    }
    *values = grown;
    *capacity = next;
    return true;
}

static uint32_t append_record(es_build *build, es_source_node value) {
    uint32_t index;
    if (build->failure != ES_BUILD_OK || build->node_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->nodes, &build->node_capacity, build->node_count + 1, sizeof(*build->nodes))) {
        build->failure = ES_BUILD_ALLOCATION;
        return ES_NO_INDEX;
    }
    value.label_index = ES_NO_INDEX;
    value.metadata_index = ES_NO_INDEX;
    value.aux_start = ES_NO_INDEX;
    value.resource_first = ES_NO_INDEX;
    index = (uint32_t)build->node_count;
    build->nodes[build->node_count++] = value;
    return index;
}

static uint32_t append_node(es_build *build, const markdown_core_node *node) {
    es_source_node value;
    memset(&value, 0, sizeof(value));
    value.node = node;
    value.wire_kind = (uint32_t)markdown_core_node_get_kind(node);
    return append_record(build, value);
}

static uint32_t append_citation(es_build *build, const markdown_core_citation *citation) {
    es_source_node value;
    memset(&value, 0, sizeof(value));
    value.citation = citation;
    value.wire_kind = ES_KIND_CITATION;
    return append_record(build, value);
}

static uint32_t append_footnote(es_build *build, const markdown_core_footnote *footnote) {
    es_source_node value;
    memset(&value, 0, sizeof(value));
    value.footnote = footnote;
    value.wire_kind = ES_KIND_FOOTNOTE;
    return append_record(build, value);
}

static uint32_t append_specimen(es_build *build, const markdown_core_specimen *specimen) {
    es_source_node value;
    memset(&value, 0, sizeof(value));
    value.specimen = specimen;
    value.wire_kind = ES_KIND_SPECIMEN;
    return append_record(build, value);
}

static void append_edge(es_build *build, uint32_t node_index) {
    if (build->failure != ES_BUILD_OK || build->edge_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->edges, &build->edge_capacity, build->edge_count + 1, sizeof(*build->edges))) {
        build->failure = ES_BUILD_ALLOCATION;
        return;
    }
    build->edges[build->edge_count++] = node_index;
}

static void append_attribute(es_build *build, markdown_core_string name, markdown_core_string value) {
    es_source_attribute attribute;
    if (build->failure != ES_BUILD_OK || build->attribute_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->attributes, &build->attribute_capacity, build->attribute_count + 1,
                        sizeof(*build->attributes))) {
        build->failure = ES_BUILD_ALLOCATION;
        return;
    }
    attribute.name = name;
    attribute.value = value;
    build->attributes[build->attribute_count++] = attribute;
}

static void append_column(es_build *build, markdown_core_table_column column) {
    if (build->failure != ES_BUILD_OK || build->column_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->columns, &build->column_capacity, build->column_count + 1,
                        sizeof(*build->columns))) {
        build->failure = ES_BUILD_ALLOCATION;
        return;
    }
    build->columns[build->column_count++] = column;
}

static size_t hash_resource(const markdown_core_resource *resource) {
    uint64_t bits = (uint64_t)(uintptr_t)resource;
    bits ^= bits >> 33;
    bits *= UINT64_C(0xff51afd7ed558ccd);
    bits ^= bits >> 33;
    bits *= UINT64_C(0xc4ceb9fe1a85ec53);
    bits ^= bits >> 33;
    return (size_t)bits;
}

/* The slot holding `resource`, or the empty slot it would take. The table is
 * never more than half full, so the probe always ends. */
static es_resource_slot *find_resource_slot(es_resource_slot *slots, size_t capacity,
                                            const markdown_core_resource *resource) {
    size_t position = hash_resource(resource) & (capacity - 1);
    for (;;) {
        es_resource_slot *slot = &slots[position];
        if (slot->resource == NULL || slot->resource == resource) {
            return slot;
        }
        position = (position + 1) & (capacity - 1);
    }
}

static bool grow_resources(es_build *build) {
    size_t capacity = build->resource_capacity == 0 ? 64 : build->resource_capacity * 2;
    es_resource_slot *slots;
    size_t index;
    if (capacity < build->resource_capacity || capacity > SIZE_MAX / sizeof(*slots)) {
        return false;
    }
    slots = (es_resource_slot *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    for (index = 0; index < build->resource_capacity; ++index) {
        const es_resource_slot *source = &build->resources[index];
        if (source->resource != NULL) {
            *find_resource_slot(slots, capacity, source->resource) = *source;
        }
    }
    free(build->resources);
    build->resources = slots;
    build->resource_capacity = capacity;
    return true;
}

/* The index of the first node reading through `resource`, which is
 * `node_index` itself the first time the resource is seen. */
static uint32_t resource_first(es_build *build, const markdown_core_resource *resource, uint32_t node_index) {
    es_resource_slot *slot;
    if (build->resource_count + 1 > build->resource_capacity / 2 && !grow_resources(build)) {
        build->failure = ES_BUILD_ALLOCATION;
        return ES_NO_INDEX;
    }
    slot = find_resource_slot(build->resources, build->resource_capacity, resource);
    if (slot->resource == NULL) {
        slot->resource = resource;
        slot->first = node_index;
        build->resource_count++;
    }
    return slot->first;
}

static markdown_core_optional_string required_string(markdown_core_string value) {
    markdown_core_optional_string result;
    result.has_value = true;
    result.value = value;
    return result;
}

static void count_string(es_build *build, markdown_core_optional_string value) {
    if (value.has_value && !add_size(&build->strings_length, value.value.length)) {
        build->failure = ES_BUILD_ALLOCATION;
    }
}

static bool is_directive(markdown_core_node_kind kind) {
    return kind == MARKDOWN_CORE_KIND_DIRECTIVE || kind == MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK;
}

/* Appends every node of a sibling chain as a record and an edge and answers
 * the edge count, or SIZE_MAX once the build has failed. */
static size_t append_chain(es_build *build, const markdown_core_node *first) {
    size_t count = 0;
    for (; first != NULL && build->failure == ES_BUILD_OK; first = markdown_core_node_get_next_sibling(first)) {
        uint32_t index = append_node(build, first);
        if (index == ES_NO_INDEX) {
            return SIZE_MAX;
        }
        append_edge(build, index);
        count++;
    }
    return build->failure == ES_BUILD_OK ? count : SIZE_MAX;
}

/* A citation's prefix is its child range and its suffix its auxiliary range;
 * a footnote's content is its child range (M4). Both chains are records like
 * any other node's children. */
static void collect_value_topology(es_build *build, size_t cursor) {
    const markdown_core_metadata *metadata = build->nodes[cursor].metadata;
    if (metadata) {
        size_t count = markdown_core_metadata_record_count(metadata);
        build->nodes[cursor].child_start = (uint32_t)build->edge_count;
        if (count > UINT32_MAX) {
            build->failure = ES_BUILD_ALLOCATION;
            return;
        }
        for (size_t i = 0; i < count && build->failure == ES_BUILD_OK; i++) {
            es_source_node value = {0};
            value.wire_kind = ES_KIND_METADATA_RECORD;
            value.metadata_record = markdown_core_metadata_record_at(metadata, i);
            uint32_t index = append_record(build, value);
            if (index == ES_NO_INDEX) {
                return;
            }
            append_edge(build, index);
            build->nodes[cursor].child_count++;
        }
        return;
    }
    if (build->nodes[cursor].metadata_record) {
        return;
    }
    const markdown_core_citation *citation = build->nodes[cursor].citation;
    const markdown_core_footnote *footnote = build->nodes[cursor].footnote;
    const markdown_core_specimen *specimen = build->nodes[cursor].specimen;
    size_t count;
    build->nodes[cursor].child_start = (uint32_t)build->edge_count;
    count = append_chain(build, citation   ? markdown_core_citation_prefix(citation)
                                : footnote ? markdown_core_footnote_content(footnote)
                                           : markdown_core_specimen_content(specimen));
    if (count == SIZE_MAX) {
        return;
    }
    build->nodes[cursor].child_count = (uint32_t)count;
    if (citation) {
        build->nodes[cursor].aux_start = (uint32_t)build->edge_count;
        count = append_chain(build, markdown_core_citation_suffix(citation));
        if (count == SIZE_MAX) {
            return;
        }
        build->nodes[cursor].aux_count = (uint32_t)count;
    }
}

/* Breadth-first indexes guarantee that every relation points forward. The JS
 * decoder can consequently build records in reverse order without recursion. */
static void collect_topology(es_build *build, const markdown_core_node *root) {
    size_t cursor;
    append_node(build, root);
    for (cursor = 0; cursor < build->node_count && build->failure == ES_BUILD_OK; ++cursor) {
        const markdown_core_node *node = build->nodes[cursor].node;
        markdown_core_node_kind kind;
        const markdown_core_node *child;
        size_t count;
        size_t index;

        if (node == NULL) {
            collect_value_topology(build, cursor);
            continue;
        }
        kind = markdown_core_node_get_kind(node);
        if (kind == MARKDOWN_CORE_KIND_NONE) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        if (kind == MARKDOWN_CORE_KIND_DOCUMENT) {
            const markdown_core_metadata *metadata = markdown_core_node_document_metadata(node);
            if (metadata) {
                es_source_node value = {0};
                value.wire_kind = ES_KIND_METADATA;
                value.metadata = metadata;
                uint32_t index = append_record(build, value);
                if (index == ES_NO_INDEX) {
                    break;
                }
                build->nodes[cursor].metadata_index = index;
            }
        }
        if (kind == MARKDOWN_CORE_KIND_CITE) {
            /* A cite's items are its child range: value records rather than
             * nodes, one per item in source order (M4). */
            const markdown_core_citation *item;
            build->nodes[cursor].child_start = (uint32_t)build->edge_count;
            for (item = markdown_core_node_cite_citations(node); item != NULL && build->failure == ES_BUILD_OK;
                 item = markdown_core_citation_next(item)) {
                uint32_t item_index = append_citation(build, item);
                if (item_index == ES_NO_INDEX) {
                    break;
                }
                append_edge(build, item_index);
                build->nodes[cursor].child_count++;
            }
            continue;
        }
        if (is_directive(kind)) {
            const markdown_core_node *label = markdown_core_node_directive_label(node);
            if (label != NULL) {
                uint32_t label_index = append_node(build, label);
                if (label_index == ES_NO_INDEX) {
                    break;
                }
                build->nodes[cursor].label_index = label_index;
            }
        }

        if (kind == MARKDOWN_CORE_KIND_CALLOUT) {
            /* The title is a node-valued list the callout owns beside its
             * content: its nodes are records like any other, and the
             * auxiliary range names them in the edge table; a present title
             * holds at least one node, so the range's count is its presence. */
            const markdown_core_node *title = markdown_core_node_callout_title(node);
            if (title != NULL) {
                build->nodes[cursor].aux_start = (uint32_t)build->edge_count;
                for (; title != NULL && build->failure == ES_BUILD_OK;
                     title = markdown_core_node_get_next_sibling(title)) {
                    uint32_t title_index = append_node(build, title);
                    if (title_index == ES_NO_INDEX) {
                        break;
                    }
                    append_edge(build, title_index);
                    build->nodes[cursor].aux_count++;
                }
                if (build->failure != ES_BUILD_OK) {
                    break;
                }
            }
        }

        count = markdown_core_node_child_count(node);
        if (count > UINT32_MAX || build->edge_count > UINT32_MAX - count) {
            build->failure = ES_BUILD_ALLOCATION;
            break;
        }
        build->nodes[cursor].child_start = (uint32_t)build->edge_count;
        build->nodes[cursor].child_count = (uint32_t)count;
        child = markdown_core_node_get_first_child(node);
        for (index = 0; index < count; ++index) {
            uint32_t child_index;
            if (child == NULL) {
                build->failure = ES_BUILD_INTERNAL;
                break;
            }
            child_index = append_node(build, child);
            if (child_index == ES_NO_INDEX) {
                break;
            }
            append_edge(build, child_index);
            child = markdown_core_node_get_next_sibling(child);
        }
        if (build->failure == ES_BUILD_OK && child != NULL) {
            build->failure = ES_BUILD_INTERNAL;
        }

        if (kind == MARKDOWN_CORE_KIND_DOCUMENT && build->failure == ES_BUILD_OK) {
            /* The document's definitions are its auxiliary range: value
             * records after the content, in scope order (M4). */
            const markdown_core_footnote *footnote;
            build->nodes[cursor].aux_start = (uint32_t)build->edge_count;
            for (footnote = markdown_core_node_document_footnotes(node);
                 footnote != NULL && build->failure == ES_BUILD_OK; footnote = markdown_core_footnote_next(footnote)) {
                uint32_t footnote_index = append_footnote(build, footnote);
                if (footnote_index == ES_NO_INDEX) {
                    break;
                }
                append_edge(build, footnote_index);
                build->nodes[cursor].aux_count++;
            }
            const markdown_core_specimen *specimen;
            for (specimen = markdown_core_node_document_specimens(node);
                 specimen != NULL && build->failure == ES_BUILD_OK; specimen = markdown_core_specimen_next(specimen)) {
                uint32_t specimen_index = append_specimen(build, specimen);
                if (specimen_index == ES_NO_INDEX) {
                    break;
                }
                append_edge(build, specimen_index);
                build->nodes[cursor].aux_count++;
            }
        }
    }
}

/* A value record's fields (M4): a citation's referent branch is the scalar,
 * its bib mode the integer, and its key or id the first string; a footnote's
 * id is the first string. */
static void collect_value_fields(es_build *build, es_source_node *record) {
    if (record->metadata) {
        return;
    }
    if (record->metadata_record) {
        const markdown_core_metadata_record *metadata = record->metadata_record;
        record->strings[0] = required_string(markdown_core_metadata_record_name(metadata));
        record->scalar0 = (int32_t)markdown_core_metadata_record_kind(metadata);
        if (record->scalar0 == MARKDOWN_CORE_METADATA_SCALAR) {
            markdown_core_metadata_scalar value;
            if (!markdown_core_metadata_record_scalar(metadata, &value)) {
                build->failure = ES_BUILD_INTERNAL;
                return;
            }
            record->flags = (uint32_t)value.kind;
            if (value.kind == MARKDOWN_CORE_METADATA_BOOL) {
                record->integer = value.value.boolean ? 1 : 0;
            } else if (value.kind == MARKDOWN_CORE_METADATA_NUMBER || value.kind == MARKDOWN_CORE_METADATA_TEXT) {
                record->strings[1] = required_string(value.value.string);
            } else if (value.kind != MARKDOWN_CORE_METADATA_NULL) {
                build->failure = ES_BUILD_INTERNAL;
                return;
            }
        } else if (record->scalar0 == MARKDOWN_CORE_METADATA_LIST) {
            size_t count = markdown_core_metadata_record_item_count(metadata);
            if (count > UINT32_MAX) {
                build->failure = ES_BUILD_ALLOCATION;
                return;
            }
            record->aux_start = (uint32_t)build->attribute_count;
            record->aux_count = (uint32_t)count;
            for (size_t i = 0; i < count; i++) {
                markdown_core_metadata_list_item item;
                if (!markdown_core_metadata_record_item_at(metadata, i, &item)) {
                    build->failure = ES_BUILD_INTERNAL;
                    return;
                }
                const char *tag;
                if (item.kind == MARKDOWN_CORE_METADATA_ITEM_NUMBER) {
                    tag = "number";
                } else if (item.kind == MARKDOWN_CORE_METADATA_ITEM_TEXT) {
                    tag = "text";
                } else {
                    build->failure = ES_BUILD_INTERNAL;
                    return;
                }
                append_attribute(build, (markdown_core_string){(const uint8_t *)tag, strlen(tag)}, item.value);
            }
        } else {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        count_string(build, record->strings[0]);
        count_string(build, record->strings[1]);
        return;
    }
    if (record->citation) {
        markdown_core_referent referent;
        if (!markdown_core_citation_referent(record->citation, &referent)) {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        record->scalar0 = (int32_t)referent.kind;
        if (referent.kind == MARKDOWN_CORE_REFERENT_BIB) {
            record->integer = (int64_t)referent.mode;
            record->strings[0] = required_string(referent.key);
        } else {
            record->strings[0] = required_string(referent.id);
        }
    } else if (record->specimen) {
        markdown_core_optional_i64 start;
        if (!markdown_core_specimen_properties(record->specimen, &record->strings[0], &start)) {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        record->integer = start.value;
        record->flags = start.has_value ? 1 : 0;
    } else {
        markdown_core_string id;
        if (!markdown_core_footnote_id(record->footnote, &id)) {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        record->strings[0] = required_string(id);
    }
    count_string(build, record->strings[0]);
}

static void collect_node_fields(es_build *build, size_t node_index) {
    es_source_node *record = &build->nodes[node_index];
    const markdown_core_node *node = record->node;
    markdown_core_node_kind kind;
    markdown_core_string first = {0};
    markdown_core_string second = {0};
    markdown_core_string third = {0};
    markdown_core_optional_string optional_first = {0};
    markdown_core_optional_string optional_second = {0};

    if (node == NULL) {
        collect_value_fields(build, record);
        return;
    }
    record->anchor = markdown_core_node_anchor(node);
    count_string(build, record->anchor);
    size_t classes = markdown_core_node_attribute_class_count(node),
           records = markdown_core_node_attribute_record_count(node);
    if (classes > UINT32_MAX || records > UINT32_MAX) {
        build->failure = ES_BUILD_ALLOCATION;
        return;
    }
    record->class_start = (uint32_t)build->attribute_count;
    record->class_count = (uint32_t)classes;
    for (size_t i = 0; i < classes; i++) {
        markdown_core_string value;
        if (!markdown_core_node_attribute_class_at(node, i, &value)) {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        append_attribute(build, (markdown_core_string){NULL, 0}, value);
    }
    record->record_start = (uint32_t)build->attribute_count;
    record->record_count = (uint32_t)records;
    for (size_t i = 0; i < records; i++) {
        markdown_core_string name, value;
        if (!markdown_core_node_attribute_record_at(node, i, &name, &value)) {
            build->failure = ES_BUILD_INTERNAL;
            return;
        }
        append_attribute(build, name, value);
    }
    kind = markdown_core_node_get_kind(node);
    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT: {
        /* The variant is the first slot and the fold marker the scalar, as
         * a list item's task marker; the title relation was recorded with
         * the topology. */
        markdown_core_optional_bool collapsed;
        if (!markdown_core_node_callout_properties(node, &optional_first, &collapsed)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = optional_first;
        record->scalar0 = collapsed.has_value ? (collapsed.value ? 1 : 0) : -1;
        break;
    }
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_CITE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        if (!markdown_core_node_heading_level(node, &record->scalar0)) {
            build->failure = ES_BUILD_INTERNAL;
        }
        break;
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor;
        markdown_core_ordered_list_variant variant;
        markdown_core_ordered_list_delimiter delimiter;
        markdown_core_optional_i64 start;
        bool tight = false;
        if (!markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->scalar0 = (int32_t)flavor;
        record->integer = start.value;
        record->flags =
            (start.has_value ? 1u : 0u) | (tight ? 2u : 0u) |
            (start.has_value ? ((uint32_t)variant.kind << 2) | ((uint32_t)delimiter.kind << 5) |
                                   (delimiter.closed ? (1u << 8) : 0u) | (variant.lowercased ? (1u << 9) : 0u)
                             : 0u);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_string marker;
        if (!markdown_core_node_list_item_marker(node, &marker)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = marker;
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced = false;
        bool closed = false;
        if (!markdown_core_node_code_block_properties(node, &optional_first, &optional_second, &third, &fenced,
                                                      &closed)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = optional_first;
        record->strings[1] = optional_second;
        record->strings[2] = required_string(third);
        record->flags = (fenced ? 1u : 0u) | (closed ? 2u : 0u);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        if (!markdown_core_node_literal(node, &first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        break;
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->scalar0 = (int32_t)mode;
        record->strings[0] = required_string(first);
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: {
        markdown_core_placement_mode mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count, head, content, foot;
        if (!markdown_core_node_table_properties(node, &count, &head, &content, &foot) || count > UINT32_MAX ||
            head > INT32_MAX || content > INT32_MAX || foot > INT32_MAX || build->column_count > UINT32_MAX - count) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->aux_start = (uint32_t)build->column_count;
        record->aux_count = (uint32_t)count;
        record->scalar0 = (int32_t)head;
        record->integer2 = (int64_t)content;
        record->integer = (int64_t)foot;
        for (size_t index = 0; index < count; ++index) {
            markdown_core_table_column column;
            if (!markdown_core_node_table_column_at(node, index, &column)) {
                build->failure = ES_BUILD_INTERNAL;
                break;
            }
            append_column(build, column);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        if (!markdown_core_node_directive_properties(node, &first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        break;
    }
    case MARKDOWN_CORE_KIND_CROSS_LINK: {
        markdown_core_destination destination;
        bool embedded;
        if (!markdown_core_node_destination(node, &destination) ||
            !markdown_core_node_cross_link_properties(node, &embedded, &optional_first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->flags = embedded ? 1 : 0;
        record->scalar0 = (int32_t)destination.kind;
        record->strings[0] = required_string(destination.path);
        record->strings[1] = destination.anchor;
        record->strings[2] = optional_first;
        break;
    }
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_IMAGE: {
        if (kind == MARKDOWN_CORE_KIND_IMAGE) {
            markdown_core_optional_i64 width, height;
            if (!markdown_core_node_image_dimensions(node, &width, &height) ||
                (width.has_value && (width.value < 1 || width.value > INT32_MAX)) ||
                (height.has_value && (height.value < 1 || height.value > INT32_MAX))) {
                build->failure = ES_BUILD_INTERNAL;
                break;
            }
            record->width = width.has_value ? (uint32_t)width.value : 0;
            record->height = height.has_value ? (uint32_t)height.value : 0;
        }
        /* The tagged `Destination`: the branch is the scalar, its strings are
         * the first slots -- the url, or the path and the optional anchor --
         * and the title is the third, so a slot never means two things. The
         * integer names the first node reading through the same resource:
         * that node carries the strings, and every later occurrence has the
         * same slot references copied at write time, so a resource crosses
         * the boundary once and the decoder materializes it once. */
        markdown_core_destination destination;
        const markdown_core_resource *resource = markdown_core_node_resource(node);
        uint32_t first;
        if (resource == NULL) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        first = resource_first(build, resource, (uint32_t)node_index);
        if (build->failure != ES_BUILD_OK) {
            break;
        }
        record->resource_first = first;
        record->integer = (int64_t)first;
        if (first != node_index) {
            record->scalar0 = build->nodes[first].scalar0;
            break;
        }
        if (!markdown_core_node_destination(node, &destination) || !markdown_core_node_title(node, &optional_first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->scalar0 = (int32_t)destination.kind;
        if (destination.kind == MARKDOWN_CORE_DESTINATION_CROSS) {
            record->strings[0] = required_string(destination.path);
            record->strings[1] = destination.anchor;
        } else {
            record->strings[0] = required_string(destination.url);
        }
        record->strings[2] = optional_first;
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_CELL:
        if (!markdown_core_node_table_cell_spans(node, &record->integer, &record->integer2)) {
            build->failure = ES_BUILD_INTERNAL;
        }
        break;
    case MARKDOWN_CORE_KIND_NONE:
    default:
        build->failure = ES_BUILD_INTERNAL;
        break;
    }

    if (build->failure == ES_BUILD_OK) {
        size_t index;
        for (index = 0; index < 4; ++index) {
            count_string(build, record->strings[index]);
        }
    }
}

static void collect_fields(es_build *build) {
    size_t index;
    for (index = 0; index < build->node_count && build->failure == ES_BUILD_OK; ++index) {
        collect_node_fields(build, index);
    }
    for (index = 0; index < build->attribute_count && build->failure == ES_BUILD_OK; ++index) {
        count_string(build, required_string(build->attributes[index].name));
        count_string(build, required_string(build->attributes[index].value));
    }
}

static void free_build(es_build *build) {
    free(build->nodes);
    free(build->edges);
    free(build->attributes);
    free(build->columns);
    free(build->resources);
}

static uint8_t *error_result(markdown_core_error_code code, markdown_core_string message) {
    static const uint8_t magic[] = {'M', 'C', 'B', '1'};
    size_t total_size = ES_HEADER_SIZE;
    uint8_t *output;
    if (!add_size(&total_size, message.length)) {
        return NULL;
    }
    output = (uint8_t *)malloc(total_size);
    if (output == NULL) {
        return NULL;
    }
    memset(output, 0, ES_HEADER_SIZE);
    memcpy(output, magic, sizeof(magic));
    put_u32(output, ES_HEADER_TOTAL_SIZE, (uint32_t)total_size);
    put_u32(output, ES_HEADER_STATUS, 1);
    put_i32(output, ES_HEADER_ERROR_CODE, (int32_t)code);
    put_u32(output, ES_HEADER_ERROR_OFFSET, ES_HEADER_SIZE);
    put_u32(output, ES_HEADER_ERROR_LENGTH, (uint32_t)message.length);
    if (message.length != 0) {
        memcpy(output + ES_HEADER_SIZE, message.data, message.length);
    }
    return output;
}

static void write_string_reference(uint8_t *output, size_t reference_offset, markdown_core_optional_string value,
                                   size_t *cursor) {
    if (!value.has_value) {
        put_u32(output, reference_offset, ES_NO_INDEX);
        put_u32(output, reference_offset + 4, 0);
        return;
    }
    put_u32(output, reference_offset, (uint32_t)*cursor);
    put_u32(output, reference_offset + 4, (uint32_t)value.value.length);
    if (value.value.length != 0) {
        memcpy(output + *cursor, value.value.data, value.value.length);
        *cursor += value.value.length;
    }
}

static uint8_t *success_result(const es_build *build, es_build_failure *failure) {
    static const uint8_t magic[] = {'M', 'C', 'B', '1'};
    size_t nodes_offset = ES_HEADER_SIZE;
    size_t edges_offset;
    size_t attributes_offset;
    size_t columns_offset;
    size_t strings_offset;
    size_t total_size;
    size_t string_cursor;
    size_t index;
    uint8_t *output;

    *failure = ES_BUILD_OK;
    if (!section_end(nodes_offset, build->node_count, ES_NODE_SIZE, &edges_offset) ||
        !section_end(edges_offset, build->edge_count, sizeof(uint32_t), &attributes_offset) ||
        !section_end(attributes_offset, build->attribute_count, ES_ATTRIBUTE_SIZE, &columns_offset) ||
        !section_end(columns_offset, build->column_count, ES_COLUMN_SIZE, &strings_offset)) {
        *failure = ES_BUILD_ALLOCATION;
        return NULL;
    }
    total_size = strings_offset;
    if (!add_size(&total_size, build->strings_length)) {
        *failure = ES_BUILD_ALLOCATION;
        return NULL;
    }
    output = (uint8_t *)malloc(total_size);
    if (output == NULL) {
        *failure = ES_BUILD_ALLOCATION;
        return NULL;
    }
    memset(output, 0, ES_HEADER_SIZE);

    memcpy(output, magic, sizeof(magic));
    put_u32(output, ES_HEADER_TOTAL_SIZE, (uint32_t)total_size);
    put_u32(output, ES_HEADER_NODE_COUNT, (uint32_t)build->node_count);
    put_u32(output, ES_HEADER_EDGE_COUNT, (uint32_t)build->edge_count);
    put_u32(output, ES_HEADER_ATTRIBUTE_COUNT, (uint32_t)build->attribute_count);
    put_u32(output, ES_HEADER_COLUMN_COUNT, (uint32_t)build->column_count);
    put_u32(output, ES_HEADER_NODES_OFFSET, (uint32_t)nodes_offset);
    put_u32(output, ES_HEADER_EDGES_OFFSET, (uint32_t)edges_offset);
    put_u32(output, ES_HEADER_ATTRIBUTES_OFFSET, (uint32_t)attributes_offset);
    put_u32(output, ES_HEADER_COLUMNS_OFFSET, (uint32_t)columns_offset);
    put_u32(output, ES_HEADER_STRINGS_OFFSET, (uint32_t)strings_offset);
    put_u32(output, ES_HEADER_STRINGS_LENGTH, (uint32_t)build->strings_length);

    string_cursor = strings_offset;
    for (index = 0; index < build->node_count; ++index) {
        const es_source_node *source = &build->nodes[index];
        markdown_core_scope scope = source->node       ? markdown_core_node_scope(source->node)
                                    : source->citation ? markdown_core_citation_scope(source->citation)
                                    : source->footnote ? markdown_core_footnote_scope(source->footnote)
                                    : source->specimen ? markdown_core_specimen_scope(source->specimen)
                                    : source->metadata ? markdown_core_metadata_scope(source->metadata)
                                                       : markdown_core_metadata_record_scope(source->metadata_record);
        size_t node_offset = nodes_offset + index * ES_NODE_SIZE;
        size_t string_index;
        put_u32(output, node_offset + ES_NODE_KIND, source->wire_kind);
        put_u32(output, node_offset + ES_NODE_FLAGS, source->flags);
        put_i32(output, node_offset + ES_NODE_SCOPE, scope.start.line);
        put_i32(output, node_offset + ES_NODE_SCOPE + 4, scope.start.column);
        put_i32(output, node_offset + ES_NODE_SCOPE + 8, scope.end.line);
        put_i32(output, node_offset + ES_NODE_SCOPE + 12, scope.end.column);
        put_u32(output, node_offset + ES_NODE_CHILD_START, source->child_start);
        put_u32(output, node_offset + ES_NODE_CHILD_COUNT, source->child_count);
        put_u32(output, node_offset + ES_NODE_LABEL_INDEX, source->label_index);
        put_u32(output, node_offset + ES_NODE_AUX_START, source->aux_start);
        put_u32(output, node_offset + ES_NODE_AUX_COUNT, source->aux_count);
        put_i32(output, node_offset + ES_NODE_SCALAR0, source->scalar0);
        put_i64(output, node_offset + ES_NODE_INTEGER2, source->integer2);
        put_i64(output, node_offset + ES_NODE_I64, source->integer);
        write_string_reference(output, node_offset + ES_NODE_ANCHOR, source->anchor, &string_cursor);
        put_u32(output, node_offset + ES_NODE_CLASSES_START, source->class_start);
        put_u32(output, node_offset + ES_NODE_CLASSES_COUNT, source->class_count);
        put_u32(output, node_offset + ES_NODE_RECORDS_START, source->record_start);
        put_u32(output, node_offset + ES_NODE_RECORDS_COUNT, source->record_count);
        put_u32(output, node_offset + ES_NODE_METADATA, source->metadata_index);
        put_u32(output, node_offset + ES_NODE_WIDTH, source->width);
        put_u32(output, node_offset + ES_NODE_HEIGHT, source->height);
        if (source->resource_first != ES_NO_INDEX && source->resource_first != index) {
            /* A later occurrence of a resource: its destination and title
             * were written with the first occurrence, so the record points
             * at those bytes rather than carrying them again. */
            size_t first_offset = nodes_offset + (size_t)source->resource_first * ES_NODE_SIZE;
            memcpy(output + node_offset + ES_NODE_STRINGS, output + first_offset + ES_NODE_STRINGS, 3 * 8);
            write_string_reference(output, node_offset + ES_NODE_STRINGS + 3 * 8, source->strings[3], &string_cursor);
            continue;
        }
        for (string_index = 0; string_index < 4; ++string_index) {
            write_string_reference(output, node_offset + ES_NODE_STRINGS + string_index * 8,
                                   source->strings[string_index], &string_cursor);
        }
    }
    for (index = 0; index < build->edge_count; ++index) {
        put_u32(output, edges_offset + index * sizeof(uint32_t), build->edges[index]);
    }
    for (index = 0; index < build->attribute_count; ++index) {
        size_t attribute_offset = attributes_offset + index * ES_ATTRIBUTE_SIZE;
        write_string_reference(output, attribute_offset, required_string(build->attributes[index].name),
                               &string_cursor);
        write_string_reference(output, attribute_offset + 8, required_string(build->attributes[index].value),
                               &string_cursor);
    }
    for (index = 0; index < build->column_count; ++index) {
        size_t offset = columns_offset + index * ES_COLUMN_SIZE;
        markdown_core_table_column column = build->columns[index];
        int64_t bits = 0;
        if (column.relative.has_value) {
            memcpy(&bits, &column.relative.value, sizeof(bits));
        }
        put_u32(output, offset, (uint32_t)column.alignment);
        put_u32(output, offset + 4, column.relative.has_value ? 1 : 0);
        put_i64(output, offset + 8, bits);
    }
    if (string_cursor != total_size) {
        free(output);
        *failure = ES_BUILD_INTERNAL;
        return NULL;
    }
    return output;
}

uint8_t *es_parse(const uint8_t *source, size_t length) {
    static const uint8_t internal_message_bytes[] = "could not produce AST result";
    markdown_core_string internal_message = {internal_message_bytes, sizeof(internal_message_bytes) - 1};
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    const markdown_core_node *root;
    es_build build = {0};
    uint8_t *output = NULL;

    document = markdown_core_document_parse(source, length, &error);
    if (document == NULL) {
        markdown_core_error_code code =
            error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error);
        markdown_core_string message = error == NULL ? internal_message : markdown_core_error_get_message(error);
        output = error_result(code, message);
        markdown_core_error_free(error);
        return output;
    }
    root = markdown_core_document_root(document);
    if (root == NULL) {
        output = error_result(MARKDOWN_CORE_ERROR_INTERNAL, internal_message);
    } else {
        es_build_failure write_failure = ES_BUILD_OK;
        collect_topology(&build, root);
        collect_fields(&build);
        if (build.failure == ES_BUILD_OK) {
            output = success_result(&build, &write_failure);
            if (output == NULL && write_failure == ES_BUILD_INTERNAL) {
                output = error_result(MARKDOWN_CORE_ERROR_INTERNAL, internal_message);
            }
        } else if (build.failure == ES_BUILD_INTERNAL) {
            output = error_result(MARKDOWN_CORE_ERROR_INTERNAL, internal_message);
        }
    }

    free_build(&build);
    markdown_core_document_free(document);
    markdown_core_error_free(error);
    return output;
}

void es_result_free(uint8_t *result) { free(result); }
