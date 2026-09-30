#include "markdown_core_wire.h"

#include "markdown_core.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * MCB3 encoder. See docs/architecture/wire-format.md.
 *
 * Records are written in post-order: a node's node-valued fields, field after
 * field in the contract's order, then the node's own record carrying their
 * counts. The walk is an explicit stack, so neither the encoder nor a reader
 * recurses, and no subtree's size is needed before it is written.
 *
 * Every accessor is called on a node of the kind the record's switch named,
 * or with an index below the count it just read, so each answers
 * MARKDOWN_CORE_OK: the encoder reads its out-parameters and not its status.
 */

/* `failed` says an allocation failed, or the message outgrew a u32 length. */
typedef struct wire_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} wire_buffer;

/* The byte offset of the message length. */
enum { WIRE_HEADER_LENGTH = 4 };
enum { WIRE_STATUS_DOCUMENT = 0, WIRE_STATUS_ERROR = 1 };

static const uint8_t wire_magic[] = {'M', 'C', 'B', '3'};

static void reserve(wire_buffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    uint8_t *data;
    if (buffer->failed) {
        return;
    }
    /* The message length is a u32, so no message may outgrow one. */
    if (additional > UINT32_MAX - buffer->size) {
        buffer->failed = true;
        return;
    }
    required = buffer->size + additional;
    if (required <= buffer->capacity) {
        return;
    }
    capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    while (capacity < required) {
        capacity = capacity > SIZE_MAX / 2 ? required : capacity * 2;
    }
    data = (uint8_t *)realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = true;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void put_bytes(wire_buffer *buffer, const uint8_t *bytes, size_t length) {
    reserve(buffer, length);
    if (!buffer->failed && length != 0) {
        memcpy(buffer->data + buffer->size, bytes, length);
        buffer->size += length;
    }
}

static void put_u8(wire_buffer *buffer, uint8_t value) { put_bytes(buffer, &value, 1); }

static void put_u32(wire_buffer *buffer, uint32_t value) {
    uint8_t bytes[4];
    size_t index;
    for (index = 0; index < 4; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
    put_bytes(buffer, bytes, sizeof(bytes));
}

static void put_u64(wire_buffer *buffer, uint64_t value) {
    uint8_t bytes[8];
    size_t index;
    for (index = 0; index < 8; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
    put_bytes(buffer, bytes, sizeof(bytes));
}

static void put_i32(wire_buffer *buffer, int32_t value) { put_u32(buffer, (uint32_t)value); }

/* Int. */
static void put_int(wire_buffer *buffer, int64_t value) { put_u64(buffer, (uint64_t)value); }

/* Double. */
static void put_double(wire_buffer *buffer, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u64(buffer, bits);
}

static void put_bool(wire_buffer *buffer, bool value) { put_u8(buffer, value ? 1 : 0); }

/* A u32 count, the encoding of `[T]`'s length and of a node-valued field. */
static void put_count(wire_buffer *buffer, size_t count) {
    if (count > UINT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_u32(buffer, (uint32_t)count);
}

static void put_string(wire_buffer *buffer, markdown_core_string value) {
    put_count(buffer, value.length);
    put_bytes(buffer, value.data, value.length);
}

static void put_optional_string(wire_buffer *buffer, markdown_core_optional_string value) {
    put_bool(buffer, value.has_value);
    if (value.has_value) {
        put_string(buffer, value.value);
    }
}

static void put_optional_int(wire_buffer *buffer, markdown_core_optional_i64 value) {
    put_bool(buffer, value.has_value);
    if (value.has_value) {
        put_int(buffer, value.value);
    }
}

/* Extent { lead: Int32, span: UInt32 }. */
static void put_extent(wire_buffer *buffer, markdown_core_extent extent) {
    put_i32(buffer, extent.lead);
    put_u32(buffer, extent.span);
}

/* An enum or branch index: the facade numbers from `first`, the wire from 0. */
static void put_index(wire_buffer *buffer, int value, int first) { put_u8(buffer, (uint8_t)(value - first)); }

/* anchor: String?, then Attributes { classes: [String], records: [Record] }. */
static void put_attributes(wire_buffer *buffer, const markdown_core_attribute_value *attributes) {
    size_t classes = markdown_core_attribute_value_class_count(attributes);
    size_t records = markdown_core_attribute_value_record_count(attributes);
    size_t index;
    put_optional_string(buffer, markdown_core_attribute_value_anchor(attributes));
    put_count(buffer, classes);
    for (index = 0; index < classes && !buffer->failed; ++index) {
        markdown_core_string value;
        markdown_core_attribute_value_class_at(attributes, index, &value);
        put_string(buffer, value);
    }
    put_count(buffer, records);
    for (index = 0; index < records && !buffer->failed; ++index) {
        markdown_core_string name, value;
        markdown_core_attribute_value_record_at(attributes, index, &name, &value);
        put_string(buffer, name);
        put_string(buffer, value);
    }
}

/* Destination: url { value } | cross { path, anchor? }. */
static void put_destination(wire_buffer *buffer, const markdown_core_node *node) {
    markdown_core_destination destination;
    markdown_core_node_destination(node, &destination);
    put_index(buffer, destination.kind, MARKDOWN_CORE_DESTINATION_URL);
    if (destination.kind == MARKDOWN_CORE_DESTINATION_URL) {
        put_string(buffer, destination.url);
    } else {
        put_string(buffer, destination.path);
        put_optional_string(buffer, destination.anchor);
    }
}

/* Dimensions?: { width: Int, height: Int? }. */
static void put_dimensions(wire_buffer *buffer, const markdown_core_node *node) {
    const markdown_core_dimensions *dimensions;
    markdown_core_node_dimensions(node, &dimensions);
    put_bool(buffer, dimensions != NULL);
    if (dimensions != NULL) {
        put_int(buffer, dimensions->width);
        put_optional_int(buffer, dimensions->height);
    }
}

/* MetadataValue?: scalar { MetadataScalar } | list { [MetadataListItem] }. */
static void put_metadata_value(wire_buffer *buffer, const markdown_core_metadata_value *value) {
    markdown_core_metadata_value_kind kind;
    put_bool(buffer, value != NULL);
    if (value == NULL) {
        return;
    }
    kind = markdown_core_metadata_value_get_kind(value);
    put_index(buffer, kind, MARKDOWN_CORE_METADATA_SCALAR);
    if (kind == MARKDOWN_CORE_METADATA_SCALAR) {
        /* MetadataScalar: null | bool { Bool } | number { String } | text { String }. */
        markdown_core_metadata_scalar scalar;
        markdown_core_metadata_value_scalar(value, &scalar);
        put_index(buffer, scalar.kind, MARKDOWN_CORE_METADATA_NULL);
        if (scalar.kind == MARKDOWN_CORE_METADATA_BOOL) {
            put_bool(buffer, scalar.value.boolean);
        } else if (scalar.kind != MARKDOWN_CORE_METADATA_NULL) {
            put_string(buffer, scalar.value.string);
        }
    } else {
        /* MetadataListItem: number { String } | text { String }. */
        size_t count;
        size_t index;
        markdown_core_metadata_value_item_count(value, &count);
        put_count(buffer, count);
        for (index = 0; index < count && !buffer->failed; ++index) {
            markdown_core_metadata_list_item item;
            markdown_core_metadata_value_item_at(value, index, &item);
            put_index(buffer, item.kind, MARKDOWN_CORE_METADATA_ITEM_NUMBER);
            put_string(buffer, item.value);
        }
    }
}

typedef markdown_core_status (*metadata_field)(const markdown_core_node *, const markdown_core_metadata_value **);

/* The ten fields in the contract's order. */
static const metadata_field metadata_fields[] = {
    markdown_core_metadata_name,     markdown_core_metadata_title,    markdown_core_metadata_subtitle,
    markdown_core_metadata_time,     markdown_core_metadata_date,     markdown_core_metadata_authors,
    markdown_core_metadata_keywords, markdown_core_metadata_abstract, markdown_core_metadata_state,
    markdown_core_metadata_comment,
};

static void put_metadata(wire_buffer *buffer, const markdown_core_node *metadata) {
    size_t index;
    for (index = 0; index < sizeof(metadata_fields) / sizeof(*metadata_fields); ++index) {
        const markdown_core_metadata_value *value;
        metadata_fields[index](metadata, &value);
        put_metadata_value(buffer, value);
    }
}

/* ---- Shared resources ---------------------------------------------------- */

/* Numbers each distinct resource in the order the message first names it. An
 * open-addressing table keyed by the resource's identity. */
typedef struct wire_resource_slot {
    const markdown_core_resource *resource;
    uint32_t ordinal;
} wire_resource_slot;

typedef struct wire_resources {
    wire_resource_slot *slots;
    size_t count;
    size_t capacity;
} wire_resources;

static size_t hash_resource(const markdown_core_resource *resource) {
    uint64_t bits = (uint64_t)(uintptr_t)resource;
    bits ^= bits >> 33;
    bits *= UINT64_C(0xff51afd7ed558ccd);
    bits ^= bits >> 33;
    return (size_t)bits;
}

static wire_resource_slot *find_resource_slot(wire_resource_slot *slots, size_t capacity,
                                              const markdown_core_resource *resource) {
    size_t position = hash_resource(resource) & (capacity - 1);
    while (slots[position].resource != NULL && slots[position].resource != resource) {
        position = (position + 1) & (capacity - 1);
    }
    return &slots[position];
}

static bool grow_resources(wire_resources *resources) {
    size_t capacity = resources->capacity == 0 ? 64 : resources->capacity * 2;
    wire_resource_slot *slots;
    size_t index;
    if (capacity < resources->capacity || capacity > SIZE_MAX / sizeof(*slots)) {
        return false;
    }
    slots = (wire_resource_slot *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    for (index = 0; index < resources->capacity; ++index) {
        if (resources->slots[index].resource != NULL) {
            *find_resource_slot(slots, capacity, resources->slots[index].resource) = resources->slots[index];
        }
    }
    free(resources->slots);
    resources->slots = slots;
    resources->capacity = capacity;
    return true;
}

/* The `dest` field of a Link or Embedded, which also stands for its `title`:
 * the resource ordinal, followed by the resource the first time it is named. */
static void put_resource(wire_buffer *buffer, wire_resources *resources, const markdown_core_node *node) {
    const markdown_core_resource *resource;
    const markdown_core_attribute_value *inherited;
    markdown_core_optional_string title;
    wire_resource_slot *slot;
    markdown_core_node_resource(node, &resource);
    if (resources->count + 1 > resources->capacity / 2 && !grow_resources(resources)) {
        buffer->failed = true;
        return;
    }
    slot = find_resource_slot(resources->slots, resources->capacity, resource);
    if (slot->resource != NULL) {
        put_u32(buffer, slot->ordinal);
        return;
    }
    if (resources->count >= UINT32_MAX) {
        buffer->failed = true;
        return;
    }
    slot->resource = resource;
    slot->ordinal = (uint32_t)resources->count++;
    put_u32(buffer, slot->ordinal);
    put_destination(buffer, node);
    markdown_core_node_title(node, &title);
    put_optional_string(buffer, title);
    markdown_core_node_inherited_attributes(node, &inherited);
    put_attributes(buffer, inherited);
}

/* ---- Node-valued fields -------------------------------------------------- */

typedef enum wire_edge_shape {
    /* One node, or none. */
    WIRE_EDGE_NODE,
    /* A first node and its next siblings. */
    WIRE_EDGE_CHAIN,
    /* A definition's bodies, each a chain. */
    WIRE_EDGE_BODIES
} wire_edge_shape;

typedef struct wire_edge {
    wire_edge_shape shape;
    const markdown_core_node *node;
    const markdown_core_definition_body *body;
} wire_edge;

enum { WIRE_MAX_EDGES = 4 };

static wire_edge node_edge(wire_edge_shape shape, const markdown_core_node *node) {
    wire_edge edge = {shape, node, NULL};
    return edge;
}

/* A node-valued field read by `accessor`. */
typedef markdown_core_status (*node_field)(const markdown_core_node *, const markdown_core_node **);

static wire_edge field_edge(wire_edge_shape shape, const markdown_core_node *node, node_field accessor) {
    const markdown_core_node *field;
    accessor(node, &field);
    return node_edge(shape, field);
}

/* The node-valued fields of `node`, in the contract's field order. This is the
 * one place that knows where each field's nodes live; the walk schedules them
 * and the record counts them from the same answer. */
static size_t node_edges(const markdown_core_node *node, markdown_core_node_kind kind, wire_edge *edges) {
    const markdown_core_node *children = markdown_core_node_get_first_child(node);
    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        edges[0] = node_edge(WIRE_EDGE_CHAIN, children);
        edges[1] = field_edge(WIRE_EDGE_NODE, node, markdown_core_node_document_metadata);
        return 2;
    case MARKDOWN_CORE_KIND_CALLOUT:
        edges[0] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_node_callout_title);
        edges[1] = node_edge(WIRE_EDGE_CHAIN, children);
        return 2;
    case MARKDOWN_CORE_KIND_TABLE:
        /* head, content and foot are one chain of rows the record partitions. */
        edges[0] = field_edge(WIRE_EDGE_NODE, node, markdown_core_node_table_caption);
        edges[1] = node_edge(WIRE_EDGE_CHAIN, children);
        return 2;
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        edges[0] = field_edge(WIRE_EDGE_NODE, node, markdown_core_node_directive_label);
        edges[1] = node_edge(WIRE_EDGE_CHAIN, children);
        return 2;
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        edges[0] = field_edge(WIRE_EDGE_NODE, node, markdown_core_node_directive_label);
        return 1;
    case MARKDOWN_CORE_KIND_CITE:
        edges[0] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_node_cite_citations);
        return 1;
    case MARKDOWN_CORE_KIND_DEFINITION:
        edges[0] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_node_definition_term);
        edges[1].shape = WIRE_EDGE_BODIES;
        edges[1].node = NULL;
        markdown_core_node_definition_bodies(node, &edges[1].body);
        return 2;
    case MARKDOWN_CORE_KIND_CITATION: {
        /* The referent's inline note, when it owns one, then the affixes. */
        markdown_core_referent referent;
        markdown_core_citation_referent(node, &referent);
        edges[0] = node_edge(WIRE_EDGE_NODE, referent.note);
        edges[1] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_citation_prefix);
        edges[2] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_citation_suffix);
        return 3;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        edges[0] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_footnote_content);
        return 1;
    case MARKDOWN_CORE_KIND_SPECIMEN:
        edges[0] = field_edge(WIRE_EDGE_CHAIN, node, markdown_core_specimen_content);
        return 1;
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_FORMULA:
    case MARKDOWN_CORE_KIND_COMMENT:
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
    case MARKDOWN_CORE_KIND_METADATA:
        return 0;
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_HEADING:
    case MARKDOWN_CORE_KIND_LIST:
    case MARKDOWN_CORE_KIND_LIST_ITEM:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED:
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
        /* The one node-valued field is the children. */
        edges[0] = node_edge(WIRE_EDGE_CHAIN, children);
        return 1;
    case MARKDOWN_CORE_KIND_NONE:
        break;
    }
    return 0;
}

static size_t chain_length(const markdown_core_node *node) {
    size_t count = 0;
    for (; node != NULL; node = markdown_core_node_get_next_sibling(node)) {
        count++;
    }
    return count;
}

/* The count of a `[K]` or `[Markup]` field. */
static void put_chain_count(wire_buffer *buffer, const wire_edge *edge) { put_count(buffer, chain_length(edge->node)); }

/* The presence of a `K?` field. */
static void put_node_presence(wire_buffer *buffer, const wire_edge *edge) { put_bool(buffer, edge->node != NULL); }

/* The counts of a `[[Markup]]` field. */
static void put_bodies_counts(wire_buffer *buffer, const wire_edge *edge) {
    const markdown_core_definition_body *body;
    size_t count = 0;
    for (body = edge->body; body != NULL; body = markdown_core_definition_body_next(body)) {
        count++;
    }
    put_count(buffer, count);
    for (body = edge->body; body != NULL && !buffer->failed; body = markdown_core_definition_body_next(body)) {
        put_count(buffer, chain_length(markdown_core_definition_body_content(body)));
    }
}

/* ---- Records ------------------------------------------------------------- */

static void put_list_fields(wire_buffer *buffer, const markdown_core_node *node) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    bool tight;
    markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight);
    put_index(buffer, flavor, MARKDOWN_CORE_LIST_FLAVOR_BULLET);
    put_optional_int(buffer, start);
    /* The ordered-marker facts exist exactly when the list has a start. */
    put_bool(buffer, start.has_value);
    if (start.has_value) {
        put_index(buffer, variant.kind, MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL);
        if (variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ALPHA ||
            variant.kind == MARKDOWN_CORE_ORDERED_LIST_VARIANT_ROMAN) {
            put_bool(buffer, variant.lowercased);
        }
    }
    put_bool(buffer, start.has_value);
    if (start.has_value) {
        put_index(buffer, delimiter.kind, MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD);
        if (delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS) {
            put_bool(buffer, delimiter.closed);
        }
    }
    put_bool(buffer, tight);
}

static void put_table_fields(wire_buffer *buffer, const markdown_core_node *node, const wire_edge *edges) {
    size_t columns, head, content, foot, index;
    markdown_core_node_table_properties(node, &columns, &head, &content, &foot);
    put_node_presence(buffer, &edges[0]);
    put_count(buffer, columns);
    for (index = 0; index < columns && !buffer->failed; ++index) {
        /* TableColumn { flow: Flow, relative: Double? }. */
        markdown_core_table_column column;
        markdown_core_node_table_column_at(node, index, &column);
        put_index(buffer, column.flow, MARKDOWN_CORE_FLOW_NONE);
        put_bool(buffer, column.relative.has_value);
        if (column.relative.has_value) {
            put_double(buffer, column.relative.value);
        }
    }
    put_count(buffer, head);
    put_count(buffer, content);
    put_count(buffer, foot);
}

/* The FootnoteTarget branches, in the contract's order. */
enum { WIRE_FOOTNOTE_LABEL = 0, WIRE_FOOTNOTE_NOTE = 1 };

static void put_citation_fields(wire_buffer *buffer, const markdown_core_node *node, const wire_edge *edges) {
    /* CitationReferent: bib { key, mode: BibMode } | footnote { target: FootnoteTarget } |
     * specimen { label }, where FootnoteTarget is label { value } | note { footnote: Footnote }. */
    markdown_core_referent referent;
    markdown_core_citation_referent(node, &referent);
    put_index(buffer, referent.kind, MARKDOWN_CORE_REFERENT_BIB);
    if (referent.kind == MARKDOWN_CORE_REFERENT_BIB) {
        put_string(buffer, referent.key);
        put_index(buffer, referent.mode, MARKDOWN_CORE_BIB_MODE_NORMAL);
    } else if (referent.kind == MARKDOWN_CORE_REFERENT_FOOTNOTE && referent.note != NULL) {
        /* The note is a node-valued field: its record is on the stack. */
        put_u8(buffer, WIRE_FOOTNOTE_NOTE);
    } else if (referent.kind == MARKDOWN_CORE_REFERENT_FOOTNOTE) {
        put_u8(buffer, WIRE_FOOTNOTE_LABEL);
        put_string(buffer, referent.label);
    } else {
        put_string(buffer, referent.label);
    }
    put_chain_count(buffer, &edges[1]);
    put_chain_count(buffer, &edges[2]);
}

/* A node's record: its kind, its inherited fields, then its own fields in the
 * contract's order, node-valued fields written as counts. */
static void put_record(wire_buffer *buffer, wire_resources *resources, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    wire_edge edges[WIRE_MAX_EDGES];
    markdown_core_string literal;
    markdown_core_optional_string first, second;
    bool flag;

    node_edges(node, kind, edges);
    put_u8(buffer, (uint8_t)kind);
    put_u64(buffer, markdown_core_node_id(node));
    put_extent(buffer, markdown_core_node_extent(node));
    put_attributes(buffer, markdown_core_node_primary_attributes(node));

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        put_chain_count(buffer, &edges[0]);
        put_node_presence(buffer, &edges[1]);
        break;
    case MARKDOWN_CORE_KIND_CALLOUT: {
        markdown_core_optional_bool collapsed;
        markdown_core_node_callout_properties(node, &first, &collapsed);
        put_optional_string(buffer, first);
        put_bool(buffer, collapsed.has_value);
        if (collapsed.has_value) {
            put_bool(buffer, collapsed.value);
        }
        put_node_presence(buffer, &edges[0]);
        if (edges[0].node != NULL) {
            put_chain_count(buffer, &edges[0]);
        }
        put_chain_count(buffer, &edges[1]);
        break;
    }
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level;
        markdown_core_node_heading_level(node, &level);
        put_int(buffer, level);
        put_chain_count(buffer, &edges[0]);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST:
        put_list_fields(buffer, node);
        put_chain_count(buffer, &edges[0]);
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_marker(node, &first);
        put_optional_string(buffer, first);
        put_chain_count(buffer, &edges[0]);
        break;
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced, closed;
        markdown_core_node_code_block_properties(node, &first, &second, &literal, &fenced, &closed);
        put_optional_string(buffer, first);
        put_optional_string(buffer, second);
        put_string(buffer, literal);
        put_bool(buffer, fenced);
        put_bool(buffer, closed);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        markdown_core_node_literal(node, &literal);
        put_string(buffer, literal);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement mode;
        markdown_core_node_formula_properties(node, &mode, &literal);
        /* A FormulaBlock's placement is implied by its kind. */
        if (kind == MARKDOWN_CORE_KIND_FORMULA) {
            put_index(buffer, mode, MARKDOWN_CORE_PLACEMENT_EMBEDDED);
        }
        put_string(buffer, literal);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE:
        put_table_fields(buffer, node, edges);
        break;
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        int64_t rowspan, colspan;
        markdown_core_node_table_cell_spans(node, &rowspan, &colspan);
        put_int(buffer, rowspan);
        put_int(buffer, colspan);
        put_chain_count(buffer, &edges[0]);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        markdown_core_node_directive_properties(node, &first);
        if (kind == MARKDOWN_CORE_KIND_DIRECTIVE) {
            /* An inline directive's name is required and it has no content. */
            put_string(buffer, first.value);
            put_node_presence(buffer, &edges[0]);
        } else {
            put_optional_string(buffer, first);
            put_node_presence(buffer, &edges[0]);
            put_chain_count(buffer, &edges[1]);
        }
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED:
        put_destination(buffer, node);
        markdown_core_node_cross_label(node, &first);
        put_optional_string(buffer, first);
        if (kind == MARKDOWN_CORE_KIND_CROSS_EMBEDDED) {
            put_dimensions(buffer, node);
        }
        break;
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED:
        put_resource(buffer, resources, node);
        if (kind == MARKDOWN_CORE_KIND_EMBEDDED) {
            put_dimensions(buffer, node);
        }
        put_chain_count(buffer, &edges[0]);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION:
        put_chain_count(buffer, &edges[0]);
        put_bodies_counts(buffer, &edges[1]);
        markdown_core_node_definition_compact(node, &flag);
        put_bool(buffer, flag);
        break;
    case MARKDOWN_CORE_KIND_CITATION:
        put_citation_fields(buffer, node, edges);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        markdown_core_footnote_label(node, &first);
        put_optional_string(buffer, first);
        put_chain_count(buffer, &edges[0]);
        break;
    case MARKDOWN_CORE_KIND_SPECIMEN: {
        markdown_core_optional_i64 start;
        markdown_core_specimen_properties(node, &first, &start);
        put_optional_string(buffer, first);
        put_optional_int(buffer, start);
        put_chain_count(buffer, &edges[0]);
        break;
    }
    case MARKDOWN_CORE_KIND_METADATA:
        put_metadata(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_NONE:
        break;
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
    case MARKDOWN_CORE_KIND_CITE:
        put_chain_count(buffer, &edges[0]);
        break;
    }
}

/* ---- The walk ------------------------------------------------------------ */

typedef enum wire_step {
    /* Schedule a node's fields, then its record. */
    WIRE_VISIT,
    /* Write a node's record; its fields are written. */
    WIRE_RECORD,
    /* Visit a node, then its next siblings. */
    WIRE_CHAIN,
    /* Visit a body's chain, then the next bodies. */
    WIRE_BODIES
} wire_step;

typedef struct wire_action {
    wire_step step;
    const markdown_core_node *node;
    const markdown_core_definition_body *body;
} wire_action;

typedef struct wire_stack {
    wire_action *actions;
    size_t count;
    size_t capacity;
} wire_stack;

static void push(wire_buffer *buffer, wire_stack *stack, wire_step step, const markdown_core_node *node,
                 const markdown_core_definition_body *body) {
    if (buffer->failed) {
        return;
    }
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity == 0 ? 64 : stack->capacity * 2;
        wire_action *actions;
        if (capacity < stack->capacity || capacity > SIZE_MAX / sizeof(*actions)) {
            buffer->failed = true;
            return;
        }
        actions = (wire_action *)realloc(stack->actions, capacity * sizeof(*actions));
        if (actions == NULL) {
            buffer->failed = true;
            return;
        }
        stack->actions = actions;
        stack->capacity = capacity;
    }
    stack->actions[stack->count].step = step;
    stack->actions[stack->count].node = node;
    stack->actions[stack->count].body = body;
    stack->count++;
}

/* Pushes a field so that it runs before anything pushed earlier. */
static void push_edge(wire_buffer *buffer, wire_stack *stack, const wire_edge *edge) {
    switch (edge->shape) {
    case WIRE_EDGE_NODE:
        if (edge->node != NULL) {
            push(buffer, stack, WIRE_VISIT, edge->node, NULL);
        }
        break;
    case WIRE_EDGE_CHAIN:
        if (edge->node != NULL) {
            push(buffer, stack, WIRE_CHAIN, edge->node, NULL);
        }
        break;
    case WIRE_EDGE_BODIES:
        if (edge->body != NULL) {
            push(buffer, stack, WIRE_BODIES, NULL, edge->body);
        }
        break;
    }
}

static void put_tree(wire_buffer *buffer, const markdown_core_node *root) {
    wire_stack stack = {0};
    wire_resources resources = {0};
    push(buffer, &stack, WIRE_VISIT, root, NULL);
    while (stack.count != 0 && !buffer->failed) {
        wire_action action = stack.actions[--stack.count];
        switch (action.step) {
        case WIRE_VISIT: {
            wire_edge edges[WIRE_MAX_EDGES];
            size_t count = node_edges(action.node, markdown_core_node_get_kind(action.node), edges);
            push(buffer, &stack, WIRE_RECORD, action.node, NULL);
            while (count != 0) {
                push_edge(buffer, &stack, &edges[--count]);
            }
            break;
        }
        case WIRE_RECORD:
            put_record(buffer, &resources, action.node);
            break;
        case WIRE_CHAIN: {
            const markdown_core_node *next = markdown_core_node_get_next_sibling(action.node);
            if (next != NULL) {
                push(buffer, &stack, WIRE_CHAIN, next, NULL);
            }
            push(buffer, &stack, WIRE_VISIT, action.node, NULL);
            break;
        }
        case WIRE_BODIES: {
            const markdown_core_definition_body *next = markdown_core_definition_body_next(action.body);
            const markdown_core_node *content = markdown_core_definition_body_content(action.body);
            if (next != NULL) {
                push(buffer, &stack, WIRE_BODIES, NULL, next);
            }
            if (content != NULL) {
                push(buffer, &stack, WIRE_CHAIN, content, NULL);
            }
            break;
        }
        }
    }
    free(stack.actions);
    free(resources.slots);
}

/* The document's footnote and specimen tables: each a count, then the id of
 * every definition in source order. */
typedef markdown_core_status (*definition_at)(const markdown_core_document *, size_t, const markdown_core_node **);

static void put_definition_table(wire_buffer *buffer, const markdown_core_document *document, size_t count,
                                 definition_at at) {
    size_t index;
    put_count(buffer, count);
    for (index = 0; index < count && !buffer->failed; ++index) {
        const markdown_core_node *definition;
        at(document, index, &definition);
        put_u64(buffer, markdown_core_node_id(definition));
    }
}

static void put_definitions(wire_buffer *buffer, const markdown_core_document *document) {
    put_definition_table(buffer, document, markdown_core_document_footnote_count(document),
                         markdown_core_document_footnote_at);
    put_definition_table(buffer, document, markdown_core_document_specimen_count(document),
                         markdown_core_document_specimen_at);
}

/* ---- Messages ------------------------------------------------------------ */

static void put_header(wire_buffer *buffer, uint8_t status) {
    put_bytes(buffer, wire_magic, sizeof(wire_magic));
    put_u32(buffer, 0);
    put_u8(buffer, status);
}

static void seal(wire_buffer *buffer) {
    size_t index;
    if (buffer->failed) {
        return;
    }
    for (index = 0; index < 4; ++index) {
        buffer->data[WIRE_HEADER_LENGTH + index] = (uint8_t)(buffer->size >> (index * 8));
    }
}

/* A failure message: the status and nothing else. */
static uint8_t *error_message(markdown_core_status status) {
    wire_buffer buffer = {0};
    put_header(&buffer, WIRE_STATUS_ERROR);
    put_u32(&buffer, (uint32_t)status);
    seal(&buffer);
    if (buffer.failed) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

uint8_t *markdown_core_wire_parse(const uint8_t *source, size_t length) {
    markdown_core_document *document;
    /* Extents are bytes whatever the unit; the unit only counts scope queries,
     * which the bindings answer themselves. */
    markdown_core_status status = markdown_core_document_parse(source, length, &document);
    wire_buffer buffer = {0};

    if (status != MARKDOWN_CORE_OK) {
        return error_message(status);
    }

    put_header(&buffer, WIRE_STATUS_DOCUMENT);
    put_tree(&buffer, markdown_core_document_root(document));
    put_definitions(&buffer, document);
    markdown_core_document_free(document);
    seal(&buffer);
    if (!buffer.failed) {
        return buffer.data;
    }

    free(buffer.data);
    return error_message(MARKDOWN_CORE_ALLOCATION_FAILED);
}

void markdown_core_wire_free(uint8_t *message) { free(message); }
