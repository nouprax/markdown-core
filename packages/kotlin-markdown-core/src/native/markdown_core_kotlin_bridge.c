#include "markdown_core_kotlin_bridge.h"

#include "markdown_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* MKC5 wire: every payload opens with the magic and a status byte (0 =
 * success data follows, 1 = an error record follows). A success payload
 * carries the document handle, its series, and the root's id, revision and
 * extent, then the tree, then the diagnostics — the same shape for an open
 * and for an edit, because an edit's answer is simply the next document.
 *
 * Every payload carries the WHOLE tree, children before parents, which is the
 * order a decoder assembles immutable values in. There is no delta section:
 * what changed is asked of the tree itself, because a node's revision is the
 * document revision at which its own fields, child list, or any descendant
 * last changed — (id, revision) is the entire update protocol.
 *
 * Node records carry (kind, id, revision, scope, fields, child-id lists). The
 * scope is in the record because it belongs to the node; it had a table of its
 * own only to serve a decoder that skipped unchanged nodes, and that decoder
 * is gone — it made a document's projection a function of how it was reached
 * rather than of its text. */

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

/* Multi-byte scalars assemble little-endian in a stack buffer and append in
 * one reserve/memcpy; the wire bytes are identical to per-byte appends. */
static void encode_u32(uint8_t *bytes, uint32_t value) {
    size_t index;
    for (index = 0; index < 4; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

static void encode_u64(uint8_t *bytes, uint64_t value) {
    size_t index;
    for (index = 0; index < 8; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

static void put_u32(bridge_buffer *buffer, uint32_t value) {
    uint8_t bytes[4];
    encode_u32(bytes, value);
    put_bytes(buffer, bytes, sizeof(bytes));
}

static void put_i32(bridge_buffer *buffer, int32_t value) { put_u32(buffer, (uint32_t)value); }

static void put_u64(bridge_buffer *buffer, uint64_t value) {
    uint8_t bytes[8];
    encode_u64(bytes, value);
    put_bytes(buffer, bytes, sizeof(bytes));
}

static void put_i64(bridge_buffer *buffer, int64_t value) { put_u64(buffer, (uint64_t)value); }

static void put_scope(bridge_buffer *buffer, markdown_core_scope scope) {
    uint8_t bytes[16];
    encode_u32(bytes, (uint32_t)scope.start.line);
    encode_u32(bytes + 4, (uint32_t)scope.start.column);
    encode_u32(bytes + 8, (uint32_t)scope.end.line);
    encode_u32(bytes + 12, (uint32_t)scope.end.column);
    put_bytes(buffer, bytes, sizeof(bytes));
}

static void put_string(bridge_buffer *buffer, markdown_core_string value, bool present) {
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
    static const uint8_t magic[] = {'M', 'K', 'C', '5'};
    put_bytes(buffer, magic, sizeof(magic));
}

/* Consumes (frees) the error. */
static void put_error(bridge_buffer *buffer, markdown_core_error *error) {
    put_u8(buffer, 1);
    put_i32(buffer, error == NULL ? MARKDOWN_CORE_ERROR_INTERNAL : markdown_core_error_get_code(error));
    if (error == NULL) {
        markdown_core_string fallback = {(const uint8_t *)"markdown parsing failed", 23};
        put_string(buffer, fallback, true);
    } else {
        put_string(buffer, markdown_core_error_get_message(error), true);
    }
    markdown_core_error_free(error);
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

static void put_html_comment(bridge_buffer *buffer, const markdown_core_node *node) {
    /* Asked of the parser, never re-derived here: the one-complete-comment
     * rule belongs to the engine, and a second copy in each binding is a
     * second definition that can disagree with the first. */
    bool comment = false;
    markdown_core_node_html_comment(node, &comment);
    put_u8(buffer, comment ? 1 : 0);
}

static void write_record(bridge_buffer *buffer, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {0};
    markdown_core_string second = {0};
    markdown_core_string third = {0};

    put_u8(buffer, (uint8_t)kind);
    put_u64(buffer, markdown_core_node_get_id(node));
    put_u64(buffer, markdown_core_node_get_revision(node));
    /* The node's own extent, read in O(1) off the node. It rides in the
     * record because it belongs to the node; a separate table existed only to
     * serve a decoder that skipped nodes, and this one no longer does. */
    put_scope(buffer, markdown_core_node_scope(node));

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
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
    case MARKDOWN_CORE_KIND_HTML:
        markdown_core_node_literal(node, &first);
        put_html_comment(buffer, node);
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
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
        bool has_attributes = false;
        size_t count = 0;
        size_t index;
        markdown_core_node_directive_properties(node, &mode, &first, &has_attributes);
        put_i32(buffer, (int32_t)mode);
        put_string(buffer, first, true);
        // The attribute map: a presence byte, a count, then the pairs in the
        // order the engine holds them. No serialized form travels the wire,
        // because there is none -- the value IS the map.
        put_u8(buffer, has_attributes ? 1u : 0u);
        if (has_attributes) {
            markdown_core_node_directive_attribute_count(node, &count);
            if (count > INT32_MAX) {
                buffer->failed = true;
                return;
            }
            put_u32(buffer, (uint32_t)count);
            for (index = 0; index < count; index++) {
                /* The count is already on the wire. Stopping short would
                 * leave the decoder reading the next record's bytes as a
                 * string length, so a disagreement fails the payload. */
                if (!markdown_core_node_directive_attribute_at(node, index, &first, &second)) {
                    buffer->failed = true;
                    return;
                }
                put_string(buffer, first, true);
                put_string(buffer, second, true);
            }
        }
        put_child_ids(buffer, node);
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
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        markdown_core_node_cross_link_reference(node, &first);
        put_string(buffer, first, true);
        break;
    case MARKDOWN_CORE_KIND_EMBED:
        markdown_core_node_embed_reference(node, &first);
        put_string(buffer, first, true);
        break;
    /* A destination, a source and a definition's destination are always
     * written — an inline link always spells its `(...)` — so they go on the
     * wire as present. Only the titles are optional, and only they carry the
     * absent marker; the decoder reads the pairing exactly that way. */
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &first, &second);
        put_string(buffer, first, true);
        put_string(buffer, second, second.data != NULL);
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &first, &second);
        put_string(buffer, first, true);
        put_string(buffer, second, second.data != NULL);
        put_child_ids(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_reference_definition_properties(node, &first, &second, &third);
        put_string(buffer, first, true);
        put_string(buffer, second, true);
        put_string(buffer, third, third.data != NULL);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        markdown_core_node_reference_properties(node, &first, &form);
        put_string(buffer, first, true);
        put_u8(buffer, (uint8_t)form);
        put_child_ids(buffer, node);
        break;
    }
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

/* Postorder: children before parents, which is the order a decoder assembles
 * immutable values in. */
static const markdown_core_node *postorder_first(const markdown_core_node *root) {
    const markdown_core_node *child;
    while ((child = markdown_core_node_get_first_child(root)) != NULL) {
        root = child;
    }
    return root;
}

static const markdown_core_node *postorder_next(const markdown_core_node *node,
                                                const markdown_core_node *root) {
    const markdown_core_node *next;
    if (node == root) {
        return NULL;
    }
    next = markdown_core_node_get_next_sibling(node);
    if (next == NULL) {
        return markdown_core_node_get_parent(node);
    }
    return postorder_first(next);
}

static size_t postorder_count(const markdown_core_node *root) {
    const markdown_core_node *node = postorder_first(root);
    size_t count = 0;
    while (node != NULL) {
        count++;
        node = postorder_next(node, root);
    }
    return count;
}

static void encode_tree(bridge_buffer *buffer, const markdown_core_node *root) {
    const markdown_core_node *node;
    size_t count = postorder_count(root);
    if (count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)count);
    for (node = postorder_first(root); node != NULL && !buffer->failed;
         node = postorder_next(node, root)) {
        write_record(buffer, node);
    }
}

static void encode_diagnostics(bridge_buffer *buffer, const markdown_core_document *document) {
    const markdown_core_diagnostic *rows = NULL;
    size_t count = markdown_core_document_diagnostics(document, &rows);
    size_t index;

    if (count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    put_i32(buffer, (int32_t)count);
    for (index = 0; index < count && !buffer->failed; ++index) {
        put_i32(buffer, (int32_t)rows[index].code);
        put_scope(buffer, rows[index].scope);
    }
}

static void apply_options(markdown_core_parse_options *options, uint32_t mask) {
    options->smart_punctuation = (mask & (1u << 0)) != 0;
    options->footnotes = (mask & (1u << 1)) != 0;
    options->tables = (mask & (1u << 2)) != 0;
    options->strikethrough = (mask & (1u << 3)) != 0;
    options->autolinks = (mask & (1u << 4)) != 0;
    options->task_lists = (mask & (1u << 5)) != 0;
    options->formulas = (mask & (1u << 6)) != 0;
    options->directives = (mask & (1u << 7)) != 0;
    options->cross_links = (mask & (1u << 8)) != 0;
    options->embeds = (mask & (1u << 9)) != 0;
}

static markdown_core_document *document_of(uint64_t handle) {
    return (markdown_core_document *)(uintptr_t)handle;
}

/* Handle, series, root id, root revision — the header every success payload
 * carries, whichever body follows it.
 *
 * The ROOT NODE's revision, not the document's: the document's revision counts
 * commits, while a node's counts the commits that changed it, and an edit that
 * changes nothing advances the first and not the second. The root is a Markup
 * node like any other and answers by the node rule. */
static const markdown_core_node *put_header(bridge_buffer *buffer,
                                            const markdown_core_document *document) {
    const markdown_core_node *root = markdown_core_document_root(document);
    put_u8(buffer, 0);
    put_u64(buffer, (uint64_t)(uintptr_t)document);
    put_u64(buffer, markdown_core_document_series(document));
    if (root == NULL) {
        buffer->failed = true;
        return NULL;
    }
    put_u64(buffer, markdown_core_node_get_id(root));
    put_u64(buffer, markdown_core_node_get_revision(root));
    put_scope(buffer, markdown_core_node_scope(root));
    return root;
}

bool markdown_core_kotlin_open(const uint8_t *source, size_t length, uint32_t options_mask,
                               uint8_t **output, size_t *output_length) {
    markdown_core_parse_options options;
    markdown_core_error *error = NULL;
    markdown_core_document *document;
    markdown_core_string text;
    const markdown_core_node *root;
    bridge_buffer buffer = {0};

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;
    markdown_core_parse_options_init(&options);
    apply_options(&options, options_mask);

    put_magic(&buffer);
    text.data = source;
    text.length = length;
    document = markdown_core_document_new(text, &options, &error);
    if (document == NULL) {
        put_error(&buffer, error);
        return finish(&buffer, output, output_length);
    }
    root = put_header(&buffer, document);
    if (root != NULL) {
        encode_tree(&buffer, root);
        encode_diagnostics(&buffer, document);
    }
    if (buffer.failed) {
        /* The handle never reaches the caller, so this call owns it. */
        markdown_core_document_free(document);
    }
    return finish(&buffer, output, output_length);
}

bool markdown_core_kotlin_edit(uint64_t handle, const uint8_t *source, size_t length,
                               uint8_t **output, size_t *output_length) {
    markdown_core_document *document = document_of(handle);
    markdown_core_document *successor;
    markdown_core_error *error = NULL;
    markdown_core_string text;
    const markdown_core_node *root;
    bridge_buffer buffer = {0};

    if (output == NULL || output_length == NULL) {
        return false;
    }
    *output = NULL;
    *output_length = 0;

    put_magic(&buffer);
    text.data = source;
    text.length = length;
    successor = markdown_core_document_edit(document, text, &error);
    if (successor == NULL) {
        put_error(&buffer, error);
        return finish(&buffer, output, output_length);
    }
    root = put_header(&buffer, successor);
    if (root != NULL) {
        encode_tree(&buffer, root);
        encode_diagnostics(&buffer, successor);
    }
    if (buffer.failed) {
        /* The handle never reaches the caller, so this call owns it. */
        markdown_core_document_free(successor);
    }
    return finish(&buffer, output, output_length);
}

void markdown_core_kotlin_release(uint64_t handle) {
    markdown_core_document_free(document_of(handle));
}

void markdown_core_kotlin_free(uint8_t *output) { free(output); }
