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
    JNI_PAYLOAD_WRITE_CHILDREN
} jni_payload_action_kind;

typedef struct jni_payload_action {
    jni_payload_action_kind kind;
    const markdown_core_node *node;
    size_t remaining;
} jni_payload_action;

typedef struct jni_payload_stack {
    jni_payload_action *actions;
    size_t count;
    size_t capacity;
} jni_payload_stack;

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
        jni_payload_action action = {JNI_PAYLOAD_WRITE_SIBLINGS, node, count};
        push_action(buffer, stack, action);
    }
}

static void schedule_children(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *node) {
    schedule_nodes(buffer, stack, markdown_core_node_get_first_child(node), markdown_core_node_child_count(node));
}

static void write_node(jni_payload_buffer *buffer, jni_payload_stack *stack, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {0};
    markdown_core_string second = {0};
    markdown_core_string third = {0};
    markdown_core_optional_string optional_first = {0};
    markdown_core_optional_string optional_second = {0};

    put_u8(buffer, (uint8_t)kind);
    put_scope(buffer, markdown_core_node_scope(node));
    if (buffer->failure != JNI_PAYLOAD_OK) {
        return;
    }

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
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
        markdown_core_optional_i64 start;
        bool tight = false;
        if (!markdown_core_node_list_properties(node, &flavor, &start, &tight)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_i32(buffer, (int32_t)flavor);
        put_i64(buffer, start.value);
        put_u8(buffer, start.has_value ? 1 : 0);
        put_u8(buffer, tight ? 1 : 0);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked;
        if (!markdown_core_node_list_item_checked(node, &checked)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, checked.has_value ? (checked.value ? 1 : 0) : UINT8_MAX);
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
        size_t count = 0;
        size_t index;
        if (!markdown_core_node_table_column_count(node, &count)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        if (count > INT32_MAX) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, (int32_t)count);
        for (index = 0; index < count; ++index) {
            markdown_core_table_alignment alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
            if (!markdown_core_node_table_alignment_at(node, index, &alignment)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                return;
            }
            put_u8(buffer, (uint8_t)alignment);
        }
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* A label is a node-valued field, not directive content. Preserve that
         * boundary on the wire instead of flattening it into the child list. */
        bool has_attributes = false;
        const markdown_core_node *label;
        size_t count = 0;
        size_t index;
        if (!markdown_core_node_directive_properties(node, &first, &has_attributes, &count)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_u8(buffer, has_attributes ? 1 : 0);
        if (count > INT32_MAX) {
            buffer->failure = JNI_PAYLOAD_ALLOCATION;
            return;
        }
        put_i32(buffer, has_attributes ? (int32_t)count : 0);
        for (index = 0; has_attributes && index < count; ++index) {
            if (!markdown_core_node_directive_attribute_at(node, index, &first, &second)) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
                return;
            }
            put_string(buffer, first, true);
            put_string(buffer, second, true);
        }
        label = markdown_core_node_directive_label(node);
        put_u8(buffer, label ? 1 : 0);
        if (label != NULL) {
            jni_payload_action children = {JNI_PAYLOAD_WRITE_CHILDREN, node, 0};
            jni_payload_action label_node = {JNI_PAYLOAD_WRITE_NODE, label, 0};
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
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
        if (!markdown_core_node_association(node, &first, &second)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_string(buffer, second, true);
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        if (!markdown_core_node_association(node, &first, &second)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_string(buffer, second, true);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        if (!markdown_core_node_association(node, &first, &second) ||
            !markdown_core_node_definition_resource(node, &third, &optional_first)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_string(buffer, second, true);
        put_string(buffer, third, true);
        put_optional_string(buffer, optional_first);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        if (!markdown_core_node_association(node, &first, &second) || !markdown_core_node_reference_form(node, &form)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_string(buffer, second, true);
        put_i32(buffer, (int32_t)form);
        schedule_children(buffer, stack, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LINK:
        if (!markdown_core_node_link_properties(node, &first, &optional_first)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_optional_string(buffer, optional_first);
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        if (!markdown_core_node_image_properties(node, &first, &optional_first)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_string(buffer, first, true);
        put_optional_string(buffer, optional_first);
        schedule_children(buffer, stack, node);
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        if (!markdown_core_node_table_row_is_header(node, &header)) {
            buffer->failure = JNI_PAYLOAD_INTERNAL;
            return;
        }
        put_u8(buffer, header ? 1 : 0);
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
    jni_payload_action root_action = {JNI_PAYLOAD_WRITE_NODE, root, 0};
    push_action(buffer, &stack, root_action);
    while (stack.count != 0 && buffer->failure == JNI_PAYLOAD_OK) {
        jni_payload_action action = stack.actions[--stack.count];
        switch (action.kind) {
        case JNI_PAYLOAD_WRITE_NODE:
            if (action.node == NULL) {
                buffer->failure = JNI_PAYLOAD_INTERNAL;
            } else {
                write_node(buffer, &stack, action.node);
            }
            break;
        case JNI_PAYLOAD_WRITE_SIBLINGS: {
            const markdown_core_node *next;
            jni_payload_action node_action;
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
                jni_payload_action siblings = {JNI_PAYLOAD_WRITE_SIBLINGS, next, action.remaining - 1};
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
        }
    }
    free(stack.actions);
}

static void apply_options(markdown_core_parse_options *options, uint32_t mask) {
    options->smart_punctuation = (mask & (1u << 0)) != 0;
    options->footnotes = (mask & (1u << 1)) != 0;
    options->strip_html_comments = (mask & (1u << 2)) != 0;
    options->tables = (mask & (1u << 3)) != 0;
    options->strikethrough = (mask & (1u << 4)) != 0;
    options->autolinks = (mask & (1u << 5)) != 0;
    options->task_lists = (mask & (1u << 6)) != 0;
    options->formulas = (mask & (1u << 7)) != 0;
    options->directives = (mask & (1u << 8)) != 0;
}

bool markdown_core_kotlin_jni_encode(const uint8_t *source, size_t length, uint32_t options_mask, uint8_t **output,
                                     size_t *output_length) {
    markdown_core_string internal_error = {internal_error_bytes, sizeof(internal_error_bytes) - 1};
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    jni_payload_buffer buffer = {0};
    const markdown_core_node *root;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);
    document = markdown_core_document_parse(source, length, &options, &error);

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
