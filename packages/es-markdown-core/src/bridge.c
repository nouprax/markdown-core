#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "markdown_core.h"

enum es_string_field {
    ES_STRING_LITERAL = 1,
    ES_STRING_FORMULA_LITERAL,
    ES_STRING_LINK_DESTINATION,
    ES_STRING_LINK_TITLE,
    ES_STRING_IMAGE_SOURCE,
    ES_STRING_IMAGE_TITLE,
    ES_STRING_FOOTNOTE_ID,
    ES_STRING_CROSS_LINK_REFERENCE,
    ES_STRING_EMBED_REFERENCE,
    ES_STRING_ERROR_MESSAGE
};

typedef struct {
    int32_t mode;
    uint32_t reserved;
    uint32_t name_data;
    uint32_t name_length;
    uint32_t attributes_data;
    uint32_t attributes_length;
} es_directive_properties_layout;

#define ES_LAYOUT_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]
ES_LAYOUT_ASSERT(es_scope_entry_size_is_32, sizeof(markdown_core_scope_entry) == 32);
ES_LAYOUT_ASSERT(es_scope_entry_id_starts_at_0, offsetof(markdown_core_scope_entry, id) == 0);
ES_LAYOUT_ASSERT(es_scope_entry_revision_starts_at_8, offsetof(markdown_core_scope_entry, revision) == 8);
ES_LAYOUT_ASSERT(es_scope_entry_scope_starts_at_16, offsetof(markdown_core_scope_entry, scope) == 16);
ES_LAYOUT_ASSERT(es_scope_size_is_16, sizeof(markdown_core_scope) == 16);
ES_LAYOUT_ASSERT(es_scope_start_starts_at_0, offsetof(markdown_core_scope, start) == 0);
ES_LAYOUT_ASSERT(es_scope_end_starts_at_8, offsetof(markdown_core_scope, end) == 8);
ES_LAYOUT_ASSERT(es_position_size_is_8, sizeof(markdown_core_position) == 8);
ES_LAYOUT_ASSERT(es_position_line_starts_at_0, offsetof(markdown_core_position, line) == 0);
ES_LAYOUT_ASSERT(es_position_column_starts_at_4, offsetof(markdown_core_position, column) == 4);
ES_LAYOUT_ASSERT(es_delta_entry_size_is_24, sizeof(markdown_core_delta_entry) == 24);
ES_LAYOUT_ASSERT(es_delta_entry_id_starts_at_0, offsetof(markdown_core_delta_entry, id) == 0);
ES_LAYOUT_ASSERT(es_delta_entry_parent_starts_at_8, offsetof(markdown_core_delta_entry, parent) == 8);
ES_LAYOUT_ASSERT(es_delta_entry_change_starts_at_16, offsetof(markdown_core_delta_entry, change) == 16);
ES_LAYOUT_ASSERT(es_directive_properties_size_is_24, sizeof(es_directive_properties_layout) == 24);
ES_LAYOUT_ASSERT(es_directive_properties_mode_starts_at_0, offsetof(es_directive_properties_layout, mode) == 0);
ES_LAYOUT_ASSERT(es_directive_properties_name_starts_at_8, offsetof(es_directive_properties_layout, name_data) == 8);
ES_LAYOUT_ASSERT(
    es_directive_properties_attributes_starts_at_16,
    offsetof(es_directive_properties_layout, attributes_data) == 16
);
#undef ES_LAYOUT_ASSERT

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
    options.directives = (flags & (1u << 8)) != 0;
    options.cross_links = (flags & (1u << 9)) != 0;
    options.embeds = (flags & (1u << 10)) != 0;
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

int32_t es_session_commit(markdown_core_session *session, markdown_core_delta **changes, markdown_core_error **error) {
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

int32_t es_session_ordered_delta_entries(
    const markdown_core_session *session,
    const markdown_core_delta *changes,
    uintptr_t *data,
    size_t *count,
    uintptr_t *error_output
) {
    markdown_core_delta_entry *entries = NULL;
    markdown_core_error *error = NULL;
    bool succeeded;

    if (!data || !count || !error_output) {
        return 0;
    }
    succeeded = markdown_core_session_ordered_delta_entries(session, changes, &entries, count, &error);
    *data = (uintptr_t)entries;
    *error_output = (uintptr_t)error;
    return succeeded ? 1 : 0;
}

void es_delta_entries_free(markdown_core_delta_entry *entries) { markdown_core_delta_entries_free(entries); }

void es_delta_free(markdown_core_delta *changes) { markdown_core_delta_free(changes); }

const markdown_core_node *es_document_root(const markdown_core_document *document) {
    return markdown_core_document_root(document);
}

uint64_t es_node_id(const markdown_core_node *node) { return markdown_core_node_get_id(node); }

uint64_t es_node_revision(const markdown_core_node *node) { return markdown_core_node_get_revision(node); }

const markdown_core_node *es_session_node_by_id(const markdown_core_session *session, uint64_t id) {
    return markdown_core_session_node_by_id(session, id);
}

int32_t es_error_code(const markdown_core_error *error) { return (int32_t)markdown_core_error_get_code(error); }

int32_t es_error_scope(const markdown_core_error *error, int32_t *coordinates) {
    markdown_core_scope scope;
    if (!markdown_core_error_get_scope(error, &scope)) {
        return 0;
    }
    coordinates[0] = scope.start.line;
    coordinates[1] = scope.start.column;
    coordinates[2] = scope.end.line;
    coordinates[3] = scope.end.column;
    return 1;
}

void es_error_free(markdown_core_error *error) { markdown_core_error_free(error); }

int32_t es_node_kind(const markdown_core_node *node) { return (int32_t)markdown_core_node_get_kind(node); }

const markdown_core_node *es_node_first_child(const markdown_core_node *node) {
    return markdown_core_node_get_first_child(node);
}

const markdown_core_node *es_node_next_sibling(const markdown_core_node *node) {
    return markdown_core_node_get_next_sibling(node);
}

/**
 * Returns the core's packed canonical-preorder scope table without another
 * traversal or copy. The public row layout is 32 bytes on the wasm32 ABI.
 */
size_t es_scope_table(const markdown_core_document *document, uintptr_t *data) {
    markdown_core_scope_entry *rows = NULL;
    size_t count = 0;

    if (!data) {
        return 0;
    }
    *data = 0;
    if (!markdown_core_document_scope_table(document, &rows, &count, NULL)) {
        return 0;
    }
    *data = (uintptr_t)rows;
    return count;
}

void es_scope_table_free(markdown_core_scope_entry *rows) { markdown_core_scope_table_free(rows); }

int32_t es_node_heading_level(const markdown_core_node *node) {
    int32_t value = 0;
    markdown_core_node_heading_level(node, &value);
    return value;
}

// Layout (32 bytes): i32 flavor, i32 tight, i32 has_start, i32 padding,
// i64 start.
void es_node_list_properties(const markdown_core_node *node, void *out) {
    markdown_core_list_flavor flavor;
    markdown_core_optional_i64 start;
    bool tight;
    int32_t *fields = (int32_t *)out;
    markdown_core_node_list_properties(node, &flavor, &start, &tight);
    fields[0] = (int32_t)flavor;
    fields[1] = tight;
    fields[2] = start.has_value;
    fields[3] = 0;
    ((int64_t *)out)[2] = start.value;
}

int32_t es_node_checked(const markdown_core_node *node) {
    markdown_core_optional_bool checked;
    markdown_core_node_list_item_checked(node, &checked);
    return checked.has_value ? (checked.value ? 1 : 0) : -1;
}

// Layout (32 bytes): u32 info data/length, u32 language data/length,
// u32 literal data/length, i32 fenced, i32 closed.
void es_node_code_properties(const markdown_core_node *node, void *out) {
    markdown_core_string_view info, language, literal;
    bool fenced, closed;
    uint32_t *fields = (uint32_t *)out;
    markdown_core_node_code_block_properties(node, &info, &language, &literal, &fenced, &closed);
    fields[0] = (uint32_t)(uintptr_t)info.data;
    fields[1] = (uint32_t)info.length;
    fields[2] = (uint32_t)(uintptr_t)language.data;
    fields[3] = (uint32_t)language.length;
    fields[4] = (uint32_t)(uintptr_t)literal.data;
    fields[5] = (uint32_t)literal.length;
    fields[6] = fenced;
    fields[7] = closed;
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

// Layout (24 bytes): i32 mode, reserved u32, then the name and attributes
// string views. Label presence and content come from the canonical child
// topology, exactly like every other typed child relation.
void es_node_directive_properties(const markdown_core_node *node, void *out) {
    markdown_core_placement_mode mode;
    markdown_core_string_view name, attributes;
    es_directive_properties_layout *properties = (es_directive_properties_layout *)out;

    markdown_core_node_directive_properties(node, &mode, &name, &attributes);
    properties->mode = (int32_t)mode;
    properties->reserved = 0;
    properties->name_data = (uint32_t)(uintptr_t)name.data;
    properties->name_length = (uint32_t)name.length;
    properties->attributes_data = (uint32_t)(uintptr_t)attributes.data;
    properties->attributes_length = (uint32_t)attributes.length;
}

void es_string(const void *object, int32_t field, uintptr_t *data, size_t *length) {
    markdown_core_string_view first = {NULL, 0}, second = {NULL, 0};
    const markdown_core_node *node = (const markdown_core_node *)object;
    markdown_core_placement_mode mode;
    switch (field) {
    case ES_STRING_LITERAL:
        markdown_core_node_literal(node, &first);
        break;
    case ES_STRING_FORMULA_LITERAL:
        markdown_core_node_formula_properties(node, &mode, &first);
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
    case ES_STRING_CROSS_LINK_REFERENCE:
        markdown_core_node_cross_link_reference(node, &first);
        break;
    case ES_STRING_EMBED_REFERENCE:
        markdown_core_node_embed_reference(node, &first);
        break;
    case ES_STRING_ERROR_MESSAGE:
        first = markdown_core_error_get_message((const markdown_core_error *)object);
        break;
    default:
        break;
    }
    es_write_view(first, data, length);
}
