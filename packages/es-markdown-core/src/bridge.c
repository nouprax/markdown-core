#include <stdint.h>
#include <stddef.h>

#include "markdown_core.h"

enum es_string_field {
    ES_STRING_CODE_INFO = 1,
    ES_STRING_CODE_LANGUAGE,
    ES_STRING_CODE_LITERAL,
    ES_STRING_LITERAL,
    ES_STRING_FORMULA_LITERAL,
    ES_STRING_DIRECTIVE_NAME,
    ES_STRING_DIRECTIVE_ATTRIBUTE_NAME,
    ES_STRING_DIRECTIVE_ATTRIBUTE_VALUE,
    ES_STRING_LINK_DESTINATION,
    ES_STRING_LINK_TITLE,
    ES_STRING_IMAGE_SOURCE,
    ES_STRING_IMAGE_TITLE,
    ES_STRING_ERROR_MESSAGE = 14,
    ES_STRING_DEFINITION_DESTINATION,
    ES_STRING_DEFINITION_TITLE,
    ES_STRING_ASSOCIATION_LABEL,
    ES_STRING_ASSOCIATION_IDENTIFIER
};

static bool es_write_string(markdown_core_string value, uintptr_t *data, size_t *length) {
    *data = (uintptr_t)value.data;
    *length = value.length;
    return true;
}

/* PRESENCE IS THE RETURN VALUE, not the pointer. This boundary used to have
 * one channel for two facts -- a null `data` meant both "absent" and "present
 * but the bytes live nowhere" -- and requirement 14 says the two are different
 * answers, so `es_string` now says which it gave. */
static bool es_write_optional_string(markdown_core_optional_string value, uintptr_t *data, size_t *length) {
    if (!value.has_value) {
        *data = 0;
        *length = 0;
        return false;
    }
    return es_write_string(value.value, data, length);
}

markdown_core_document *es_document_parse(const uint8_t *source, size_t length, uint32_t flags,
                                          markdown_core_error **error) {
    markdown_core_parse_options options;
    markdown_core_parse_options_init(&options);
    options.smart_punctuation = (flags & (1u << 0)) != 0;
    options.footnotes = (flags & (1u << 1)) != 0;
    options.strip_html_comments = (flags & (1u << 2)) != 0;
    options.tables = (flags & (1u << 3)) != 0;
    options.strikethrough = (flags & (1u << 4)) != 0;
    options.autolinks = (flags & (1u << 5)) != 0;
    options.task_lists = (flags & (1u << 6)) != 0;
    options.formulas = (flags & (1u << 7)) != 0;
    options.directives = (flags & (1u << 8)) != 0;
    return markdown_core_document_parse(source, length, &options, error);
}

void es_document_free(markdown_core_document *document) { markdown_core_document_free(document); }

const markdown_core_node *es_document_root(const markdown_core_document *document) {
    return markdown_core_document_root(document);
}

int32_t es_error_code(const markdown_core_error *error) { return (int32_t)markdown_core_error_get_code(error); }

void es_error_free(markdown_core_error *error) { markdown_core_error_free(error); }

int32_t es_node_kind(const markdown_core_node *node) { return (int32_t)markdown_core_node_get_kind(node); }

const markdown_core_node *es_node_first_child(const markdown_core_node *node) {
    return markdown_core_node_get_first_child(node);
}

const markdown_core_node *es_node_next_sibling(const markdown_core_node *node) {
    return markdown_core_node_get_next_sibling(node);
}

const markdown_core_node *es_node_directive_label(const markdown_core_node *node) {
    return markdown_core_node_directive_label(node);
}

int32_t es_scope_coordinate(const markdown_core_node *node, int32_t coordinate) {
    markdown_core_scope scope = markdown_core_node_scope(node);
    switch (coordinate) {
    case 0:
        return scope.start.line;
    case 1:
        return scope.start.column;
    case 2:
        return scope.end.line;
    default:
        return scope.end.column;
    }
}

int32_t es_node_heading_level(const markdown_core_node *node) {
    int32_t value = 0;
    markdown_core_node_heading_level(node, &value);
    return value;
}

int32_t es_node_list_flavor(const markdown_core_node *node) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    bool tight;
    markdown_core_node_list_properties(node, &flavor, &start, &tight);
    return (int32_t)flavor;
}

int32_t es_node_list_tight(const markdown_core_node *node) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    bool tight;
    markdown_core_node_list_properties(node, &flavor, &start, &tight);
    return tight;
}

int32_t es_node_list_start_state(const markdown_core_node *node, int64_t *value) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    bool tight;
    markdown_core_node_list_properties(node, &flavor, &start, &tight);
    *value = start.value;
    return start.has_value;
}

int32_t es_node_checked(const markdown_core_node *node) {
    markdown_core_optional_bool checked;
    markdown_core_node_list_item_checked(node, &checked);
    return checked.has_value ? (checked.value ? 1 : 0) : -1;
}

int32_t es_node_code_flag(const markdown_core_node *node, int32_t field) {
    markdown_core_optional_string info, language;
    markdown_core_string literal;
    bool fenced, closed;
    markdown_core_node_code_block_properties(node, &info, &language, &literal, &fenced, &closed);
    return field == 0 ? fenced : closed;
}

int32_t es_node_reference_form(const markdown_core_node *node) {
    markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
    markdown_core_node_reference_form(node, &form);
    return (int32_t)form;
}

int32_t es_node_formula_mode(const markdown_core_node *node) {
    markdown_core_placement_mode mode;
    markdown_core_string literal;
    markdown_core_node_formula_properties(node, &mode, &literal);
    return (int32_t)mode;
}

size_t es_node_table_column_count(const markdown_core_node *node) {
    size_t count = 0;
    markdown_core_node_table_column_count(node, &count);
    return count;
}

int32_t es_node_table_alignment(const markdown_core_node *node, size_t index) {
    markdown_core_table_alignment alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
    markdown_core_node_table_alignment_at(node, index, &alignment);
    return (int32_t)alignment;
}

int32_t es_node_table_row_header(const markdown_core_node *node) {
    bool value = false;
    markdown_core_node_table_row_is_header(node, &value);
    return value;
}

/* -1 when the source wrote no attribute container at all, so an absent one and
 * an empty one stay apart on a single return value. */
int32_t es_node_directive_attribute_count(const markdown_core_node *node) {
    markdown_core_string name;
    bool has_attributes = false;
    size_t count = 0;
    markdown_core_node_directive_properties(node, &name, &has_attributes, &count);
    return has_attributes ? (int32_t)count : -1;
}

/* Set immediately before an attribute read. The bridge is single-threaded --
 * one wasm instance, one call at a time -- so a slot is enough, and it keeps
 * es_string's signature the one every other field already uses. */
static size_t es_attribute_index = 0;

void es_set_attribute_index(int32_t index) { es_attribute_index = index < 0 ? 0 : (size_t)index; }

bool es_string(const void *object, int32_t field, uintptr_t *data, size_t *length) {
    markdown_core_string first = {NULL, 0}, second = {NULL, 0}, third = {NULL, 0};
    markdown_core_optional_string opt_first = {false, {NULL, 0}}, opt_second = {false, {NULL, 0}};
    const markdown_core_node *node = (const markdown_core_node *)object;
    bool first_bool, second_bool;
    markdown_core_placement_mode mode;
    size_t count;
    switch (field) {
    case ES_STRING_CODE_INFO:
    case ES_STRING_CODE_LANGUAGE:
        markdown_core_node_code_block_properties(node, &opt_first, &opt_second, &third, &first_bool, &second_bool);
        return es_write_optional_string(field == ES_STRING_CODE_INFO ? opt_first : opt_second, data, length);
    case ES_STRING_CODE_LITERAL:
        markdown_core_node_code_block_properties(node, &opt_first, &opt_second, &third, &first_bool, &second_bool);
        return es_write_string(third, data, length);
    case ES_STRING_LITERAL:
        markdown_core_node_literal(node, &first);
        break;
    case ES_STRING_FORMULA_LITERAL:
        markdown_core_node_formula_properties(node, &mode, &first);
        break;
    case ES_STRING_DIRECTIVE_NAME:
        markdown_core_node_directive_properties(node, &first, &first_bool, &count);
        break;
    case ES_STRING_DIRECTIVE_ATTRIBUTE_NAME:
    case ES_STRING_DIRECTIVE_ATTRIBUTE_VALUE:
        /* The index rides in `es_attribute_index`: `es_string` takes a field
         * and an object, and an attribute needs one more number than that. */
        markdown_core_node_directive_attribute_at(node, es_attribute_index, &first, &second);
        first = field == ES_STRING_DIRECTIVE_ATTRIBUTE_NAME ? first : second;
        break;
    case ES_STRING_LINK_DESTINATION:
        markdown_core_node_link_properties(node, &first, &opt_first);
        break;
    case ES_STRING_LINK_TITLE:
        markdown_core_node_link_properties(node, &first, &opt_first);
        return es_write_optional_string(opt_first, data, length);
    case ES_STRING_IMAGE_SOURCE:
        markdown_core_node_image_properties(node, &first, &opt_first);
        break;
    case ES_STRING_IMAGE_TITLE:
        markdown_core_node_image_properties(node, &first, &opt_first);
        return es_write_optional_string(opt_first, data, length);
    case ES_STRING_DEFINITION_DESTINATION:
        markdown_core_node_definition_resource(node, &first, &opt_first);
        break;
    case ES_STRING_DEFINITION_TITLE:
        markdown_core_node_definition_resource(node, &first, &opt_first);
        return es_write_optional_string(opt_first, data, length);
    case ES_STRING_ASSOCIATION_LABEL:
    case ES_STRING_ASSOCIATION_IDENTIFIER:
        markdown_core_node_association(node, &first, &second);
        first = field == ES_STRING_ASSOCIATION_LABEL ? first : second;
        break;
    case ES_STRING_ERROR_MESSAGE:
        first = markdown_core_error_get_message((const markdown_core_error *)object);
        break;
    default:
        break;
    }
    return es_write_string(first, data, length);
}
