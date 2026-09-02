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

enum { ES_HEADER_SIZE = 64, ES_NODE_SIZE = 96, ES_ATTRIBUTE_SIZE = 16 };
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
    ES_HEADER_ALIGNMENT_COUNT = 36,
    ES_HEADER_NODES_OFFSET = 40,
    ES_HEADER_EDGES_OFFSET = 44,
    ES_HEADER_ATTRIBUTES_OFFSET = 48,
    ES_HEADER_ALIGNMENTS_OFFSET = 52,
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
    ES_NODE_RESERVED = 48,
    ES_NODE_I64 = 56,
    ES_NODE_STRINGS = 64
};

typedef struct es_source_node {
    const markdown_core_node *node;
    uint32_t child_start;
    uint32_t child_count;
    uint32_t label_index;
    uint32_t aux_start;
    uint32_t aux_count;
    uint32_t flags;
    int32_t scalar0;
    int64_t integer;
    markdown_core_optional_string strings[4];
} es_source_node;

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
    uint8_t *alignments;
    size_t alignment_count;
    size_t alignment_capacity;
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

static uint32_t append_node(es_build *build, const markdown_core_node *node) {
    es_source_node value;
    uint32_t index;
    if (build->failure != ES_BUILD_OK || build->node_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->nodes, &build->node_capacity, build->node_count + 1, sizeof(*build->nodes))) {
        build->failure = ES_BUILD_ALLOCATION;
        return ES_NO_INDEX;
    }
    memset(&value, 0, sizeof(value));
    value.node = node;
    value.label_index = ES_NO_INDEX;
    value.aux_start = ES_NO_INDEX;
    index = (uint32_t)build->node_count;
    build->nodes[build->node_count++] = value;
    return index;
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

static void append_alignment(es_build *build, markdown_core_table_alignment alignment) {
    if (build->failure != ES_BUILD_OK || build->alignment_count >= UINT32_MAX ||
        !reserve_vector((void **)&build->alignments, &build->alignment_capacity, build->alignment_count + 1,
                        sizeof(*build->alignments))) {
        build->failure = ES_BUILD_ALLOCATION;
        return;
    }
    build->alignments[build->alignment_count++] = (uint8_t)alignment;
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

/* Breadth-first indexes guarantee that every relation points forward. The JS
 * decoder can consequently build records in reverse order without recursion. */
static void collect_topology(es_build *build, const markdown_core_node *root) {
    size_t cursor;
    append_node(build, root);
    for (cursor = 0; cursor < build->node_count && build->failure == ES_BUILD_OK; ++cursor) {
        const markdown_core_node *node = build->nodes[cursor].node;
        markdown_core_node_kind kind = markdown_core_node_get_kind(node);
        const markdown_core_node *child;
        size_t count;
        size_t index;

        if (kind == MARKDOWN_CORE_KIND_NONE) {
            build->failure = ES_BUILD_INTERNAL;
            break;
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
    }
}

static void collect_node_fields(es_build *build, size_t node_index) {
    es_source_node *record = &build->nodes[node_index];
    const markdown_core_node *node = record->node;
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {0};
    markdown_core_string second = {0};
    markdown_core_string third = {0};
    markdown_core_optional_string optional_first = {0};
    markdown_core_optional_string optional_second = {0};

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        if (!markdown_core_node_heading_level(node, &record->scalar0)) {
            build->failure = ES_BUILD_INTERNAL;
        }
        break;
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor;
        markdown_core_optional_i64 start;
        bool tight = false;
        if (!markdown_core_node_list_properties(node, &flavor, &start, &tight)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->scalar0 = (int32_t)flavor;
        record->integer = start.value;
        record->flags = (start.has_value ? 1u : 0u) | (tight ? 2u : 0u);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked;
        if (!markdown_core_node_list_item_checked(node, &checked)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->scalar0 = checked.has_value ? (checked.value ? 1 : 0) : -1;
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
        size_t count = 0;
        size_t index;
        if (!markdown_core_node_table_column_count(node, &count) || count > UINT32_MAX ||
            build->alignment_count > UINT32_MAX - count) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->aux_start = (uint32_t)build->alignment_count;
        record->aux_count = (uint32_t)count;
        for (index = 0; index < count; ++index) {
            markdown_core_table_alignment alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
            if (!markdown_core_node_table_alignment_at(node, index, &alignment)) {
                build->failure = ES_BUILD_INTERNAL;
                break;
            }
            append_alignment(build, alignment);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        bool has_attributes = false;
        size_t count = 0;
        size_t index;
        if (!markdown_core_node_directive_properties(node, &first, &has_attributes, &count) || count > UINT32_MAX ||
            build->attribute_count > UINT32_MAX - count) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->flags = has_attributes ? 1u : 0u;
        record->aux_start = (uint32_t)build->attribute_count;
        record->aux_count = has_attributes ? (uint32_t)count : 0;
        for (index = 0; has_attributes && index < count; ++index) {
            if (!markdown_core_node_directive_attribute_at(node, index, &first, &second)) {
                build->failure = ES_BUILD_INTERNAL;
                break;
            }
            append_attribute(build, first, second);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        if (!markdown_core_node_association(node, &first, &second)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->strings[1] = required_string(second);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        if (!markdown_core_node_association(node, &first, &second) ||
            !markdown_core_node_definition_resource(node, &third, &optional_first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->strings[1] = required_string(second);
        record->strings[2] = required_string(third);
        record->strings[3] = optional_first;
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        if (!markdown_core_node_association(node, &first, &second) || !markdown_core_node_reference_form(node, &form)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->strings[1] = required_string(second);
        record->scalar0 = (int32_t)form;
        break;
    }
    case MARKDOWN_CORE_KIND_LINK:
        if (!markdown_core_node_link_properties(node, &first, &optional_first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->strings[1] = optional_first;
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        if (!markdown_core_node_image_properties(node, &first, &optional_first)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->strings[0] = required_string(first);
        record->strings[1] = optional_first;
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        if (!markdown_core_node_table_row_is_header(node, &header)) {
            build->failure = ES_BUILD_INTERNAL;
            break;
        }
        record->flags = header ? 1u : 0u;
        break;
    }
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
    free(build->alignments);
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
    size_t alignments_offset;
    size_t strings_offset;
    size_t total_size;
    size_t string_cursor;
    size_t index;
    uint8_t *output;

    *failure = ES_BUILD_OK;
    if (!section_end(nodes_offset, build->node_count, ES_NODE_SIZE, &edges_offset) ||
        !section_end(edges_offset, build->edge_count, sizeof(uint32_t), &attributes_offset) ||
        !section_end(attributes_offset, build->attribute_count, ES_ATTRIBUTE_SIZE, &alignments_offset) ||
        !section_end(alignments_offset, build->alignment_count, sizeof(uint8_t), &strings_offset)) {
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
    put_u32(output, ES_HEADER_ALIGNMENT_COUNT, (uint32_t)build->alignment_count);
    put_u32(output, ES_HEADER_NODES_OFFSET, (uint32_t)nodes_offset);
    put_u32(output, ES_HEADER_EDGES_OFFSET, (uint32_t)edges_offset);
    put_u32(output, ES_HEADER_ATTRIBUTES_OFFSET, (uint32_t)attributes_offset);
    put_u32(output, ES_HEADER_ALIGNMENTS_OFFSET, (uint32_t)alignments_offset);
    put_u32(output, ES_HEADER_STRINGS_OFFSET, (uint32_t)strings_offset);
    put_u32(output, ES_HEADER_STRINGS_LENGTH, (uint32_t)build->strings_length);

    string_cursor = strings_offset;
    for (index = 0; index < build->node_count; ++index) {
        const es_source_node *source = &build->nodes[index];
        markdown_core_scope scope = markdown_core_node_scope(source->node);
        size_t node_offset = nodes_offset + index * ES_NODE_SIZE;
        size_t string_index;
        put_u32(output, node_offset + ES_NODE_KIND, (uint32_t)markdown_core_node_get_kind(source->node));
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
        memset(output + node_offset + ES_NODE_RESERVED, 0, ES_NODE_I64 - ES_NODE_RESERVED);
        put_i64(output, node_offset + ES_NODE_I64, source->integer);
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
    if (build->alignment_count != 0) {
        memcpy(output + alignments_offset, build->alignments, build->alignment_count);
    }
    if (string_cursor != total_size) {
        free(output);
        *failure = ES_BUILD_INTERNAL;
        return NULL;
    }
    return output;
}

static void apply_options(markdown_core_parse_options *options, uint32_t flags) {
    markdown_core_parse_options_init(options);
    options->smart_punctuation = (flags & (1u << 0)) != 0;
    options->footnotes = (flags & (1u << 1)) != 0;
    options->strip_html_comments = (flags & (1u << 2)) != 0;
    options->tables = (flags & (1u << 3)) != 0;
    options->strikethrough = (flags & (1u << 4)) != 0;
    options->autolinks = (flags & (1u << 5)) != 0;
    options->task_lists = (flags & (1u << 6)) != 0;
    options->formulas = (flags & (1u << 7)) != 0;
    options->directives = (flags & (1u << 8)) != 0;
}

uint8_t *es_parse(const uint8_t *source, size_t length, uint32_t flags) {
    static const uint8_t internal_message_bytes[] = "could not produce AST result";
    markdown_core_string internal_message = {internal_message_bytes, sizeof(internal_message_bytes) - 1};
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    const markdown_core_node *root;
    es_build build = {0};
    uint8_t *output = NULL;

    apply_options(&options, flags);
    document = markdown_core_document_parse(source, length, &options, &error);
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
