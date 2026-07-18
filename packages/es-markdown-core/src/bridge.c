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
    ES_STRING_DIRECTIVE_ATTRIBUTES,
    ES_STRING_LINK_DESTINATION,
    ES_STRING_LINK_TITLE,
    ES_STRING_IMAGE_SOURCE,
    ES_STRING_IMAGE_TITLE,
    ES_STRING_FOOTNOTE_ID,
    ES_STRING_ERROR_MESSAGE
};

static void es_write_view(markdown_core_string_view view, uintptr_t *data, size_t *length) {
    *data = (uintptr_t)view.data;
    *length = view.length;
}

markdown_core_session *es_session_open(uint32_t flags, markdown_core_error **error) {
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
    options.dollar_formula_delimiters = (flags & (1u << 8)) != 0;
    options.latex_formula_delimiters = (flags & (1u << 9)) != 0;
    options.directives = (flags & (1u << 10)) != 0;
    return markdown_core_session_open(&options, error);
}

void es_session_free(markdown_core_session *session) { markdown_core_session_free(session); }

int32_t es_session_edit(
    markdown_core_session *session,
    size_t byte_start,
    size_t byte_end,
    const uint8_t *bytes,
    size_t length,
    markdown_core_error **error
) {
    return markdown_core_session_edit(session, byte_start, byte_end, bytes, length, error);
}

int32_t
es_session_commit(markdown_core_session *session, markdown_core_delta **changes, markdown_core_error **error) {
    return markdown_core_session_commit(session, changes, error);
}

const markdown_core_document *es_session_document(const markdown_core_session *session) {
    return markdown_core_session_document(session);
}

uint64_t es_session_revision(const markdown_core_session *session) { return markdown_core_session_revision(session); }

uint64_t es_session_lineage(const markdown_core_session *session) { return markdown_core_session_lineage(session); }

size_t es_session_length(const markdown_core_session *session) { return markdown_core_session_length(session); }

int32_t es_session_footnote_info(const markdown_core_session *session, uint64_t id, uint64_t *fields) {
    markdown_core_footnote_info info;
    if (!markdown_core_session_footnote_info(session, id, &info)) {
        return 0;
    }
    fields[0] = info.definition;
    fields[1] = info.number;
    fields[2] = info.reference_ordinal;
    fields[3] = info.reference_count;
    return 1;
}

size_t es_session_footnotes(const markdown_core_session *session, uintptr_t *data) {
    const markdown_core_node_id *ids = NULL;
    size_t count = markdown_core_session_footnotes(session, &ids);
    *data = (uintptr_t)ids;
    return count;
}

size_t es_session_footnote_references(const markdown_core_session *session, uint64_t definition, uintptr_t *data) {
    const markdown_core_node_id *ids = NULL;
    size_t count = markdown_core_session_footnote_references(session, definition, &ids);
    *data = (uintptr_t)ids;
    return count;
}

uint64_t es_delta_revision(const markdown_core_delta *changes, int32_t boundary) {
    uint64_t before = 0;
    uint64_t after = 0;
    markdown_core_delta_revisions(changes, &before, &after);
    return boundary == 0 ? before : after;
}

size_t es_delta_ids(const markdown_core_delta *changes, int32_t verdict, uintptr_t *data) {
    const markdown_core_node_id *ids = NULL;
    size_t count = 0;
    switch (verdict) {
    case 0:
        count = markdown_core_delta_added(changes, &ids);
        break;
    case 1:
        count = markdown_core_delta_removed(changes, &ids);
        break;
    case 2:
        count = markdown_core_delta_changed(changes, &ids);
        break;
    default:
        count = markdown_core_delta_bubbled(changes, &ids);
        break;
    }
    *data = (uintptr_t)ids;
    return count;
}

void es_delta_free(markdown_core_delta *changes) { markdown_core_delta_free(changes); }

const markdown_core_node *es_document_root(const markdown_core_document *document) {
    return markdown_core_document_root(document);
}

uint64_t es_node_id(const markdown_core_node *node) { return markdown_core_node_get_id(node); }

uint64_t es_node_revision(const markdown_core_node *node) { return markdown_core_node_get_revision(node); }

int32_t es_error_code(const markdown_core_error *error) {
    return (int32_t)markdown_core_error_get_code(error);
}

void es_error_free(markdown_core_error *error) { markdown_core_error_free(error); }

int32_t es_node_kind(const markdown_core_node *node) {
    return (int32_t)markdown_core_node_get_kind(node);
}

const markdown_core_node *es_node_first_child(const markdown_core_node *node) {
    return markdown_core_node_get_first_child(node);
}

const markdown_core_node *es_node_next_sibling(const markdown_core_node *node) {
    return markdown_core_node_get_next_sibling(node);
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
    markdown_core_string_view info, language, literal;
    bool fenced, closed;
    markdown_core_node_code_block_properties(node, &info, &language, &literal, &fenced, &closed);
    return field == 0 ? fenced : closed;
}

int32_t es_node_formula_mode(const markdown_core_node *node) {
    markdown_core_placement_mode mode;
    markdown_core_string_view literal;
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

int32_t es_node_directive_mode(const markdown_core_node *node) {
    markdown_core_placement_mode mode;
    markdown_core_string_view name, attributes;
    bool has_label;
    size_t label_count;
    markdown_core_node_directive_properties(node, &mode, &name, &attributes, &has_label,
                                            &label_count);
    return (int32_t)mode;
}

int32_t es_node_directive_label_count(const markdown_core_node *node) {
    markdown_core_placement_mode mode;
    markdown_core_string_view name, attributes;
    bool has_label;
    size_t label_count;
    markdown_core_node_directive_properties(node, &mode, &name, &attributes, &has_label,
                                            &label_count);
    return has_label ? (int32_t)label_count : -1;
}

void es_string(const void *object, int32_t field, uintptr_t *data, size_t *length) {
    markdown_core_string_view first = {NULL, 0}, second = {NULL, 0}, third = {NULL, 0};
    const markdown_core_node *node = (const markdown_core_node *)object;
    bool first_bool, second_bool;
    markdown_core_placement_mode mode;
    size_t count;
    switch (field) {
    case ES_STRING_CODE_INFO:
    case ES_STRING_CODE_LANGUAGE:
    case ES_STRING_CODE_LITERAL:
        markdown_core_node_code_block_properties(node, &first, &second, &third, &first_bool,
                                                 &second_bool);
        es_write_view(field == ES_STRING_CODE_INFO       ? first
                      : field == ES_STRING_CODE_LANGUAGE ? second
                                                         : third,
                      data, length);
        return;
    case ES_STRING_LITERAL:
        markdown_core_node_literal(node, &first);
        break;
    case ES_STRING_FORMULA_LITERAL:
        markdown_core_node_formula_properties(node, &mode, &first);
        break;
    case ES_STRING_DIRECTIVE_NAME:
    case ES_STRING_DIRECTIVE_ATTRIBUTES:
        markdown_core_node_directive_properties(node, &mode, &first, &second, &first_bool, &count);
        first = field == ES_STRING_DIRECTIVE_NAME ? first : second;
        break;
    case ES_STRING_LINK_DESTINATION:
    case ES_STRING_LINK_TITLE:
        markdown_core_node_link_properties(node, &first, &second);
        first = field == ES_STRING_LINK_DESTINATION ? first : second;
        break;
    case ES_STRING_IMAGE_SOURCE:
    case ES_STRING_IMAGE_TITLE:
        markdown_core_node_image_properties(node, &first, &second);
        first = field == ES_STRING_IMAGE_SOURCE ? first : second;
        break;
    case ES_STRING_FOOTNOTE_ID:
        markdown_core_node_footnote_id(node, &first);
        break;
    case ES_STRING_ERROR_MESSAGE:
        first = markdown_core_error_get_message((const markdown_core_error *)object);
        break;
    default:
        break;
    }
    es_write_view(first, data, length);
}
