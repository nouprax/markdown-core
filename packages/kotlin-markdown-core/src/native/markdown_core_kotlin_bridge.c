#include "markdown_core_kotlin_bridge.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* MKC3 wire: every payload opens with the magic and a status byte (0 =
 * success data follows, 1 = an error record follows). Node records carry
 * (kind, id, revision, fields, child-id lists) and never positions; a commit
 * body lists removed ids and then full records for added/changed/bubbled
 * nodes ordered children-before-parents, which is exactly the order a mirror
 * can rebuild in. Scopes travel as a separate (id, revision, scope) table so
 * snapshots can materialize them lazily. */

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

static void put_u64(bridge_buffer *buffer, uint64_t value) {
    size_t index;
    for (index = 0; index < 8; ++index) {
        put_u8(buffer, (uint8_t)(value >> (index * 8)));
    }
}

static void put_i64(bridge_buffer *buffer, int64_t value) { put_u64(buffer, (uint64_t)value); }

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

static void put_magic(bridge_buffer *buffer) {
    static const uint8_t magic[] = {'M', 'K', 'C', '3'};
    put_bytes(buffer, magic, sizeof(magic));
}

/* Consumes (frees) the error. */
static void put_error(bridge_buffer *buffer, markdown_core_error *error) {
    markdown_core_scope scope;
    bool has_scope = error != NULL && markdown_core_error_get_scope(error, &scope);
    put_u8(buffer, 1);
    put_i32(buffer, error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error));
    if (error == NULL) {
        markdown_core_string_view fallback = {(const uint8_t *)"markdown parsing failed", 23};
        put_string(buffer, fallback, true);
    } else {
        put_string(buffer, markdown_core_error_get_message(error), true);
    }
    put_u8(buffer, has_scope ? 1 : 0);
    if (has_scope) {
        put_scope(buffer, scope);
    }
    markdown_core_error_free(error);
}

static void put_argument_error(bridge_buffer *buffer, const char *message) {
    markdown_core_string_view view = {(const uint8_t *)message, strlen(message)};
    put_u8(buffer, 1);
    put_i32(buffer, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT);
    put_string(buffer, view, true);
    put_u8(buffer, 0);
}

static bool finish(bridge_buffer *buffer, uint8_t **output, size_t *output_length) {
    if (output == NULL || output_length == NULL) {
        free(buffer->data);
        return false;
    }
    if (buffer->failed) {
        free(buffer->data);
        *output = NULL;
        *output_length = 0;
        return false;
    }
    *output = buffer->data;
    *output_length = buffer->size;
    return true;
}

static void put_id_list(bridge_buffer *buffer, const markdown_core_node *node, size_t count) {
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
        put_u64(buffer, markdown_core_node_get_id(node));
        node = markdown_core_node_get_next_sibling(node);
    }
}

static void put_child_ids(bridge_buffer *buffer, const markdown_core_node *node) {
    put_id_list(buffer, markdown_core_node_get_first_child(node), markdown_core_node_child_count(node));
}

static void write_record(bridge_buffer *buffer, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string_view first = {0};
    markdown_core_string_view second = {0};
    markdown_core_string_view third = {0};

    put_u8(buffer, (uint8_t)kind);
    put_u64(buffer, markdown_core_node_get_id(node));
    put_u64(buffer, markdown_core_node_get_revision(node));

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        markdown_core_node_heading_level(node, &level);
        put_i32(buffer, level);
        put_child_ids(buffer, node);
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
        put_child_ids(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked;
        markdown_core_node_list_item_checked(node, &checked);
        put_u8(buffer, checked.has_value ? (checked.value ? 1 : 0) : UINT8_MAX);
        put_child_ids(buffer, node);
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
        put_child_ids(buffer, node);
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
            put_id_list(buffer, markdown_core_node_directive_first_label_child(node), label_count);
        } else {
            put_i32(buffer, -1);
        }
        content = markdown_core_node_directive_first_content_child(node);
        for (const markdown_core_node *cursor = content; cursor != NULL;
             cursor = markdown_core_node_get_next_sibling(cursor)) {
            ++content_count;
        }
        put_id_list(buffer, content, content_count);
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
        markdown_core_node_footnote_id(node, &first);
        put_string(buffer, first, true);
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(node, &first);
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &first, &second);
        put_string(buffer, first, first.data != NULL);
        put_string(buffer, second, second.data != NULL);
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &first, &second);
        put_string(buffer, first, first.data != NULL);
        put_string(buffer, second, second.data != NULL);
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        markdown_core_node_table_row_is_header(node, &header);
        put_u8(buffer, header ? 1 : 0);
        put_child_ids(buffer, node);
        break;
    }
    default:
        buffer->failed = true;
        break;
    }
}

enum { VERDICT_ADDED = 0, VERDICT_CHANGED = 1, VERDICT_BUBBLED = 2 };

typedef struct record_entry {
    const markdown_core_node *node;
    uint32_t depth;
    uint8_t verdict;
} record_entry;

static uint32_t node_depth(const markdown_core_node *node) {
    uint32_t depth = 0;
    const markdown_core_node *parent = markdown_core_node_get_parent(node);
    while (parent != NULL) {
        ++depth;
        parent = markdown_core_node_get_parent(parent);
    }
    return depth;
}

/* Deeper entries first: a record's children are strictly deeper than the
 * record, so this order lets the decoder resolve child ids in one pass.
 * Equal-depth nodes are never ancestor-related; their order is free. */
static int record_order(const void *left, const void *right) {
    const record_entry *a = (const record_entry *)left;
    const record_entry *b = (const record_entry *)right;
    if (a->depth != b->depth) {
        return a->depth > b->depth ? -1 : 1;
    }
    return 0;
}

static bool gather_records(record_entry *entries, size_t *cursor, const markdown_core_session *session,
                           const markdown_core_delta *changes,
                           size_t (*accessor)(const markdown_core_delta *, const markdown_core_node_id **),
                           uint8_t verdict) {
    const markdown_core_node_id *ids = NULL;
    size_t count = accessor(changes, &ids);
    size_t index;
    for (index = 0; index < count; ++index) {
        const markdown_core_node *node = markdown_core_session_node_by_id(session, ids[index]);
        if (node == NULL) {
            return false;
        }
        entries[*cursor].node = node;
        entries[*cursor].depth = node_depth(node);
        entries[*cursor].verdict = verdict;
        ++*cursor;
    }
    return true;
}

static void encode_commit_body(bridge_buffer *buffer, const markdown_core_session *session,
                               const markdown_core_delta *changes) {
    uint64_t before = 0;
    uint64_t after = 0;
    const markdown_core_node_id *removed = NULL;
    size_t removed_count;
    const markdown_core_node_id *ids = NULL;
    size_t total;
    size_t cursor = 0;
    size_t index;
    record_entry *entries;

    markdown_core_delta_revisions(changes, &before, &after);
    put_u64(buffer, before);
    put_u64(buffer, after);

    removed_count = markdown_core_delta_removed(changes, &removed);
    if (removed_count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)removed_count);
    for (index = 0; index < removed_count; ++index) {
        put_u64(buffer, removed[index]);
    }

    total = markdown_core_delta_added(changes, &ids) + markdown_core_delta_changed(changes, &ids)
            + markdown_core_delta_bubbled(changes, &ids);
    if (total > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)total);
    if (total == 0) {
        return;
    }
    entries = (record_entry *)malloc(total * sizeof(record_entry));
    if (entries == NULL) {
        buffer->failed = true;
        return;
    }
    if (!gather_records(entries, &cursor, session, changes, markdown_core_delta_added, VERDICT_ADDED)
        || !gather_records(entries, &cursor, session, changes, markdown_core_delta_changed, VERDICT_CHANGED)
        || !gather_records(entries, &cursor, session, changes, markdown_core_delta_bubbled, VERDICT_BUBBLED)) {
        free(entries);
        buffer->failed = true;
        return;
    }
    qsort(entries, total, sizeof(record_entry), record_order);
    for (index = 0; index < total; ++index) {
        put_u8(buffer, entries[index].verdict);
        write_record(buffer, entries[index].node);
    }
    free(entries);
}

typedef struct node_stack {
    const markdown_core_node **items;
    size_t count;
    size_t capacity;
} node_stack;

static bool stack_push(node_stack *stack, const markdown_core_node *node) {
    if (stack->count == stack->capacity) {
        size_t capacity = stack->capacity == 0 ? 64 : stack->capacity * 2;
        const markdown_core_node **items =
            (const markdown_core_node **)realloc((void *)stack->items, capacity * sizeof(*items));
        if (items == NULL) {
            return false;
        }
        stack->items = items;
        stack->capacity = capacity;
    }
    stack->items[stack->count++] = node;
    return true;
}

static void encode_scope_table(bridge_buffer *buffer, const markdown_core_session *session) {
    const markdown_core_document *view = markdown_core_session_document(session);
    const markdown_core_node *root = view == NULL ? NULL : markdown_core_document_root(view);
    node_stack stack = {0};
    size_t count_offset;
    uint64_t count = 0;

    if (root == NULL) {
        buffer->failed = true;
        return;
    }
    count_offset = buffer->size;
    put_i32(buffer, 0);
    if (!stack_push(&stack, root)) {
        buffer->failed = true;
        return;
    }
    while (stack.count != 0 && !buffer->failed) {
        const markdown_core_node *node = stack.items[--stack.count];
        const markdown_core_node *child;
        put_u64(buffer, markdown_core_node_get_id(node));
        put_u64(buffer, markdown_core_node_get_revision(node));
        put_scope(buffer, markdown_core_node_scope(node));
        ++count;
        for (child = markdown_core_node_get_first_child(node); child != NULL;
             child = markdown_core_node_get_next_sibling(child)) {
            if (!stack_push(&stack, child)) {
                buffer->failed = true;
                break;
            }
        }
    }
    free((void *)stack.items);
    if (buffer->failed) {
        return;
    }
    if (count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    for (size_t index = 0; index < 4; ++index) {
        buffer->data[count_offset + index] = (uint8_t)((uint32_t)count >> (index * 8));
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

static markdown_core_session *to_session(markdown_core_kotlin_session *session) {
    return (markdown_core_session *)session;
}

static const markdown_core_session *to_const_session(const markdown_core_kotlin_session *session) {
    return (const markdown_core_session *)session;
}

static uint64_t session_root_id(const markdown_core_session *session) {
    const markdown_core_document *view = markdown_core_session_document(session);
    const markdown_core_node *root = view == NULL ? NULL : markdown_core_document_root(view);
    return root == NULL ? 0 : markdown_core_node_get_id(root);
}

/* One-shot parse as session sugar: open + edit + single delta-bearing commit
 * + free, encoded in one crossing. The commit's delta lists every node as
 * added, so the payload body is the same record stream a session commit
 * produces, followed by the eagerly materialized scope table. */
bool markdown_core_kotlin_parse(const uint8_t *source, size_t length, uint32_t options_mask, uint8_t **output,
                                size_t *output_length) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_session *session;
    markdown_core_delta *changes = NULL;
    bridge_buffer buffer = {0};

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);

    put_magic(&buffer);
    session = markdown_core_session_open(&options, &error);
    if (session == NULL) {
        put_error(&buffer, error);
        return finish(&buffer, output, output_length);
    }
    if (!markdown_core_session_edit(session, 0, 0, source, length, &error)
        || !markdown_core_session_commit(session, &changes, &error)) {
        put_error(&buffer, error);
        markdown_core_session_free(session);
        return finish(&buffer, output, output_length);
    }
    put_u8(&buffer, 0);
    put_u64(&buffer, markdown_core_session_lineage(session));
    put_u64(&buffer, session_root_id(session));
    encode_commit_body(&buffer, session, changes);
    markdown_core_delta_free(changes);
    encode_scope_table(&buffer, session);
    markdown_core_session_free(session);
    return finish(&buffer, output, output_length);
}

void markdown_core_kotlin_free(uint8_t *output) { free(output); }

markdown_core_kotlin_session *markdown_core_kotlin_session_open(uint32_t options_mask) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_session *session;

    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);
    session = markdown_core_session_open(&options, &error);
    if (session == NULL) {
        markdown_core_error_free(error);
        return NULL;
    }
    return (markdown_core_kotlin_session *)session;
}

void markdown_core_kotlin_session_free(markdown_core_kotlin_session *session) {
    markdown_core_session_free(to_session(session));
}

uint64_t markdown_core_kotlin_session_lineage(const markdown_core_kotlin_session *session) {
    return markdown_core_session_lineage(to_const_session(session));
}

uint64_t markdown_core_kotlin_session_revision(const markdown_core_kotlin_session *session) {
    return markdown_core_session_revision(to_const_session(session));
}

uint64_t markdown_core_kotlin_session_length(const markdown_core_kotlin_session *session) {
    return (uint64_t)markdown_core_session_length(to_const_session(session));
}

uint64_t markdown_core_kotlin_session_root(const markdown_core_kotlin_session *session) {
    return session_root_id(to_const_session(session));
}

bool markdown_core_kotlin_session_edit(markdown_core_kotlin_session *session, uint64_t byte_start,
                                       uint64_t byte_end, const uint8_t *bytes, size_t length,
                                       uint8_t **output, size_t *output_length) {
    markdown_core_error *error = NULL;
    bridge_buffer buffer = {0};

    put_magic(&buffer);
    if (byte_start > SIZE_MAX || byte_end > SIZE_MAX) {
        put_argument_error(&buffer, "edit range exceeds the platform address space");
    } else if (markdown_core_session_edit(to_session(session), (size_t)byte_start, (size_t)byte_end, bytes,
                                          length, &error)) {
        put_u8(&buffer, 0);
    } else {
        put_error(&buffer, error);
    }
    return finish(&buffer, output, output_length);
}

bool markdown_core_kotlin_session_commit(markdown_core_kotlin_session *session, uint8_t **output,
                                         size_t *output_length) {
    markdown_core_error *error = NULL;
    markdown_core_delta *changes = NULL;
    bridge_buffer buffer = {0};

    put_magic(&buffer);
    if (!markdown_core_session_commit(to_session(session), &changes, &error)) {
        put_error(&buffer, error);
        return finish(&buffer, output, output_length);
    }
    put_u8(&buffer, 0);
    encode_commit_body(&buffer, to_session(session), changes);
    markdown_core_delta_free(changes);
    return finish(&buffer, output, output_length);
}

bool markdown_core_kotlin_session_scopes(const markdown_core_kotlin_session *session, uint8_t **output,
                                         size_t *output_length) {
    bridge_buffer buffer = {0};

    put_magic(&buffer);
    put_u8(&buffer, 0);
    encode_scope_table(&buffer, to_const_session(session));
    return finish(&buffer, output, output_length);
}

bool markdown_core_kotlin_session_footnote_info(const markdown_core_kotlin_session *session, uint64_t id,
                                                uint8_t **output, size_t *output_length) {
    markdown_core_footnote_info info;
    bool found;
    bridge_buffer buffer = {0};

    put_magic(&buffer);
    put_u8(&buffer, 0);
    found = markdown_core_session_footnote_info(to_const_session(session), id, &info);
    put_u8(&buffer, found ? 1 : 0);
    if (found) {
        put_u64(&buffer, info.definition);
        put_u64(&buffer, info.number);
        put_u64(&buffer, info.reference_ordinal);
        put_u64(&buffer, info.reference_count);
    }
    return finish(&buffer, output, output_length);
}

static bool encode_id_array(const markdown_core_node_id *ids, size_t count, uint8_t **output,
                            size_t *output_length) {
    bridge_buffer buffer = {0};
    size_t index;

    put_magic(&buffer);
    put_u8(&buffer, 0);
    if (count > INT32_MAX) {
        buffer.failed = true;
        return finish(&buffer, output, output_length);
    }
    put_i32(&buffer, (int32_t)count);
    for (index = 0; index < count; ++index) {
        put_u64(&buffer, ids[index]);
    }
    return finish(&buffer, output, output_length);
}

bool markdown_core_kotlin_session_footnotes(const markdown_core_kotlin_session *session, uint8_t **output,
                                            size_t *output_length) {
    const markdown_core_node_id *ids = NULL;
    size_t count = markdown_core_session_footnotes(to_const_session(session), &ids);
    return encode_id_array(ids, count, output, output_length);
}

bool markdown_core_kotlin_session_footnote_references(const markdown_core_kotlin_session *session,
                                                      uint64_t definition, uint8_t **output,
                                                      size_t *output_length) {
    const markdown_core_node_id *ids = NULL;
    size_t count = markdown_core_session_footnote_references(to_const_session(session), definition, &ids);
    return encode_id_array(ids, count, output, output_length);
}
