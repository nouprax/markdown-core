#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
    ES_STRING_ERROR_MESSAGE,
    /* Appended: the ids are shared with the decoder's `stringField` table, so
     * inserting one in the middle would renumber the rest. */
    ES_STRING_DEFINITION_LABEL,
    ES_STRING_DEFINITION_DESTINATION,
    ES_STRING_DEFINITION_TITLE,
    ES_STRING_REFERENCE_LABEL
};

typedef struct {
    int32_t mode;
    uint32_t has_attributes;
    uint32_t name_data;
    uint32_t name_length;
    uint32_t attribute_count;
    uint32_t reserved;
} es_directive_properties_layout;

#define ES_LAYOUT_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]
ES_LAYOUT_ASSERT(es_scope_size_is_16, sizeof(markdown_core_scope) == 16);
ES_LAYOUT_ASSERT(es_scope_start_starts_at_0, offsetof(markdown_core_scope, start) == 0);
ES_LAYOUT_ASSERT(es_scope_end_starts_at_8, offsetof(markdown_core_scope, end) == 8);
ES_LAYOUT_ASSERT(es_position_size_is_8, sizeof(markdown_core_position) == 8);
ES_LAYOUT_ASSERT(es_position_line_starts_at_0, offsetof(markdown_core_position, line) == 0);
ES_LAYOUT_ASSERT(es_position_column_starts_at_4, offsetof(markdown_core_position, column) == 4);
ES_LAYOUT_ASSERT(es_diff_size_is_16, sizeof(markdown_core_diff) == 16);
ES_LAYOUT_ASSERT(es_diff_markup_starts_at_0, offsetof(markdown_core_diff, markup) == 0);
ES_LAYOUT_ASSERT(es_diff_parts_starts_at_8, offsetof(markdown_core_diff, parts) == 8);
ES_LAYOUT_ASSERT(es_diagnostic_size_is_20, sizeof(markdown_core_diagnostic) == 20);
ES_LAYOUT_ASSERT(es_diagnostic_code_starts_at_0, offsetof(markdown_core_diagnostic, code) == 0);
ES_LAYOUT_ASSERT(es_diagnostic_scope_starts_at_4, offsetof(markdown_core_diagnostic, scope) == 4);
ES_LAYOUT_ASSERT(es_directive_properties_size_is_24, sizeof(es_directive_properties_layout) == 24);
ES_LAYOUT_ASSERT(es_directive_properties_mode_starts_at_0, offsetof(es_directive_properties_layout, mode) == 0);
ES_LAYOUT_ASSERT(es_directive_properties_name_starts_at_8, offsetof(es_directive_properties_layout, name_data) == 8);
ES_LAYOUT_ASSERT(
    es_directive_properties_count_starts_at_16,
    offsetof(es_directive_properties_layout, attribute_count) == 16
);
#undef ES_LAYOUT_ASSERT

static void es_write_view(markdown_core_string view, uintptr_t *data, size_t *length) {
    *data = (uintptr_t)view.data;
    *length = view.length;
}

markdown_core_document *es_document_open(
    const uint8_t *bytes,
    size_t length,
    uint32_t flags,
    markdown_core_error **error
) {
    markdown_core_parse_options options;
    markdown_core_string markdown;
    markdown_core_parse_options_init(&options);
    options.smart_punctuation = (flags & (1u << 0)) != 0;
    options.footnotes = (flags & (1u << 1)) != 0;
    options.tables = (flags & (1u << 2)) != 0;
    options.strikethrough = (flags & (1u << 3)) != 0;
    options.autolinks = (flags & (1u << 4)) != 0;
    options.task_lists = (flags & (1u << 5)) != 0;
    options.formulas = (flags & (1u << 6)) != 0;
    options.directives = (flags & (1u << 7)) != 0;
    options.cross_links = (flags & (1u << 8)) != 0;
    options.embeds = (flags & (1u << 9)) != 0;
    markdown.data = bytes;
    markdown.length = length;
    return markdown_core_document_new(markdown, &options, error);
}

/* Hands `document` new text. CONSUMES it on every path, success or not;
 * `out` receives the successor document and the delta, each caller-owned. */
int32_t es_document_edit(
    markdown_core_document *document,
    const uint8_t *bytes,
    size_t length,
    uintptr_t *out,
    markdown_core_error **error
) {
    markdown_core_commit commit;
    markdown_core_string markdown;

    markdown.data = bytes;
    markdown.length = length;
    memset(&commit, 0, sizeof(commit));
    // The edit reads `document` and takes nothing; the JS handle keeps it and
    // `es_document_free` is what releases it.
    if (!markdown_core_document_edit(document, markdown, &commit, error)) {
        return 0;
    }
    out[0] = (uintptr_t)commit.document;
    out[1] = (uintptr_t)commit.delta;
    return 1;
}

void es_document_free(markdown_core_document *document) { markdown_core_document_free(document); }

uint64_t es_document_series(const markdown_core_document *document) {
    return markdown_core_document_series(document);
}

/* Every diagnostic of `document`: the count, and a pointer to the document's
 * own array, which borrows from it. Nothing to free. */
size_t es_document_diagnostics(const markdown_core_document *document, uintptr_t *data) {
    const markdown_core_diagnostic *rows = NULL;
    size_t count = markdown_core_document_diagnostics(document, &rows);
    *data = (uintptr_t)rows;
    return count;
}

uint64_t es_delta_revision(const markdown_core_delta *changes, int32_t boundary) {
    uint64_t before = 0;
    uint64_t after = 0;
    markdown_core_delta_revisions(changes, &before, &after);
    return boundary == 0 ? before : after;
}

/**
 * The delta's rows, handed back as the facade's own array: `(id, parts)` at
 * a 16-byte stride, borrowing from the delta and freed with it.
 *
 * No copy and no resolved node pointers: the value tree is rebuilt by the
 * decoder's own walk over the committed tree, and this array is only the
 * diff a consumer reads to locate what changed.
 */
size_t es_delta_diffs(const markdown_core_delta *changes, uintptr_t *data) {
    const markdown_core_diff *rows = NULL;
    size_t count = markdown_core_delta_diffs(changes, &rows);
    *data = (uintptr_t)rows;
    return count;
}

void es_delta_free(markdown_core_delta *changes) { markdown_core_delta_free(changes); }

const markdown_core_node *es_document_root(const markdown_core_document *document) {
    return markdown_core_document_root(document);
}

uint64_t es_node_id(const markdown_core_node *node) { return markdown_core_node_get_id(node); }

uint64_t es_node_revision(const markdown_core_node *node) { return markdown_core_node_get_revision(node); }

/* The node's own absolute extent, written into the shared scratch block as
 * four int32 coordinates. O(1): the parser stored it on the node. */
void es_node_scope(const markdown_core_node *node, int32_t *coordinates) {
    markdown_core_scope scope = markdown_core_node_scope(node);
    coordinates[0] = scope.start.line;
    coordinates[1] = scope.start.column;
    coordinates[2] = scope.end.line;
    coordinates[3] = scope.end.column;
}

/* The one-complete-comment bit, from the parser. Deriving it a second time in
 * each binding is a second definition that can disagree with the first. */
int32_t es_node_html_comment(const markdown_core_node *node) {
    bool comment = false;
    markdown_core_node_html_comment(node, &comment);
    return comment ? 1 : 0;
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

/* A reference's source form. Its label travels through es_string like every
 * other string; only this scalar needs an accessor of its own. */
int32_t es_node_reference_form(const markdown_core_node *node) {
    markdown_core_string label = {NULL, 0};
    markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
    markdown_core_node_reference_properties(node, &label, &form);
    return (int32_t)form;
}

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
    markdown_core_string info, language, literal;
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

// Layout (24 bytes): i32 mode, u32 has-attributes, the name string view, and
// the attribute count. The entries themselves come from
// es_node_directive_attribute_at, one crossing per pair, because a map is
// what they are -- there is no serialized form to hand over in one piece.
// Label presence and content come from the canonical child topology, exactly
// like every other typed child relation.
void es_node_directive_properties(const markdown_core_node *node, void *out) {
    markdown_core_placement_mode mode;
    markdown_core_string name;
    bool has_attributes = false;
    size_t count = 0;
    es_directive_properties_layout *properties = (es_directive_properties_layout *)out;

    markdown_core_node_directive_properties(node, &mode, &name, &has_attributes);
    markdown_core_node_directive_attribute_count(node, &count);
    properties->mode = (int32_t)mode;
    properties->has_attributes = has_attributes ? 1u : 0u;
    properties->name_data = (uint32_t)(uintptr_t)name.data;
    properties->name_length = (uint32_t)name.length;
    properties->attribute_count = (uint32_t)count;
    properties->reserved = 0;
}

/* One attribute pair into the shared scratch: u32 key data, u32 key length,
 * u32 value data, u32 value length. */
int32_t es_node_directive_attribute_at(const markdown_core_node *node, size_t index, void *out) {
    markdown_core_string key = {NULL, 0}, value = {NULL, 0};
    uint32_t *slots = (uint32_t *)out;

    if (!markdown_core_node_directive_attribute_at(node, index, &key, &value)) {
        return 0;
    }
    slots[0] = (uint32_t)(uintptr_t)key.data;
    slots[1] = (uint32_t)key.length;
    slots[2] = (uint32_t)(uintptr_t)value.data;
    slots[3] = (uint32_t)value.length;
    return 1;
}

void es_string(const void *object, int32_t field, uintptr_t *data, size_t *length) {
    markdown_core_string first = {NULL, 0}, second = {NULL, 0};
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
    case ES_STRING_DEFINITION_LABEL:
    case ES_STRING_DEFINITION_DESTINATION:
    case ES_STRING_DEFINITION_TITLE: {
        markdown_core_string third = {NULL, 0};
        markdown_core_node_reference_definition_properties(node, &first, &second, &third);
        if (field == ES_STRING_DEFINITION_DESTINATION) {
            first = second;
        } else if (field == ES_STRING_DEFINITION_TITLE) {
            first = third;
        }
        break;
    }
    case ES_STRING_REFERENCE_LABEL: {
        markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        markdown_core_node_reference_properties(node, &first, &form);
        break;
    }
    default:
        break;
    }
    es_write_view(first, data, length);
}
