#include "markdown_core_kotlin_payload.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum payload_failure { PAYLOAD_OK = 0, PAYLOAD_ALLOCATION, PAYLOAD_INTERNAL } payload_failure;

typedef struct payload_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    payload_failure failure;
} payload_buffer;

typedef enum payload_action_kind {
    PAYLOAD_WRITE_NODE,
    PAYLOAD_WRITE_SIBLINGS,
    PAYLOAD_WRITE_CHILDREN,
    PAYLOAD_WRITE_CHAIN,
    PAYLOAD_WRITE_DEFINITION_BODIES,
    PAYLOAD_WRITE_DEFINITION_BODY
} payload_action_kind;

/* A sibling or body action carries the offset of the count it stands for:
 * the count is written as a placeholder when the list is scheduled and
 * filled in when the chain ends, so a chain is walked once, as it is
 * written, and never first to count it. `written` is how many the action
 * has already scheduled. */
typedef struct payload_action {
    payload_action_kind kind;
    const markdown_core_node *node;
    size_t written;
    size_t count_offset;
    const markdown_core_definition_body *body;
} payload_action;

typedef struct payload_stack {
    payload_action *actions;
    size_t count;
    size_t capacity;
} payload_stack;

/* Every occurrence of one reference definition reads through one resource in
 * the C tree. This table numbers each distinct resource in the order the
 * payload first meets it, so a destination and title cross the boundary once
 * however often the definition is used. */
typedef struct payload_resource_slot {
    const markdown_core_resource *resource;
    int32_t ordinal;
} payload_resource_slot;

typedef struct payload_resources {
    payload_resource_slot *slots;
    size_t count;
    size_t capacity;
} payload_resources;

static const uint8_t payload_magic[] = {'M', 'K', 'J', '1'};
static const uint8_t internal_error_bytes[] = "could not encode the AST payload";

/* Grows the buffer so `additional` more bytes fit. The first growth takes
 * the caller's estimate, so a payload sized from its source reallocates
 * rarely instead of doubling up from one kilobyte. */
static bool grow(payload_buffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    uint8_t *data;
    if (buffer->failure != PAYLOAD_OK) {
        return false;
    }
    if (additional > SIZE_MAX - buffer->size) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return false;
    }
    required = buffer->size + additional;
    capacity = buffer->capacity == 0 ? 1024 : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    data = (uint8_t *)realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

/* True when `additional` more bytes fit, growing if they do not: the one
 * check a field pays before its bytes are stored in place. */
static inline bool ensure(payload_buffer *buffer, size_t additional) {
    if (additional <= buffer->capacity - buffer->size && buffer->failure == PAYLOAD_OK) {
        return true;
    }
    return grow(buffer, additional);
}

static void put_bytes(payload_buffer *buffer, const uint8_t *bytes, size_t length) {
    if (ensure(buffer, length) && length != 0) {
        memcpy(buffer->data + buffer->size, bytes, length);
        buffer->size += length;
    }
}

static inline void store_i32(uint8_t *at, int32_t value) {
    uint32_t bits = (uint32_t)value;
    at[0] = (uint8_t)bits;
    at[1] = (uint8_t)(bits >> 8);
    at[2] = (uint8_t)(bits >> 16);
    at[3] = (uint8_t)(bits >> 24);
}

static inline void put_u8(payload_buffer *buffer, uint8_t value) {
    if (ensure(buffer, 1)) {
        buffer->data[buffer->size++] = value;
    }
}

static inline void put_i32(payload_buffer *buffer, int32_t value) {
    if (ensure(buffer, 4)) {
        store_i32(buffer->data + buffer->size, value);
        buffer->size += 4;
    }
}

static inline void put_i64(payload_buffer *buffer, int64_t value) {
    if (ensure(buffer, 8)) {
        uint64_t bits = (uint64_t)value;
        uint8_t *at = buffer->data + buffer->size;
        size_t index;
        for (index = 0; index < 8; ++index) {
            at[index] = (uint8_t)(bits >> (index * 8));
        }
        buffer->size += 8;
    }
}

static void put_scope(payload_buffer *buffer, markdown_core_scope scope) {
    if (ensure(buffer, 16)) {
        uint8_t *at = buffer->data + buffer->size;
        store_i32(at, scope.start.line);
        store_i32(at + 4, scope.start.column);
        store_i32(at + 8, scope.end.line);
        store_i32(at + 12, scope.end.column);
        buffer->size += 16;
    }
}

static void put_optional_string(payload_buffer *buffer, markdown_core_optional_string value);

static void put_string(payload_buffer *buffer, markdown_core_string value, bool present) {
    if (!present) {
        put_i32(buffer, -1);
        return;
    }
    if (value.length > INT32_MAX) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return;
    }
    if (ensure(buffer, 4 + value.length)) {
        uint8_t *at = buffer->data + buffer->size;
        store_i32(at, (int32_t)value.length);
        if (value.length != 0) {
            memcpy(at + 4, value.data, value.length);
        }
        buffer->size += 4 + value.length;
    }
}

/* Optional-string presence is explicit; an empty present string is distinct
 * from an absent string. */
static void put_optional_string(payload_buffer *buffer, markdown_core_optional_string value) {
    put_string(buffer, value.value, value.has_value);
}

static void write_error(payload_buffer *buffer, markdown_core_error_code code, markdown_core_string message) {
    put_u8(buffer, 1);
    put_i32(buffer, code);
    put_string(buffer, message, true);
}

static void push_action(payload_buffer *buffer, payload_stack *stack, payload_action action) {
    size_t capacity;
    payload_action *actions;
    if (buffer->failure != PAYLOAD_OK) {
        return;
    }
    if (stack->count == stack->capacity) {
        capacity = stack->capacity == 0 ? 64 : stack->capacity * 2;
        if (capacity < stack->capacity || capacity > SIZE_MAX / sizeof(*stack->actions)) {
            buffer->failure = PAYLOAD_ALLOCATION;
            return;
        }
        actions = (payload_action *)realloc(stack->actions, capacity * sizeof(*stack->actions));
        if (actions == NULL) {
            buffer->failure = PAYLOAD_ALLOCATION;
            return;
        }
        stack->actions = actions;
        stack->capacity = capacity;
    }
    stack->actions[stack->count++] = action;
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
static payload_resource_slot *find_resource_slot(payload_resource_slot *slots, size_t capacity,
                                                 const markdown_core_resource *resource) {
    size_t position = hash_resource(resource) & (capacity - 1);
    for (;;) {
        payload_resource_slot *slot = &slots[position];
        if (slot->resource == NULL || slot->resource == resource) {
            return slot;
        }
        position = (position + 1) & (capacity - 1);
    }
}

static bool grow_resources(payload_resources *resources) {
    size_t capacity = resources->capacity == 0 ? 64 : resources->capacity * 2;
    payload_resource_slot *slots;
    size_t index;
    if (capacity < resources->capacity || capacity > SIZE_MAX / sizeof(*slots)) {
        return false;
    }
    slots = (payload_resource_slot *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    for (index = 0; index < resources->capacity; ++index) {
        const payload_resource_slot *source = &resources->slots[index];
        if (source->resource != NULL) {
            *find_resource_slot(slots, capacity, source->resource) = *source;
        }
    }
    free(resources->slots);
    resources->slots = slots;
    resources->capacity = capacity;
    return true;
}

/* The ordinal of `resource` in first-sight order; `first_sight` says whether
 * this call is the sight that assigned it. */
static int32_t resource_ordinal(payload_buffer *buffer, payload_resources *resources,
                                const markdown_core_resource *resource, bool *first_sight) {
    payload_resource_slot *slot;
    *first_sight = false;
    if (resources->count + 1 > resources->capacity / 2 && !grow_resources(resources)) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return -1;
    }
    slot = find_resource_slot(resources->slots, resources->capacity, resource);
    if (slot->resource == NULL) {
        if (resources->count > INT32_MAX) {
            buffer->failure = PAYLOAD_ALLOCATION;
            return -1;
        }
        slot->resource = resource;
        slot->ordinal = (int32_t)resources->count;
        resources->count++;
        *first_sight = true;
    }
    return slot->ordinal;
}

/* Writes the count a list stands for once its chain has been walked. */
static void fill_count(payload_buffer *buffer, size_t count_offset, size_t count) {
    if (buffer->failure != PAYLOAD_OK) {
        return;
    }
    if (count > INT32_MAX) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return;
    }
    store_i32(buffer->data + count_offset, (int32_t)count);
}

/* Schedules the chain starting at `first` as one counted list: the count is
 * a placeholder the sibling action fills in when it reaches the end of the
 * chain, so the chain is walked once. An empty chain is the count zero. */
static void schedule_nodes(payload_buffer *buffer, payload_stack *stack, const markdown_core_node *first) {
    size_t count_offset = buffer->size;
    put_i32(buffer, 0);
    if (first != NULL) {
        payload_action action = {
            .kind = PAYLOAD_WRITE_SIBLINGS, .node = first, .written = 0, .count_offset = count_offset};
        push_action(buffer, stack, action);
    }
}

static void schedule_children(payload_buffer *buffer, payload_stack *stack, const markdown_core_node *node) {
    schedule_nodes(buffer, stack, markdown_core_node_get_first_child(node));
}

static void write_attributes(payload_buffer *buffer, const markdown_core_attribute_value *attributes) {
    put_optional_string(buffer, markdown_core_attribute_value_anchor(attributes));
    size_t classes = markdown_core_attribute_value_class_count(attributes),
           records = markdown_core_attribute_value_record_count(attributes);
    if (classes > INT32_MAX || records > INT32_MAX) {
        buffer->failure = PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)classes);
    for (size_t i = 0; i < classes; i++) {
        markdown_core_string value;
        if (!markdown_core_attribute_value_class_at(attributes, i, &value)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, value, true);
    }
    put_i32(buffer, (int32_t)records);
    for (size_t i = 0; i < records; i++) {
        markdown_core_string name, value;
        if (!markdown_core_attribute_value_record_at(attributes, i, &name, &value)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, name, true);
        put_string(buffer, value, true);
    }
}

static void write_metadata_value(payload_buffer *buffer, const markdown_core_metadata_value *record) {
    put_u8(buffer, record ? 1 : 0);
    if (!record) {
        return;
    }
    markdown_core_metadata_value_kind kind = markdown_core_metadata_value_get_kind(record);
    put_u8(buffer, (uint8_t)kind);
    if (kind == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar value;
        if (!markdown_core_metadata_value_scalar(record, &value)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, (uint8_t)value.kind);
        switch (value.kind) {
        case MARKDOWN_CORE_METADATA_NULL:
            break;
        case MARKDOWN_CORE_METADATA_BOOL:
            put_u8(buffer, value.value.boolean ? 1 : 0);
            break;
        case MARKDOWN_CORE_METADATA_NUMBER:
        case MARKDOWN_CORE_METADATA_TEXT:
            put_string(buffer, value.value.string, true);
            break;
        default:
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
    } else if (kind == MARKDOWN_CORE_METADATA_LIST) {
        size_t items = markdown_core_metadata_value_item_count(record);
        if (items > INT32_MAX) {
            buffer->failure = PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, (int32_t)items);
        for (size_t j = 0; j < items; j++) {
            markdown_core_metadata_list_item item;
            if (!markdown_core_metadata_value_item_at(record, j, &item)) {
                buffer->failure = PAYLOAD_INTERNAL;
                return;
            }
            put_u8(buffer, (uint8_t)item.kind);
            put_string(buffer, item.value, true);
        }
    } else {
        buffer->failure = PAYLOAD_INTERNAL;
        return;
    }
}
static void write_metadata(payload_buffer *buffer, const markdown_core_node *metadata) {
    write_metadata_value(buffer, markdown_core_metadata_name(metadata));
    write_metadata_value(buffer, markdown_core_metadata_title(metadata));
    write_metadata_value(buffer, markdown_core_metadata_subtitle(metadata));
    write_metadata_value(buffer, markdown_core_metadata_time(metadata));
    write_metadata_value(buffer, markdown_core_metadata_date(metadata));
    write_metadata_value(buffer, markdown_core_metadata_authors(metadata));
    write_metadata_value(buffer, markdown_core_metadata_keywords(metadata));
    write_metadata_value(buffer, markdown_core_metadata_abstract(metadata));
    write_metadata_value(buffer, markdown_core_metadata_state(metadata));
    write_metadata_value(buffer, markdown_core_metadata_comment(metadata));
}

static void put_dimensions(payload_buffer *buffer, const markdown_core_dimensions *dimensions) {
    if (dimensions &&
        (dimensions->width < 1 ||
         (dimensions->height.has_value && (dimensions->height.value < 1 || dimensions->height.value > INT32_MAX)))) {
        buffer->failure = PAYLOAD_INTERNAL;
        return;
    }
    put_u8(buffer, dimensions ? 1 : 0);
    if (dimensions) {
        put_i32(buffer, dimensions->width);
        put_u8(buffer, dimensions->height.has_value ? 1 : 0);
        if (dimensions->height.has_value) {
            put_i32(buffer, (int32_t)dimensions->height.value);
        }
    }
}

static void write_node(payload_buffer *buffer, payload_stack *stack, payload_resources *resources,
                       const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {0};
    markdown_core_string third = {0};
    markdown_core_optional_string optional_first = {0};
    markdown_core_optional_string optional_second = {0};

    /* One reservation covers the header and any kind's fixed fields; each
     * store below then finds its room already there. */
    if (!ensure(buffer, 64)) {
        return;
    }
    put_u8(buffer, (uint8_t)kind);
    put_scope(buffer, markdown_core_node_scope(node));
    write_attributes(buffer, markdown_core_node_primary_attributes(node));
    if (!ensure(buffer, 64)) {
        return;
    }

    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT: {
        /* The metadata leads, then the title -- a node-valued list sent before
         * the content, as the walk visits it, whose count is its presence
         * because a present title holds at least one node -- then the
         * content. */
        markdown_core_optional_bool collapsed;
        payload_action children = {.kind = PAYLOAD_WRITE_CHILDREN, .node = node};
        if (!markdown_core_node_callout_properties(node, &optional_first, &collapsed)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        put_u8(buffer, collapsed.has_value ? (collapsed.value ? 1 : 0) : UINT8_MAX);
        push_action(buffer, stack, children);
        schedule_nodes(buffer, stack, markdown_core_node_callout_title(node));
        break;
    }
    case MARKDOWN_CORE_KIND_DOCUMENT: {
        const markdown_core_node *metadata = markdown_core_node_document_metadata(node);
        put_u8(buffer, metadata ? 1 : 0);
        push_action(buffer, stack,
                    (payload_action){.kind = PAYLOAD_WRITE_CHAIN, .node = markdown_core_node_document_specimens(node)});
        push_action(buffer, stack,
                    (payload_action){.kind = PAYLOAD_WRITE_CHAIN, .node = markdown_core_node_document_footnotes(node)});
        push_action(buffer, stack, (payload_action){.kind = PAYLOAD_WRITE_CHILDREN, .node = node});
        if (metadata) {
            push_action(buffer, stack, (payload_action){.kind = PAYLOAD_WRITE_NODE, .node = metadata});
        }
        break;
    }
    case MARKDOWN_CORE_KIND_METADATA:
        write_metadata(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE:
        if (!markdown_core_footnote_id(node, &first)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_SPECIMEN: {
        markdown_core_optional_i64 start;
        if (!markdown_core_specimen_properties(node, &optional_first, &start)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        put_i64(buffer, start.value);
        put_u8(buffer, start.has_value ? 1 : 0);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_CITATION: {
        markdown_core_referent referent;
        if (!markdown_core_citation_referent(node, &referent)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, (uint8_t)referent.kind);
        switch (referent.kind) {
        case MARKDOWN_CORE_REFERENT_BIB:
            put_string(buffer, referent.key, true);
            put_i32(buffer, referent.mode);
            break;
        case MARKDOWN_CORE_REFERENT_FOOTNOTE:
        case MARKDOWN_CORE_REFERENT_SPECIMEN:
            put_string(buffer, referent.id, true);
            break;
        default:
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        push_action(buffer, stack,
                    (payload_action){.kind = PAYLOAD_WRITE_CHAIN, .node = markdown_core_citation_suffix(node)});
        schedule_nodes(buffer, stack, markdown_core_citation_prefix(node));
        break;
    }
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_MARK:
    case MARKDOWN_CORE_KIND_INSERTION:
    case MARKDOWN_CORE_KIND_SPAN:
    case MARKDOWN_CORE_KIND_SUPERSCRIPT:
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
    case MARKDOWN_CORE_KIND_SUBSCRIPT:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        if (!markdown_core_node_heading_level(node, &level)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, level);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
        break;
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor;
        markdown_core_ordered_list_variant variant;
        markdown_core_ordered_list_delimiter delimiter;
        markdown_core_optional_i64 start;
        bool tight = false;
        if (!markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &tight)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, (int32_t)flavor);
        put_i64(buffer, start.value);
        put_u8(buffer, start.has_value ? 1 : 0);
        put_i32(buffer, start.has_value ? (int32_t)variant.kind : 0);
        put_u8(buffer, variant.lowercased ? 1 : 0);
        put_i32(buffer, start.has_value ? (int32_t)delimiter.kind : 0);
        put_u8(buffer, delimiter.closed ? 1 : 0);
        put_u8(buffer, tight ? 1 : 0);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_string marker;
        if (!markdown_core_node_list_item_marker(node, &marker)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, marker);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced = false;
        bool closed = false;
        if (!markdown_core_node_code_block_properties(node, &optional_first, &optional_second, &third, &fenced,
                                                      &closed)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        put_optional_string(buffer, optional_second);
        put_string(buffer, third, true);
        put_u8(buffer, fenced ? 1 : 0);
        put_u8(buffer, closed ? 1 : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
        if (!markdown_core_node_literal(node, &first)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, (int32_t)mode);
        put_string(buffer, first, true);
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: {
        markdown_core_placement mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first) || mode != MARKDOWN_CORE_PLACEMENT_STANDALONE) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count, head, content, foot;
        if (!markdown_core_node_table_properties(node, &count, &head, &content, &foot)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        if (count > INT32_MAX || head > INT32_MAX || content > INT32_MAX || foot > INT32_MAX) {
            buffer->failure = PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, (int32_t)count);
        for (size_t index = 0; index < count; ++index) {
            markdown_core_table_column column;
            if (!markdown_core_node_table_column_at(node, index, &column)) {
                buffer->failure = PAYLOAD_INTERNAL;
                return;
            }
            put_u8(buffer, (uint8_t)column.flow);
            put_u8(buffer, column.relative.has_value ? 1 : 0);
            if (column.relative.has_value) {
                int64_t bits;
                memcpy(&bits, &column.relative.value, sizeof(bits));
                put_i64(buffer, bits);
            }
        }
        put_i32(buffer, (int32_t)head);
        put_i32(buffer, (int32_t)content);
        put_i32(buffer, (int32_t)foot);
        const markdown_core_node *caption = markdown_core_node_table_caption(node);
        put_u8(buffer, caption ? 1 : 0);
        if (caption) {
            payload_action children = {.kind = PAYLOAD_WRITE_CHILDREN, .node = node};
            payload_action field = {.kind = PAYLOAD_WRITE_NODE, .node = caption};
            push_action(buffer, stack, children);
            push_action(buffer, stack, field);
        } else {
            schedule_children(buffer, stack, node);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_DEFINITION: {
        bool compact;
        if (!markdown_core_node_definition_compact(node, &compact)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, compact ? 1 : 0);
        payload_action bodies = {.kind = PAYLOAD_WRITE_DEFINITION_BODIES,
                                 .body = markdown_core_node_definition_bodies(node)};
        push_action(buffer, stack, bodies);
        schedule_nodes(buffer, stack, markdown_core_node_definition_term(node));
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* A label is a node-valued field, not directive content. Preserve that
         * boundary on the wire instead of flattening it into the child list. */
        if (!markdown_core_node_directive_properties(node, &optional_first)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        const markdown_core_node *label = markdown_core_node_directive_label(node);
        put_u8(buffer, label ? 1 : 0);
        if (label != NULL) {
            payload_action children = {.kind = PAYLOAD_WRITE_CHILDREN, .node = node};
            payload_action label_node = {.kind = PAYLOAD_WRITE_NODE, .node = label};
            push_action(buffer, stack, children);
            push_action(buffer, stack, label_node);
        } else {
            schedule_children(buffer, stack, node);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_CITE:
        /* Citation nodes belong to the citations field, outside ordinary content. */
        schedule_nodes(buffer, stack, markdown_core_node_cite_citations(node));
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: {
        markdown_core_destination destination;
        if (!markdown_core_node_destination(node, &destination)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, (int32_t)destination.kind);
        put_string(buffer, destination.path, true);
        put_optional_string(buffer, destination.anchor);
        put_optional_string(buffer, markdown_core_node_cross_label(node));
        if (kind == MARKDOWN_CORE_KIND_CROSS_EMBEDDED) {
            put_dimensions(buffer, markdown_core_node_dimensions(node));
        }
        break;
    }
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_EMBEDDED: {
        /* The resource's ordinal leads. Only its first sight carries the
         * destination and title; a later occurrence names the ordinal and
         * nothing else, so the decoder materializes each resource once. */
        markdown_core_destination destination;
        const markdown_core_resource *resource = markdown_core_node_resource(node);
        bool first_sight = false;
        int32_t ordinal;
        if (resource == NULL) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        ordinal = resource_ordinal(buffer, resources, resource, &first_sight);
        if (buffer->failure != PAYLOAD_OK) {
            return;
        }
        put_i32(buffer, ordinal);
        if (first_sight) {
            if (!markdown_core_node_destination(node, &destination) ||
                !markdown_core_node_title(node, &optional_first)) {
                buffer->failure = PAYLOAD_INTERNAL;
                return;
            }
            /* The branch ordinal leads and only that branch's fields follow it. */
            put_i32(buffer, (int32_t)destination.kind);
            switch (destination.kind) {
            case MARKDOWN_CORE_DESTINATION_URL:
                put_string(buffer, destination.url, true);
                break;
            case MARKDOWN_CORE_DESTINATION_CROSS:
                put_string(buffer, destination.path, true);
                put_optional_string(buffer, destination.anchor);
                break;
            default:
                buffer->failure = PAYLOAD_INTERNAL;
                return;
            }
            put_optional_string(buffer, optional_first);
            write_attributes(buffer, markdown_core_node_inherited_attributes(node));
        }
        if (kind == MARKDOWN_CORE_KIND_EMBEDDED) {
            put_dimensions(buffer, markdown_core_node_dimensions(node));
        }
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        int64_t rowspan, colspan;
        if (!markdown_core_node_table_cell_spans(node, &rowspan, &colspan)) {
            buffer->failure = PAYLOAD_INTERNAL;
            return;
        }
        put_i64(buffer, rowspan);
        put_i64(buffer, colspan);
        schedule_children(buffer, stack, node);
        break;
    }

    default:
        buffer->failure = PAYLOAD_INTERNAL;
        break;
    }
}

static void write_tree(payload_buffer *buffer, const markdown_core_node *root) {
    payload_stack stack = {0};
    payload_resources resources = {0};
    payload_action root_action = {.kind = PAYLOAD_WRITE_NODE, .node = root};
    push_action(buffer, &stack, root_action);
    while (stack.count != 0 && buffer->failure == PAYLOAD_OK) {
        payload_action action = stack.actions[--stack.count];
        switch (action.kind) {
        case PAYLOAD_WRITE_NODE:
            if (action.node == NULL) {
                buffer->failure = PAYLOAD_INTERNAL;
            } else {
                write_node(buffer, &stack, &resources, action.node);
            }
            break;
        case PAYLOAD_WRITE_SIBLINGS: {
            const markdown_core_node *next;
            payload_action node_action = {0};
            size_t written = action.written + 1;
            if (action.node == NULL) {
                buffer->failure = PAYLOAD_INTERNAL;
                break;
            }
            next = markdown_core_node_get_next_sibling(action.node);
            if (next != NULL) {
                payload_action siblings = {.kind = PAYLOAD_WRITE_SIBLINGS,
                                           .node = next,
                                           .written = written,
                                           .count_offset = action.count_offset};
                push_action(buffer, &stack, siblings);
            } else {
                fill_count(buffer, action.count_offset, written);
            }
            node_action.kind = PAYLOAD_WRITE_NODE;
            node_action.node = action.node;
            push_action(buffer, &stack, node_action);
            break;
        }
        case PAYLOAD_WRITE_CHILDREN:
            schedule_children(buffer, &stack, action.node);
            break;
        case PAYLOAD_WRITE_CHAIN:
            schedule_nodes(buffer, &stack, action.node);
            break;
        case PAYLOAD_WRITE_DEFINITION_BODIES: {
            /* A definition has at least one body; the count fills in as
             * the last body is scheduled. */
            if (action.body == NULL) {
                buffer->failure = PAYLOAD_INTERNAL;
                break;
            }
            action.count_offset = buffer->size;
            put_i32(buffer, 0);
            action.kind = PAYLOAD_WRITE_DEFINITION_BODY;
            action.written = 0;
            push_action(buffer, &stack, action);
            break;
        }
        case PAYLOAD_WRITE_DEFINITION_BODY: {
            const markdown_core_definition_body *body = action.body;
            action.body = markdown_core_definition_body_next(body);
            action.written++;
            if (action.body) {
                push_action(buffer, &stack, action);
            } else {
                fill_count(buffer, action.count_offset, action.written);
            }
            schedule_nodes(buffer, &stack, markdown_core_definition_body_content(body));
            break;
        }
        }
    }
    free(stack.actions);
    free(resources.slots);
}

bool markdown_core_kotlin_payload_encode(const uint8_t *source, size_t length, uint8_t **output,
                                         size_t *output_length) {
    markdown_core_string internal_error = {internal_error_bytes, sizeof(internal_error_bytes) - 1};
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    payload_buffer buffer = {0};
    const markdown_core_node *root;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    document = markdown_core_document_parse(source, length, &error);

    /* A payload runs a few times its source: node headers cost more than
     * the bytes that produced them. Sizing the buffer from the source up
     * front leaves the doubling to the documents that outrun the estimate. */
    if (length <= (SIZE_MAX - 4096) / 4) {
        grow(&buffer, length * 4 + 4096);
    }
    put_bytes(&buffer, payload_magic, sizeof(payload_magic));
    if (document == NULL) {
        markdown_core_error_code code =
            error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error);
        markdown_core_string message = error == NULL ? internal_error : markdown_core_error_get_message(error);
        write_error(&buffer, code, message);
        markdown_core_error_free(error);
    } else {
        put_u8(&buffer, 0);
        root = markdown_core_document_root(document);
        if (root == NULL) {
            buffer.failure = PAYLOAD_INTERNAL;
        } else {
            write_tree(&buffer, root);
        }
        markdown_core_document_free(document);
    }

    if (buffer.failure == PAYLOAD_INTERNAL) {
        free(buffer.data);
        memset(&buffer, 0, sizeof(buffer));
        put_bytes(&buffer, payload_magic, sizeof(payload_magic));
        write_error(&buffer, MARKDOWN_CORE_ERROR_INTERNAL, internal_error);
    }
    if (buffer.failure != PAYLOAD_OK) {
        free(buffer.data);
        return false;
    }
    *output = buffer.data;
    *output_length = buffer.size;
    return true;
}

void markdown_core_kotlin_payload_free(uint8_t *output) { free(output); }
