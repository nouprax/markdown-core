#include "markdown_core_kotlin_jni_payload.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef enum jni_payload_failure {
    JNI_PAYLOAD_OK = 0,
    JNI_PAYLOAD_ALLOCATION,
    JNI_PAYLOAD_INTERNAL
} jni_payload_failure;

typedef struct jni_payload_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    jni_payload_failure failure;
} jni_payload_buffer;

typedef enum jni_payload_action_kind {
    JNI_PAYLOAD_WRITE_NODE,
    JNI_PAYLOAD_WRITE_SIBLINGS,
    JNI_PAYLOAD_WRITE_CHILDREN,
    /* The document's footnotes after its content: the count, then each. */
    JNI_PAYLOAD_WRITE_FOOTNOTES,
    /* One footnote -- scope, id, content -- then the rest of the chain. */
    JNI_PAYLOAD_WRITE_FOOTNOTE,
    JNI_PAYLOAD_WRITE_SPECIMENS,
    JNI_PAYLOAD_WRITE_SPECIMEN,
    /* One citation -- scope, referent, prefix, suffix -- then the rest. */
    JNI_PAYLOAD_WRITE_CITATION,
    /* A citation's suffix, after its prefix has been written. */
    JNI_PAYLOAD_WRITE_SUFFIX,
    JNI_PAYLOAD_WRITE_DEFINITION_BODIES,
    JNI_PAYLOAD_WRITE_DEFINITION_BODY
} jni_payload_action_kind;

typedef struct jni_payload_action {
    jni_payload_action_kind kind;
    const markdown_core_node *node;
    const markdown_core_footnote *footnote;
    const markdown_core_citation *citation;
    size_t remaining;
    const markdown_core_specimen *specimen;
    const markdown_core_definition_body *body;
} jni_payload_action;

typedef struct jni_payload_stack {
    jni_payload_action *actions;
    size_t count;
    size_t capacity;
} jni_payload_stack;

/* Every occurrence of one reference definition reads through one resource in
 * the C tree. This table numbers each distinct resource in the order the
 * payload first meets it, so a destination and title cross the boundary once
 * however often the definition is used. */
typedef struct jni_payload_resource_slot {
    const markdown_core_resource *resource;
    int32_t ordinal;
} jni_payload_resource_slot;

typedef struct jni_payload_resources {
    jni_payload_resource_slot *slots;
    size_t count;
    size_t capacity;
} jni_payload_resources;

static const uint8_t jni_payload_magic[] = {'M', 'K', 'J', '1'};
static const uint8_t internal_error_bytes[] = "could not encode JNI AST payload";

static void reserve(jni_payload_buffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    uint8_t *data;
    if (buffer->failure != JNI_PAYLOAD_OK) {
        return;
    }
    if (additional > SIZE_MAX - buffer->size) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    required = buffer->size + additional;
    if (required <= buffer->capacity) {
        return;
    }
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
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void put_bytes(jni_payload_buffer *buffer, const uint8_t *bytes, size_t length) {
    reserve(buffer, length);
    if (buffer->failure == JNI_PAYLOAD_OK && length != 0) {
        memcpy(buffer->data + buffer->size, bytes, length);
        buffer->size += length;
    }
}

static void put_u8(jni_payload_buffer *buffer, uint8_t value) { put_bytes(buffer, &value, 1); }

static void put_i32(jni_payload_buffer *buffer, int32_t value) {
    uint32_t bits = (uint32_t)value;
    size_t index;
    for (index = 0; index < 4; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_i64(jni_payload_buffer *buffer, int64_t value) {
    uint64_t bits = (uint64_t)value;
    size_t index;
    for (index = 0; index < 8; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_scope(jni_payload_buffer *buffer, markdown_core_scope scope) {
    put_i32(buffer, scope.start.line);
    put_i32(buffer, scope.start.column);
    put_i32(buffer, scope.end.line);
    put_i32(buffer, scope.end.column);
}

static void put_optional_string(jni_payload_buffer *buffer, markdown_core_optional_string value);

static void put_string(jni_payload_buffer *buffer, markdown_core_string value, bool present) {
    if (!present) {
        put_i32(buffer, -1);
        return;
    }
    if (value.length > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)value.length);
    put_bytes(buffer, value.data, value.length);
}

/* Optional-string presence is explicit; an empty present string is distinct
 * from an absent string. */
static void put_optional_string(jni_payload_buffer *buffer, markdown_core_optional_string value) {
    put_string(buffer, value.value, value.has_value);
}

static void write_error(jni_payload_buffer *buffer, markdown_core_error_code code, markdown_core_string message) {
    put_u8(buffer, 1);
    put_i32(buffer, code);
    put_string(buffer, message, true);
}

static void push_action(jni_payload_buffer *buffer, jni_payload_stack *stack, jni_payload_action action) {
    size_t capacity;
    jni_payload_action *actions;
    if (buffer->failure != JNI_PAYLOAD_OK) {
        return;
    }
    if (stack->count == stack->capacity) {
        capacity = stack->capacity == 0 ? 64 : stack->capacity * 2;
        if (capacity < stack->capacity || capacity > SIZE_MAX / sizeof(*stack->actions)) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return;
        }
        actions = (jni_payload_action *)realloc(stack->actions, capacity * sizeof(*stack->actions));
        if (actions == NULL) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
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
static jni_payload_resource_slot *find_resource_slot(jni_payload_resource_slot *slots, size_t capacity,
                                                     const markdown_core_resource *resource) {
    size_t position = hash_resource(resource) & (capacity - 1);
    for (;;) {
        jni_payload_resource_slot *slot = &slots[position];
        if (slot->resource == NULL || slot->resource == resource) {
            return slot;
        }
        position = (position + 1) & (capacity - 1);
    }
}

static bool grow_resources(jni_payload_resources *resources) {
    size_t capacity = resources->capacity == 0 ? 64 : resources->capacity * 2;
    jni_payload_resource_slot *slots;
    size_t index;
    if (capacity < resources->capacity || capacity > SIZE_MAX / sizeof(*slots)) {
        return false;
    }
    slots = (jni_payload_resource_slot *)calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    for (index = 0; index < resources->capacity; ++index) {
        const jni_payload_resource_slot *source = &resources->slots[index];
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
static int32_t resource_ordinal(jni_payload_buffer *buffer, jni_payload_resources *resources,
                                const markdown_core_resource *resource, bool *first_sight) {
    jni_payload_resource_slot *slot;
    *first_sight = false;
    if (resources->count + 1 > resources->capacity / 2 && !grow_resources(resources)) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return -1;
    }
    slot = find_resource_slot(resources->slots, resources->capacity, resource);
    if (slot->resource == NULL) {
        if (resources->count > INT32_MAX) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return -1;
        }
        slot->resource = resource;
        slot->ordinal = (int32_t)resources->count;
        resources->count++;
        *first_sight = true;
    }
    return slot->ordinal;
}

static void schedule_nodes(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *node,
                           size_t count) {
    if (count > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)count);
    if ((count == 0) != (node == NULL)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    if (count != 0) {
        jni_payload_action action = {.kind = JNI_PAYLOAD_WRITE_SIBLINGS, .node = node, .remaining = count};
        push_action(buffer, stack, action);
    }
}

static void schedule_children(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *node) {
    schedule_nodes(buffer, stack, markdown_core_node_get_first_child(node), markdown_core_node_child_count(node));
}

/* A chain a value owns -- an affix or a footnote's content -- has no owner
 * to count it, so it is counted by walking. */
static size_t chain_length(const markdown_core_node *node) {
    size_t count = 0;
    for (; node; node = markdown_core_node_get_next_sibling(node)) {
        count++;
    }
    return count;
}

static void schedule_chain(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *first) {
    schedule_nodes(buffer, stack, first, chain_length(first));
}

/* The document's footnotes follow its content (M4): the count, then each
 * footnote's scope, id, and content. */
static void write_footnotes(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *root) {
    const markdown_core_footnote *first = markdown_core_node_document_footnotes(root);
    const markdown_core_footnote *cursor;
    size_t count = 0;
    for (cursor = first; cursor; cursor = markdown_core_footnote_next(cursor)) {
        count++;
    }
    if (count > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)count);
    if (count != 0) {
        jni_payload_action action = {.kind = JNI_PAYLOAD_WRITE_FOOTNOTE, .footnote = first, .remaining = count};
        push_action(buffer, stack, action);
    }
}

static void write_footnote(jni_payload_buffer *buffer, jni_payload_stack *stack, jni_payload_action action) {
    const markdown_core_footnote *next = markdown_core_footnote_next(action.footnote);
    markdown_core_string id;
    if ((action.remaining == 1) != (next == NULL)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    if (action.remaining > 1) {
        jni_payload_action rest = {
            .kind = JNI_PAYLOAD_WRITE_FOOTNOTE, .footnote = next, .remaining = action.remaining - 1};
        push_action(buffer, stack, rest);
    }
    if (!markdown_core_footnote_id(action.footnote, &id)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    put_scope(buffer, markdown_core_footnote_scope(action.footnote));
    put_string(buffer, id, true);
    schedule_chain(buffer, stack, markdown_core_footnote_content(action.footnote));
}

static void write_specimens(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *root) {
    const markdown_core_specimen *first = markdown_core_node_document_specimens(root);
    const markdown_core_specimen *cursor;
    size_t count = 0;
    for (cursor = first; cursor; cursor = markdown_core_specimen_next(cursor)) {
        count++;
    }
    if (count > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)count);
    if (count != 0) {
        jni_payload_action action = {.kind = JNI_PAYLOAD_WRITE_SPECIMEN, .specimen = first, .remaining = count};
        push_action(buffer, stack, action);
    }
}

static void write_specimen(jni_payload_buffer *buffer, jni_payload_stack *stack, jni_payload_action action) {
    const markdown_core_specimen *next = markdown_core_specimen_next(action.specimen);
    markdown_core_optional_string id;
    markdown_core_optional_i64 start;
    if ((action.remaining == 1) != (next == NULL)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    if (action.remaining > 1) {
        jni_payload_action rest = {
            .kind = JNI_PAYLOAD_WRITE_SPECIMEN, .specimen = next, .remaining = action.remaining - 1};
        push_action(buffer, stack, rest);
    }
    if (!markdown_core_specimen_properties(action.specimen, &id, &start)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    put_scope(buffer, markdown_core_specimen_scope(action.specimen));
    put_optional_string(buffer, id);
    put_i64(buffer, start.value);
    put_u8(buffer, start.has_value ? 1 : 0);
    schedule_chain(buffer, stack, markdown_core_specimen_content(action.specimen));
}

/* A cite's items (M4): the count, then each citation's scope, its referent
 * -- the branch ordinal, then only that branch's fields -- and its prefix and
 * suffix content in that order. */
static void write_citations(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *node) {
    const markdown_core_citation *first = markdown_core_node_cite_citations(node);
    const markdown_core_citation *cursor;
    size_t count = 0;
    for (cursor = first; cursor; cursor = markdown_core_citation_next(cursor)) {
        count++;
    }
    if (count > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    if (count == 0) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    put_i32(buffer, (int32_t)count);
    {
        jni_payload_action action = {.kind = JNI_PAYLOAD_WRITE_CITATION, .citation = first, .remaining = count};
        push_action(buffer, stack, action);
    }
}

static void write_citation(jni_payload_buffer *buffer, jni_payload_stack *stack, jni_payload_action action) {
    const markdown_core_citation *next = markdown_core_citation_next(action.citation);
    markdown_core_referent referent;
    jni_payload_action suffix = {.kind = JNI_PAYLOAD_WRITE_SUFFIX, .citation = action.citation};
    if ((action.remaining == 1) != (next == NULL)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    if (action.remaining > 1) {
        jni_payload_action rest = {
            .kind = JNI_PAYLOAD_WRITE_CITATION, .citation = next, .remaining = action.remaining - 1};
        push_action(buffer, stack, rest);
    }
    if (!markdown_core_citation_referent(action.citation, &referent)) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    put_scope(buffer, markdown_core_citation_scope(action.citation));
    put_u8(buffer, (uint8_t)referent.kind);
    switch (referent.kind) {
    case MARKDOWN_CORE_REFERENT_BIB:
        put_string(buffer, referent.key, true);
        put_i32(buffer, (int32_t)referent.mode);
        break;
    case MARKDOWN_CORE_REFERENT_FOOTNOTE:
    case MARKDOWN_CORE_REFERENT_SPECIMEN:
        put_string(buffer, referent.id, true);
        break;
    default:
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
    push_action(buffer, stack, suffix);
    schedule_chain(buffer, stack, markdown_core_citation_prefix(action.citation));
}

static void write_attributes(jni_payload_buffer *buffer, const markdown_core_attribute_value *attributes) {
    put_optional_string(buffer, markdown_core_attribute_value_anchor(attributes));
    size_t classes = markdown_core_attribute_value_class_count(attributes),
           records = markdown_core_attribute_value_record_count(attributes);
    if (classes > INT32_MAX || records > INT32_MAX) {
        buffer->failure = JNI_PAYLOAD_ALLOCATION;
        return;
    }
    put_i32(buffer, (int32_t)classes);
    for (size_t i = 0; i < classes; i++) {
        markdown_core_string value;
        if (!markdown_core_attribute_value_class_at(attributes, i, &value)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, value, true);
    }
    put_i32(buffer, (int32_t)records);
    for (size_t i = 0; i < records; i++) {
        markdown_core_string name, value;
        if (!markdown_core_attribute_value_record_at(attributes, i, &name, &value)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, name, true);
        put_string(buffer, value, true);
    }
}

static void write_metadata_value(jni_payload_buffer *buffer, const markdown_core_metadata_value *record) {
    put_u8(buffer, record ? 1 : 0);
    if (!record) {
        return;
    }
    markdown_core_metadata_value_kind kind = markdown_core_metadata_value_get_kind(record);
    put_u8(buffer, (uint8_t)kind);
    if (kind == MARKDOWN_CORE_METADATA_SCALAR) {
        markdown_core_metadata_scalar value;
        if (!markdown_core_metadata_value_scalar(record, &value)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
    } else if (kind == MARKDOWN_CORE_METADATA_LIST) {
        size_t items = markdown_core_metadata_value_item_count(record);
        if (items > INT32_MAX) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, (int32_t)items);
        for (size_t j = 0; j < items; j++) {
            markdown_core_metadata_list_item item;
            if (!markdown_core_metadata_value_item_at(record, j, &item)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                return;
            }
            put_u8(buffer, (uint8_t)item.kind);
            put_string(buffer, item.value, true);
        }
    } else {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        return;
    }
}
static void write_metadata(jni_payload_buffer *buffer, const markdown_core_metadata *metadata) {
    put_u8(buffer, metadata ? 1 : 0);
    if (!metadata) {
        return;
    }
    put_scope(buffer, markdown_core_metadata_scope(metadata));
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

static void put_dimensions(jni_payload_buffer *buffer, const markdown_core_dimensions *dimensions) {
    if (dimensions &&
        (dimensions->width < 1 ||
         (dimensions->height.has_value && (dimensions->height.value < 1 || dimensions->height.value > INT32_MAX)))) {
        buffer->failure = JNI_PAYLOAD_INTERNAL;
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

static void write_node(jni_payload_buffer *buffer, jni_payload_stack *stack, jni_payload_resources *resources,
                       const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {0};
    markdown_core_string second = {0};
    markdown_core_string third = {0};
    markdown_core_optional_string optional_first = {0};
    markdown_core_optional_string optional_second = {0};

    put_u8(buffer, (uint8_t)kind);
    put_scope(buffer, markdown_core_node_scope(node));
    write_attributes(buffer, markdown_core_node_primary_attributes(node));
    if (buffer->failure != JNI_PAYLOAD_OK) {
        return;
    }

    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT: {
        /* The metadata leads, then the title -- a node-valued list sent before
         * the content, as the walk visits it, whose count is its presence
         * because a present title holds at least one node -- then the
         * content. */
        markdown_core_optional_bool collapsed;
        const markdown_core_node *title;
        const markdown_core_node *cursor;
        size_t count = 0;
        if (!markdown_core_node_callout_properties(node, &optional_first, &collapsed)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        put_u8(buffer, collapsed.has_value ? (collapsed.value ? 1 : 0) : UINT8_MAX);
        title = markdown_core_node_callout_title(node);
        for (cursor = title; cursor; cursor = markdown_core_node_get_next_sibling(cursor)) {
            count++;
        }
        if (title != NULL) {
            jni_payload_action children = {.kind = JNI_PAYLOAD_WRITE_CHILDREN, .node = node};
            push_action(buffer, stack, children);
            schedule_nodes(buffer, stack, title, count);
        } else {
            schedule_nodes(buffer, stack, NULL, 0);
            schedule_children(buffer, stack, node);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_DOCUMENT: {
        write_metadata(buffer, markdown_core_node_document_metadata(node));
        /* The content leads, as the walk visits it; the footnotes follow. */
        jni_payload_action footnotes = {.kind = JNI_PAYLOAD_WRITE_FOOTNOTES, .node = node};
        jni_payload_action specimens = {.kind = JNI_PAYLOAD_WRITE_SPECIMENS, .node = node};
        push_action(buffer, stack, specimens);
        push_action(buffer, stack, footnotes);
        schedule_children(buffer, stack, node);
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
    case MARKDOWN_CORE_KIND_TABLE_ROW:
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        if (!markdown_core_node_heading_level(node, &level)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, (int32_t)mode);
        put_string(buffer, first, true);
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: {
        markdown_core_placement_mode mode;
        if (!markdown_core_node_formula_properties(node, &mode, &first) || mode != MARKDOWN_CORE_PLACEMENT_STANDALONE) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count, head, content, foot;
        if (!markdown_core_node_table_properties(node, &count, &head, &content, &foot)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        if (count > INT32_MAX || head > INT32_MAX || content > INT32_MAX || foot > INT32_MAX) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, (int32_t)count);
        for (size_t index = 0; index < count; ++index) {
            markdown_core_table_column column;
            if (!markdown_core_node_table_column_at(node, index, &column)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                return;
            }
            put_u8(buffer, (uint8_t)column.alignment);
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
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_DEFINITION: {
        bool compact;
        if (!markdown_core_node_definition_compact(node, &compact)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, compact ? 1 : 0);
        jni_payload_action bodies = {.kind = JNI_PAYLOAD_WRITE_DEFINITION_BODIES,
                                     .body = markdown_core_node_definition_bodies(node)};
        push_action(buffer, stack, bodies);
        schedule_chain(buffer, stack, markdown_core_node_definition_term(node));
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* A label is a node-valued field, not directive content. Preserve that
         * boundary on the wire instead of flattening it into the child list. */
        if (!markdown_core_node_directive_properties(node, &optional_first)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_optional_string(buffer, optional_first);
        const markdown_core_node *label = markdown_core_node_directive_label(node);
        put_u8(buffer, label ? 1 : 0);
        if (label != NULL) {
            jni_payload_action children = {.kind = JNI_PAYLOAD_WRITE_CHILDREN, .node = node};
            jni_payload_action label_node = {.kind = JNI_PAYLOAD_WRITE_NODE, .node = label};
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
        /* The items are values the cite owns, not children. */
        write_citations(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: {
        markdown_core_destination destination;
        if (!markdown_core_node_destination(node, &destination)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
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
    case MARKDOWN_CORE_KIND_MEDIA: {
        /* The resource's ordinal leads. Only its first sight carries the
         * destination and title; a later occurrence names the ordinal and
         * nothing else, so the decoder materializes each resource once. */
        markdown_core_destination destination;
        const markdown_core_resource *resource = markdown_core_node_resource(node);
        bool first_sight = false;
        int32_t ordinal;
        if (resource == NULL) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        ordinal = resource_ordinal(buffer, resources, resource, &first_sight);
        if (buffer->failure != JNI_PAYLOAD_OK) {
            return;
        }
        put_i32(buffer, ordinal);
        if (first_sight) {
            if (!markdown_core_node_destination(node, &destination) ||
                !markdown_core_node_title(node, &optional_first)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
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
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                return;
            }
            put_optional_string(buffer, optional_first);
            write_attributes(buffer, markdown_core_node_inherited_attributes(node));
        }
        if (kind == MARKDOWN_CORE_KIND_MEDIA) {
            put_dimensions(buffer, markdown_core_node_dimensions(node));
        }
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_CELL: {
        int64_t rowspan, colspan;
        if (!markdown_core_node_table_cell_spans(node, &rowspan, &colspan)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_i64(buffer, rowspan);
        put_i64(buffer, colspan);
        schedule_children(buffer, stack, node);
        break;
    }

    default:
        buffer->failure = JNI_PAYLOAD_INTERNAL;
        break;
    }
}

static void write_tree(jni_payload_buffer *buffer, const markdown_core_node *root) {
    jni_payload_stack stack = {0};
    jni_payload_resources resources = {0};
    jni_payload_action root_action = {.kind = JNI_PAYLOAD_WRITE_NODE, .node = root};
    push_action(buffer, &stack, root_action);
    while (stack.count != 0 && buffer->failure == JNI_PAYLOAD_OK) {
        jni_payload_action action = stack.actions[--stack.count];
        switch (action.kind) {
        case JNI_PAYLOAD_WRITE_NODE:
            if (action.node == NULL) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
            } else {
                write_node(buffer, &stack, &resources, action.node);
            }
            break;
        case JNI_PAYLOAD_WRITE_SIBLINGS: {
            const markdown_core_node *next;
            jni_payload_action node_action = {0};
            if (action.node == NULL || action.remaining == 0) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                break;
            }
            next = markdown_core_node_get_next_sibling(action.node);
            if ((action.remaining == 1) != (next == NULL)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                break;
            }
            if (action.remaining > 1) {
                jni_payload_action siblings = {
                    .kind = JNI_PAYLOAD_WRITE_SIBLINGS, .node = next, .remaining = action.remaining - 1};
                push_action(buffer, &stack, siblings);
            }
            node_action.kind = JNI_PAYLOAD_WRITE_NODE;
            node_action.node = action.node;
            node_action.remaining = 0;
            push_action(buffer, &stack, node_action);
            break;
        }
        case JNI_PAYLOAD_WRITE_CHILDREN:
            schedule_children(buffer, &stack, action.node);
            break;
        case JNI_PAYLOAD_WRITE_SPECIMENS:
            write_specimens(buffer, &stack, action.node);
            break;
        case JNI_PAYLOAD_WRITE_SPECIMEN:
            write_specimen(buffer, &stack, action);
            break;
        case JNI_PAYLOAD_WRITE_FOOTNOTES:
            write_footnotes(buffer, &stack, action.node);
            break;
        case JNI_PAYLOAD_WRITE_FOOTNOTE:
            write_footnote(buffer, &stack, action);
            break;
        case JNI_PAYLOAD_WRITE_CITATION:
            write_citation(buffer, &stack, action);
            break;
        case JNI_PAYLOAD_WRITE_DEFINITION_BODIES: {
            size_t count = 0;
            for (const markdown_core_definition_body *body = action.body; body;
                 body = markdown_core_definition_body_next(body)) {
                count++;
            }
            if (!count || count > INT32_MAX) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                break;
            }
            put_i32(buffer, (int32_t)count);
            action.kind = JNI_PAYLOAD_WRITE_DEFINITION_BODY;
            push_action(buffer, &stack, action);
            break;
        }
        case JNI_PAYLOAD_WRITE_DEFINITION_BODY: {
            const markdown_core_definition_body *body = action.body;
            action.body = markdown_core_definition_body_next(body);
            if (action.body) {
                push_action(buffer, &stack, action);
            }
            schedule_chain(buffer, &stack, markdown_core_definition_body_content(body));
            break;
        }
        case JNI_PAYLOAD_WRITE_SUFFIX:
            schedule_chain(buffer, &stack, markdown_core_citation_suffix(action.citation));
            break;
        }
    }
    free(stack.actions);
    free(resources.slots);
}

bool markdown_core_kotlin_jni_encode(const uint8_t *source, size_t length, uint8_t **output, size_t *output_length) {
    markdown_core_string internal_error = {internal_error_bytes, sizeof(internal_error_bytes) - 1};
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    jni_payload_buffer buffer = {0};
    const markdown_core_node *root;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    document = markdown_core_document_parse(source, length, &error);

    put_bytes(&buffer, jni_payload_magic, sizeof(jni_payload_magic));
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
            buffer.failure = JNI_PAYLOAD_INTERNAL;
        } else {
            write_tree(&buffer, root);
        }
        markdown_core_document_free(document);
    }

    if (buffer.failure == JNI_PAYLOAD_INTERNAL) {
        free(buffer.data);
        memset(&buffer, 0, sizeof(buffer));
        put_bytes(&buffer, jni_payload_magic, sizeof(jni_payload_magic));
        write_error(&buffer, MARKDOWN_CORE_ERROR_INTERNAL, internal_error);
    }
    if (buffer.failure != JNI_PAYLOAD_OK) {
        free(buffer.data);
        return false;
    }
    *output = buffer.data;
    *output_length = buffer.size;
    return true;
}

void markdown_core_kotlin_jni_payload_free(uint8_t *output) { free(output); }
