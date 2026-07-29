#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "markdown_core.h"

// Internal engine headers, compiled into the same WASM module: the scope
// table export resolves sealed-relative positions with the canonical dump's
// parent accumulator (extensions/ast.c dump_tree is the reference), which
// needs the raw node fields the facade deliberately hides.
#include <node.h>
#include "directive.h"

enum es_string_field {
    ES_STRING_LITERAL = 1,
    ES_STRING_FORMULA_LITERAL,
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

const markdown_core_node *es_session_node_by_id(const markdown_core_session *session, uint64_t id) {
    return markdown_core_session_node_by_id(session, id);
}

const markdown_core_node *es_node_parent(const markdown_core_node *node) {
    return markdown_core_node_get_parent(node);
}

int32_t es_error_code(const markdown_core_error *error) {
    return (int32_t)markdown_core_error_get_code(error);
}

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

int32_t es_node_kind(const markdown_core_node *node) {
    return (int32_t)markdown_core_node_get_kind(node);
}

const markdown_core_node *es_node_first_child(const markdown_core_node *node) {
    return markdown_core_node_get_first_child(node);
}

const markdown_core_node *es_node_next_sibling(const markdown_core_node *node) {
    return markdown_core_node_get_next_sibling(node);
}

typedef struct {
    uint64_t id;
    uint64_t revision;
    int32_t start_line;
    int32_t start_column;
    int32_t end_line;
    int32_t end_column;
} es_scope_row;

typedef struct {
    const markdown_core_node *node;
    int32_t parent_start_line;
} es_scope_frame;

static bool es_is_label(const markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

static bool es_scope_reserve(void **items, size_t *capacity, size_t needed, size_t item_size) {
    void *grown;
    size_t next = *capacity ? *capacity : 64;
    if (needed <= *capacity) {
        return true;
    }
    while (next < needed) {
        next *= 2;
    }
    grown = realloc(*items, next * item_size);
    if (!grown) {
        return false;
    }
    *items = grown;
    *capacity = next;
    return true;
}

/**
 * One pre-order walk over the canonical subtree at `root`, emitting every
 * node's (id, revision, absolute scope) row. Sealed-relative positions
 * resolve through a parent accumulator — the same arithmetic as the
 * canonical dump (extensions/ast.c dump_tree) — so building a snapshot's
 * whole scope table is O(n), not n times the O(depth) ancestor walk of
 * markdown_core_node_scope. Depth is input-controlled, hence the explicit
 * frame stack. Returns the row count and writes the malloc'd row array to
 * `data` (caller frees); a zero count with a null `data` reports failure.
 */
size_t es_scope_table(const markdown_core_node *root, uintptr_t *data) {
    es_scope_row *rows = NULL;
    es_scope_frame *stack = NULL;
    size_t count = 0, row_capacity = 0, depth = 0, stack_capacity = 0;
    *data = 0;
    if (!root || !es_scope_reserve((void **)&stack, &stack_capacity, 1, sizeof(*stack))) {
        return 0;
    }
    stack[depth].node = root;
    stack[depth].parent_start_line = 0;
    depth++;
    while (depth) {
        es_scope_frame frame = stack[--depth];
        const markdown_core_node *node = frame.node;
        const markdown_core_node *child;
        int32_t start_line = node->start_line;
        int32_t end_line = node->end_line;
        es_scope_row *row;
        if (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
            // The canonical traversal hides directive-label wrappers, so a
            // hidden wrapper between this node and its canonical parent
            // contributes its own delta.
            start_line += frame.parent_start_line;
            if (es_is_label(node->parent)) {
                start_line += node->parent->start_line;
            }
            end_line += start_line;
        }
        if (!es_scope_reserve((void **)&rows, &row_capacity, count + 1, sizeof(*rows))) {
            free(rows);
            free(stack);
            return 0;
        }
        row = &rows[count++];
        row->id = markdown_core_node_get_id(node);
        row->revision = markdown_core_node_get_revision(node);
        row->start_line = start_line;
        row->start_column = node->start_column;
        row->end_line = end_line;
        row->end_column = node->end_column;
        for (child = markdown_core_node_get_first_child(node); child;
             child = markdown_core_node_get_next_sibling(child)) {
            if (!es_scope_reserve((void **)&stack, &stack_capacity, depth + 1, sizeof(*stack))) {
                free(rows);
                free(stack);
                return 0;
            }
            stack[depth].node = child;
            stack[depth].parent_start_line = start_line;
            depth++;
        }
    }
    free(stack);
    *data = (uintptr_t)rows;
    return count;
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

// Layout (24 bytes): i32 mode, i32 label count (-1 without a label),
// u32 name data/length, u32 attributes data/length.
void es_node_directive_properties(const markdown_core_node *node, void *out) {
    markdown_core_placement_mode mode;
    markdown_core_string_view name, attributes;
    bool has_label;
    size_t label_count;
    int32_t *fields = (int32_t *)out;
    uint32_t *views = (uint32_t *)out;
    markdown_core_node_directive_properties(node, &mode, &name, &attributes, &has_label,
                                            &label_count);
    fields[0] = (int32_t)mode;
    fields[1] = has_label ? (int32_t)label_count : -1;
    views[2] = (uint32_t)(uintptr_t)name.data;
    views[3] = (uint32_t)name.length;
    views[4] = (uint32_t)(uintptr_t)attributes.data;
    views[5] = (uint32_t)attributes.length;
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
    case ES_STRING_ERROR_MESSAGE:
        first = markdown_core_error_get_message((const markdown_core_error *)object);
        break;
    default:
        break;
    }
    es_write_view(first, data, length);
}
