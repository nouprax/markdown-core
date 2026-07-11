#include "markdown_core_kotlin_bridge.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct bridge_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} bridge_buffer;

static void reserve(bridge_buffer *buffer, size_t additional) {
    size_t required;
    size_t capacity;
    uint8_t *data;
    if (buffer->failed || additional > SIZE_MAX - buffer->size) {
        buffer->failed = true;
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
        buffer->failed = true;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void put_bytes(bridge_buffer *buffer, const uint8_t *bytes, size_t length) {
    reserve(buffer, length);
    if (!buffer->failed && length != 0) {
        memcpy(buffer->data + buffer->size, bytes, length);
        buffer->size += length;
    }
}

static void put_u8(bridge_buffer *buffer, uint8_t value) { put_bytes(buffer, &value, 1); }

static void put_i32(bridge_buffer *buffer, int32_t value) {
    uint32_t bits = (uint32_t)value;
    size_t index;
    for (index = 0; index < 4; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_i64(bridge_buffer *buffer, int64_t value) {
    uint64_t bits = (uint64_t)value;
    size_t index;
    for (index = 0; index < 8; ++index) {
        put_u8(buffer, (uint8_t)(bits >> (index * 8)));
    }
}

static void put_scope(bridge_buffer *buffer, markdown_core_scope scope) {
    put_i32(buffer, scope.start.line);
    put_i32(buffer, scope.start.column);
    put_i32(buffer, scope.end.line);
    put_i32(buffer, scope.end.column);
}

static void put_string(bridge_buffer *buffer, markdown_core_string_view value, bool present) {
    if (!present) {
        put_i32(buffer, -1);
        return;
    }
    if (value.length > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)value.length);
    put_bytes(buffer, value.data, value.length);
}

static void write_node(bridge_buffer *buffer, const markdown_core_node *node);

static void write_nodes(bridge_buffer *buffer, const markdown_core_node *node, size_t count) {
    size_t index;
    if (count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)count);
    for (index = 0; index < count; ++index) {
        if (node == NULL) {
            buffer->failed = true;
            return;
        }
        write_node(buffer, node);
        node = markdown_core_node_get_next_sibling(node);
    }
}

static void write_children(bridge_buffer *buffer, const markdown_core_node *node) {
    write_nodes(buffer, markdown_core_node_get_first_child(node), markdown_core_node_child_count(node));
}

static void write_node(bridge_buffer *buffer, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string_view first = {0};
    markdown_core_string_view second = {0};
    markdown_core_string_view third = {0};

    put_u8(buffer, (uint8_t)kind);
    put_scope(buffer, markdown_core_node_scope(node));

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
        write_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        markdown_core_node_heading_level(node, &level);
        put_i32(buffer, level);
        write_children(buffer, node);
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
        markdown_core_node_list_properties(node, &flavor, &start, &tight);
        put_i32(buffer, (int32_t)flavor);
        put_i64(buffer, start.value);
        put_u8(buffer, start.has_value ? 1 : 0);
        put_u8(buffer, tight ? 1 : 0);
        write_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked;
        markdown_core_node_list_item_checked(node, &checked);
        put_u8(buffer, checked.has_value ? (checked.value ? 1 : 0) : UINT8_MAX);
        write_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced = false;
        bool closed = false;
        markdown_core_node_code_block_properties(node, &first, &second, &third, &fenced, &closed);
        put_string(buffer, first, first.data != NULL);
        put_string(buffer, second, second.data != NULL);
        put_string(buffer, third, true);
        put_u8(buffer, fenced ? 1 : 0);
        put_u8(buffer, closed ? 1 : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
        markdown_core_node_literal(node, &first);
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode;
        markdown_core_node_formula_properties(node, &mode, &first);
        put_i32(buffer, (int32_t)mode);
        put_string(buffer, first, true);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count = 0;
        size_t index;
        markdown_core_node_table_column_count(node, &count);
        if (count > INT32_MAX) {
            buffer->failed = true;
            return;
        }
        put_i32(buffer, (int32_t)count);
        for (index = 0; index < count; ++index) {
            markdown_core_table_alignment alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
            markdown_core_node_table_alignment_at(node, index, &alignment);
            put_u8(buffer, (uint8_t)alignment);
        }
        write_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        markdown_core_placement_mode mode;
        bool has_label = false;
        size_t label_count = 0;
        const markdown_core_node *content;
        size_t content_count = 0;
        markdown_core_node_directive_properties(node, &mode, &first, &second, &has_label, &label_count);
        put_i32(buffer, (int32_t)mode);
        put_string(buffer, first, true);
        put_string(buffer, second, second.data != NULL);
        if (has_label) {
            write_nodes(buffer, markdown_core_node_directive_first_label_child(node), label_count);
        } else {
            put_i32(buffer, -1);
        }
        content = markdown_core_node_directive_first_content_child(node);
        for (const markdown_core_node *cursor = content; cursor != NULL;
             cursor = markdown_core_node_get_next_sibling(cursor)) {
            ++content_count;
        }
        write_nodes(buffer, content, content_count);
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
        markdown_core_node_footnote_id(node, &first);
        put_string(buffer, first, true);
        write_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(node, &first);
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &first, &second);
        put_string(buffer, first, first.data != NULL);
        put_string(buffer, second, second.data != NULL);
        write_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &first, &second);
        put_string(buffer, first, first.data != NULL);
        put_string(buffer, second, second.data != NULL);
        write_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        markdown_core_node_table_row_is_header(node, &header);
        put_u8(buffer, header ? 1 : 0);
        write_children(buffer, node);
        break;
    }
    default:
        buffer->failed = true;
        break;
    }
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
    options->dollar_formula_delimiters = (mask & (1u << 8)) != 0;
    options->latex_formula_delimiters = (mask & (1u << 9)) != 0;
    options->directives = (mask & (1u << 10)) != 0;
}

bool markdown_core_kotlin_parse(const uint8_t *source, size_t length, uint32_t options_mask, uint8_t **output,
                                size_t *output_length) {
    static const uint8_t magic[] = {'M', 'K', 'C', '2'};
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    bridge_buffer buffer = {0};
    const markdown_core_node *root;

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);
    document = markdown_core_document_parse(source, length, &options, &error);

    put_bytes(&buffer, magic, sizeof(magic));
    if (document == NULL) {
        markdown_core_scope scope;
        bool has_scope = error != NULL && markdown_core_error_get_scope(error, &scope);
        put_u8(&buffer, 1);
        put_i32(&buffer, error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error));
        if (error == NULL) {
            markdown_core_string_view fallback = {(const uint8_t *)"markdown parsing failed", 23};
            put_string(&buffer, fallback, true);
        } else {
            put_string(&buffer, markdown_core_error_get_message(error), true);
        }
        put_u8(&buffer, has_scope ? 1 : 0);
        if (has_scope) {
            put_scope(&buffer, scope);
        }
        markdown_core_error_free(error);
    } else {
        put_u8(&buffer, 0);
        root = markdown_core_document_root(document);
        if (root == NULL) {
            buffer.failed = true;
        } else {
            write_node(&buffer, root);
        }
        markdown_core_document_free(document);
    }

    if (buffer.failed) {
        free(buffer.data);
        return false;
    }
    *output = buffer.data;
    *output_length = buffer.size;
    return true;
}

void markdown_core_kotlin_free(uint8_t *output) { free(output); }
