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

/* runs: [Run { source: Extent }]. */
static void put_runs(wire_buffer *buffer, const markdown_core_node *node) {
    size_t count;
    const markdown_core_run *runs = markdown_core_node_runs(node, &count);
    put_count(buffer, count);
    for (size_t i = 0; i < count; i++) {
        put_extent(buffer, runs[i].source);
    }
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

/* Destination: url { value } | cross { path, anchor? } | reference { label }. */
static void put_destination(wire_buffer *buffer, const markdown_core_node *node) {
    markdown_core_destination destination;
    markdown_core_node_destination(node, &destination);
    put_index(buffer, destination.kind, MARKDOWN_CORE_DESTINATION_URL);
    switch (destination.kind) {
    case MARKDOWN_CORE_DESTINATION_URL:
        put_string(buffer, destination.url);
        break;
    case MARKDOWN_CORE_DESTINATION_CROSS:
        put_string(buffer, destination.path);
        put_optional_string(buffer, destination.anchor);
        break;
    case MARKDOWN_CORE_DESTINATION_REFERENCE:
        put_string(buffer, destination.label);
        break;
    }
}

/* `dest`, then `title`: a Link's, an Embedded's or a Reference's. */
static void put_resource(wire_buffer *buffer, const markdown_core_node *node) {
    markdown_core_optional_string title;
    put_destination(buffer, node);
    markdown_core_node_title(node, &title);
    put_optional_string(buffer, title);
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

/* ---- Node-valued fields -------------------------------------------------- */

/* THE NODE-VALUED FIELDS of one node, as a cursor reads them: how many nodes
 * each field holds. */
typedef struct wire_fields {
    size_t count[MARKDOWN_CORE_FIELD_SUFFIX + 1];
} wire_fields;

/* Each field's place among a kind's fields in the contract's order. The
 * cursor reads them in canonical traversal order, which is the contract's
 * order for every kind but Document: it visits metadata before content and
 * declares it after. */
static const uint8_t field_rank[MARKDOWN_CORE_FIELD_SUFFIX + 1] = {
    [MARKDOWN_CORE_FIELD_TITLE] = 1,       [MARKDOWN_CORE_FIELD_CAPTION] = 1,   [MARKDOWN_CORE_FIELD_LABEL] = 1,
    [MARKDOWN_CORE_FIELD_TERM] = 1,        [MARKDOWN_CORE_FIELD_NOTE] = 1,      [MARKDOWN_CORE_FIELD_HEAD] = 2,
    [MARKDOWN_CORE_FIELD_PREFIX] = 2,      [MARKDOWN_CORE_FIELD_CONTENT] = 3,   [MARKDOWN_CORE_FIELD_CELLS] = 3,
    [MARKDOWN_CORE_FIELD_DEFINITIONS] = 3, [MARKDOWN_CORE_FIELD_CITATIONS] = 3, [MARKDOWN_CORE_FIELD_SUFFIX] = 3,
    [MARKDOWN_CORE_FIELD_FOOT] = 4,        [MARKDOWN_CORE_FIELD_METADATA] = 4,
};

/* Moves `cursor` to its node's first child; false, with `failed` set when
 * its path could not grow, when there is none. */
static bool cursor_enter(wire_buffer *buffer, markdown_core_cursor *cursor) {
    bool moved;
    if (markdown_core_cursor_child(cursor, &moved) != MARKDOWN_CORE_OK) {
        buffer->failed = true;
        return false;
    }
    return moved;
}

/* How many nodes each node-valued field of `node` holds. */
static void read_fields(wire_buffer *buffer, markdown_core_cursor *cursor, const markdown_core_node *node,
                        wire_fields *fields) {
    memset(fields, 0, sizeof(*fields));
    markdown_core_cursor_reset(cursor, node);
    if (cursor_enter(buffer, cursor)) {
        do {
            fields->count[markdown_core_cursor_field(cursor)]++;
        } while (markdown_core_cursor_next(cursor));
    }
}

/* The counts of a `[[Markup]]` field, a definition's bodies: how many, and
 * how many nodes each holds, an empty one included. */
static void put_bodies_counts(wire_buffer *buffer, markdown_core_cursor *cursor, const markdown_core_node *node) {
    size_t bodies, body = 0, count = 0;
    markdown_core_node_definition_bodies(node, &bodies);
    put_count(buffer, bodies);
    markdown_core_cursor_reset(cursor, node);
    if (cursor_enter(buffer, cursor)) {
        do {
            if (markdown_core_cursor_field(cursor) != MARKDOWN_CORE_FIELD_CONTENT) {
                continue;
            }
            for (; body < markdown_core_cursor_list(cursor); body++, count = 0) {
                put_count(buffer, count);
            }
            count++;
        } while (markdown_core_cursor_next(cursor));
    }
    for (; body < bodies; body++, count = 0) {
        put_count(buffer, count);
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

static void put_table_fields(wire_buffer *buffer, const markdown_core_node *node, const wire_fields *fields) {
    size_t columns, head, content, foot, index;
    markdown_core_node_table_properties(node, &columns, &head, &content, &foot);
    put_bool(buffer, fields->count[MARKDOWN_CORE_FIELD_CAPTION] != 0);
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

static void put_citation_fields(wire_buffer *buffer, const markdown_core_node *node, const wire_fields *fields) {
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
    put_count(buffer, fields->count[MARKDOWN_CORE_FIELD_PREFIX]);
    put_count(buffer, fields->count[MARKDOWN_CORE_FIELD_SUFFIX]);
}

/* A node's record: its kind, its inherited fields, then its own fields in the
 * contract's order, node-valued fields written as counts. */
static void put_record(wire_buffer *buffer, markdown_core_cursor *cursor, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    wire_fields fields;
    const size_t *count = fields.count;
    markdown_core_string literal;
    markdown_core_optional_string first, second;
    bool flag;

    read_fields(buffer, cursor, node, &fields);
    put_u8(buffer, (uint8_t)kind);
    put_u64(buffer, markdown_core_node_id(node));
    put_extent(buffer, markdown_core_node_extent(node));
    put_runs(buffer, node);
    put_attributes(buffer, markdown_core_node_attributes(node));

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        put_bool(buffer, count[MARKDOWN_CORE_FIELD_METADATA] != 0);
        break;
    case MARKDOWN_CORE_KIND_CALLOUT: {
        markdown_core_optional_bool collapsed;
        markdown_core_node_callout_properties(node, &first, &collapsed);
        put_optional_string(buffer, first);
        put_bool(buffer, collapsed.has_value);
        if (collapsed.has_value) {
            put_bool(buffer, collapsed.value);
        }
        put_bool(buffer, count[MARKDOWN_CORE_FIELD_TITLE] != 0);
        if (count[MARKDOWN_CORE_FIELD_TITLE] != 0) {
            put_count(buffer, count[MARKDOWN_CORE_FIELD_TITLE]);
        }
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    }
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level;
        markdown_core_node_heading_level(node, &level);
        put_int(buffer, level);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST:
        put_list_fields(buffer, node);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_marker(node, &first);
        put_optional_string(buffer, first);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
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
        put_table_fields(buffer, node, &fields);
        break;
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        int64_t rowspan, colspan;
        markdown_core_node_table_cell_spans(node, &rowspan, &colspan);
        put_int(buffer, rowspan);
        put_int(buffer, colspan);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        markdown_core_node_directive_properties(node, &first);
        if (kind == MARKDOWN_CORE_KIND_DIRECTIVE) {
            /* An inline directive's name is required and it has no content. */
            put_string(buffer, first.value);
            put_bool(buffer, count[MARKDOWN_CORE_FIELD_LABEL] != 0);
        } else {
            put_optional_string(buffer, first);
            put_bool(buffer, count[MARKDOWN_CORE_FIELD_LABEL] != 0);
            put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
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
        put_resource(buffer, node);
        if (kind == MARKDOWN_CORE_KIND_EMBEDDED) {
            put_dimensions(buffer, node);
        }
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE:
        markdown_core_reference_label(node, &literal);
        put_string(buffer, literal);
        put_resource(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_TERM]);
        put_bodies_counts(buffer, cursor, node);
        markdown_core_node_definition_compact(node, &flag);
        put_bool(buffer, flag);
        break;
    case MARKDOWN_CORE_KIND_CITATION:
        put_citation_fields(buffer, node, &fields);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        markdown_core_footnote_label(node, &first);
        put_optional_string(buffer, first);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    case MARKDOWN_CORE_KIND_SPECIMEN: {
        markdown_core_optional_i64 start;
        markdown_core_specimen_properties(node, &first, &start);
        put_optional_string(buffer, first);
        put_optional_int(buffer, start);
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
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
    case MARKDOWN_CORE_KIND_TABLE_ROW:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CELLS]);
        break;
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_DEFINITIONS]);
        break;
    case MARKDOWN_CORE_KIND_CITE:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CITATIONS]);
        break;
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
        put_count(buffer, count[MARKDOWN_CORE_FIELD_CONTENT]);
        break;
    }
}

/* ---- The walk ------------------------------------------------------------ */

typedef enum wire_step {
    /* Schedule a node's fields, then its record. */
    WIRE_VISIT,
    /* Write a node's record; its fields are written. */
    WIRE_RECORD
} wire_step;

typedef struct wire_action {
    wire_step step;
    /* The field's place in the contract's order (field_rank), while the
     * action waits to be ordered among its siblings. */
    uint8_t rank;
    const markdown_core_node *node;
} wire_action;

typedef struct wire_stack {
    wire_action *actions;
    size_t count;
    size_t capacity;
} wire_stack;

static void push(wire_buffer *buffer, wire_stack *stack, wire_step step, uint8_t rank, const markdown_core_node *node) {
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
    stack->actions[stack->count].rank = rank;
    stack->actions[stack->count].node = node;
    stack->count++;
}

/* Schedules the children of `node` so that they are written field after
 * field in the contract's order, each field's in stored order: read in
 * canonical traversal order, ordered by field, stably, and reversed onto the
 * stack. */
static void push_children(wire_buffer *buffer, wire_stack *stack, markdown_core_cursor *cursor,
                          const markdown_core_node *node) {
    size_t first = stack->count, index;
    markdown_core_cursor_reset(cursor, node);
    if (cursor_enter(buffer, cursor)) {
        do {
            push(buffer, stack, WIRE_VISIT, field_rank[markdown_core_cursor_field(cursor)],
                 markdown_core_cursor_node(cursor));
        } while (markdown_core_cursor_next(cursor));
    }
    if (buffer->failed) {
        return;
    }
    wire_action *actions = stack->actions + first;
    size_t count = stack->count - first;
    for (index = 1; index < count; index++) {
        wire_action held = actions[index];
        size_t at = index;
        for (; at > 0 && actions[at - 1].rank > held.rank; at--) {
            actions[at] = actions[at - 1];
        }
        actions[at] = held;
    }
    for (index = 0; index < count / 2; index++) {
        wire_action held = actions[index];
        actions[index] = actions[count - 1 - index];
        actions[count - 1 - index] = held;
    }
}

static void put_tree(wire_buffer *buffer, const markdown_core_node *root) {
    wire_stack stack = {0};
    markdown_core_cursor *cursor;
    if (markdown_core_cursor_open(root, &cursor) != MARKDOWN_CORE_OK) {
        buffer->failed = true;
        return;
    }
    push(buffer, &stack, WIRE_VISIT, 0, root);
    while (stack.count != 0 && !buffer->failed) {
        wire_action action = stack.actions[--stack.count];
        if (action.step == WIRE_VISIT) {
            push(buffer, &stack, WIRE_RECORD, 0, action.node);
            push_children(buffer, &stack, cursor, action.node);
        } else {
            put_record(buffer, cursor, action.node);
        }
    }
    markdown_core_cursor_free(cursor);
    free(stack.actions);
}

/* The document's footnote, specimen and Reference tables: each a count,
 * then the id of every definition in source order. */
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

/* The document's reference label table: a count, then each label that
 * resolves, in byte order, and the id of the Reference or Heading it
 * resolves to. */
static void put_reference_labels(wire_buffer *buffer, const markdown_core_document *document) {
    size_t count = markdown_core_document_reference_label_count(document), index;
    put_count(buffer, count);
    for (index = 0; index < count && !buffer->failed; ++index) {
        markdown_core_string label;
        const markdown_core_node *target;
        markdown_core_document_reference_label_at(document, index, &label, &target);
        put_string(buffer, label);
        put_u64(buffer, markdown_core_node_id(target));
    }
}

static void put_definitions(wire_buffer *buffer, const markdown_core_document *document) {
    put_definition_table(buffer, document, markdown_core_document_footnote_count(document),
                         markdown_core_document_footnote_at);
    put_definition_table(buffer, document, markdown_core_document_specimen_count(document),
                         markdown_core_document_specimen_at);
    put_definition_table(buffer, document, markdown_core_document_reference_count(document),
                         markdown_core_document_reference_at);
    put_reference_labels(buffer, document);
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

/* The document's message, or NULL when it cannot be made. */
static uint8_t *document_bytes(const markdown_core_document *document) {
    wire_buffer buffer = {0};
    put_header(&buffer, WIRE_STATUS_DOCUMENT);
    put_tree(&buffer, markdown_core_document_root(document));
    put_definitions(&buffer, document);
    seal(&buffer);
    if (!buffer.failed) {
        return buffer.data;
    }
    free(buffer.data);
    return NULL;
}

/* The document's message, or ALLOCATION_FAILED's when it cannot be made. */
static uint8_t *document_message(const markdown_core_document *document) {
    uint8_t *message = document_bytes(document);
    return message ? message : error_message(MARKDOWN_CORE_ALLOCATION_FAILED);
}

/* The message a session's step answers with: its document, or its failure. */
static uint8_t *step_message(markdown_core_status status, const markdown_core_document *document) {
    return status == MARKDOWN_CORE_OK ? document_message(document) : error_message(status);
}

uint8_t *markdown_core_wire_parse(const uint8_t *source, size_t length) {
    markdown_core_document *document;
    /* Extents are bytes whatever the unit; the unit only counts scope queries,
     * which the bindings answer themselves. */
    markdown_core_status status = markdown_core_document_parse(source, length, &document);
    if (status != MARKDOWN_CORE_OK) {
        return error_message(status);
    }
    uint8_t *message = document_message(document);
    markdown_core_document_free(document);
    return message;
}

uint8_t *markdown_core_wire_session_new(const uint8_t *source, size_t length, markdown_core_text_unit unit,
                                        markdown_core_session **session) {
    markdown_core_session *made = NULL;
    markdown_core_status status = markdown_core_session_new(source, length, unit, &made);
    *session = NULL;
    if (status != MARKDOWN_CORE_OK) {
        return error_message(status);
    }
    /* The session is the caller's only with its document's message. */
    uint8_t *message = document_bytes(markdown_core_session_document(made));
    if (!message) {
        markdown_core_session_free(made);
        return error_message(MARKDOWN_CORE_ALLOCATION_FAILED);
    }
    *session = made;
    return message;
}

uint8_t *markdown_core_wire_session_edit(markdown_core_session *session, const size_t *edits, size_t count,
                                         const uint8_t *texts) {
    markdown_core_text_edit *batch = count ? malloc(count * sizeof(*batch)) : NULL;
    if (count && !batch) {
        return error_message(MARKDOWN_CORE_ALLOCATION_FAILED);
    }
    for (size_t index = 0; index < count; ++index) {
        const size_t *edit = &edits[index * 3];
        batch[index] = (markdown_core_text_edit){edit[0], edit[1], texts, edit[2]};
        /* With no bytes at all, `texts` may be NULL: only a text moves it. */
        if (edit[2]) {
            texts += edit[2];
        }
    }
    const markdown_core_document *document = NULL;
    markdown_core_status status = markdown_core_session_edit(session, batch, count, &document);
    free(batch);
    return step_message(status, document);
}

uint8_t *markdown_core_wire_session_append(markdown_core_session *session, const uint8_t *text, size_t size) {
    const markdown_core_document *document = NULL;
    markdown_core_status status = markdown_core_session_append(session, text, size, &document);
    return step_message(status, document);
}

void markdown_core_wire_free(uint8_t *message) { free(message); }
