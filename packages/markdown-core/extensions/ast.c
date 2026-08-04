#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "session_internal.h"
#include "cross_reference.h"
#include "directive.h"
#include "formula.h"
#include "markdown-core-extensions.h"
#include "strikethrough.h"
#include "table.h"

#include <markdown-core.h>
#include <node.h>
#include <parser.h>

struct markdown_core_error {
    markdown_core_error_code code;
    char *message;
    bool has_scope;
    markdown_core_scope scope;
};

typedef struct dump_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} dump_buffer;

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

static void set_error(markdown_core_error **error, markdown_core_error_code code, const char *message) {
    markdown_core_error *value;
    size_t length;
    if (!error) {
        return;
    }
    value = (markdown_core_error *)calloc(1, sizeof(*value));
    if (!value) {
        return;
    }
    length = strlen(message);
    value->message = (char *)malloc(length + 1);
    if (!value->message) {
        free(value);
        return;
    }
    memcpy(value->message, message, length + 1);
    value->code = code;
    *error = value;
}

void markdown_core_ast_set_error(markdown_core_error **error, markdown_core_error_code code, const char *message) {
    set_error(error, code, message);
}

void markdown_core_parse_options_init(markdown_core_parse_options *options) {
    if (!options) {
        return;
    }
    options->smart_punctuation = true;
    options->footnotes = true;
    options->strip_html_comments = true;
    options->tables = true;
    options->strikethrough = true;
    options->autolinks = true;
    options->task_lists = true;
    options->formulas = true;
    options->directives = true;
    options->cross_links = true;
    options->embeds = true;
}

markdown_core_document *markdown_core_document_parse(
    const uint8_t *source,
    size_t length,
    const markdown_core_parse_options *options,
    markdown_core_error **error
) {
    markdown_core_session *session;
    markdown_core_document *document;

    clear_error(error);
    if (!source && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "source must not be null when length is nonzero");
        return NULL;
    }
    // Unpooled: the detached tree must outlive the session, and Document.parse
    // keeps its v1 memory profile (no arena rides along with the document).
    session = markdown_core_session_open_with_mem(options, markdown_core_mem_default(), false, error);
    if (!session) {
        return NULL;
    }
    // A one-shot parse never commits again, so session indexes and per-unit
    // lookup records would be pure overhead.
    session->one_shot = true;
    session->record_lookups = false;
    if (length && (!markdown_core_session_edit(session, 0, 0, source, length, error) ||
                   !markdown_core_session_commit(session, NULL, error))) {
        markdown_core_session_free(session);
        return NULL;
    }
    document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        markdown_core_session_free(session);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    // Detach the committed tree; nodes keep their session-assigned ids.
    document->root = session->view.root;
    session->view.root = NULL;
    markdown_core_session_free(session);
    return document;
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    markdown_core_node_free(document->root);
    free(document);
}

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) {
    return document ? document->root : NULL;
}

const markdown_core_node *markdown_core_document_concrete(const markdown_core_document *document) {
    /* Internal boundary: callers hold a parsed document, so there is no NULL
     * to tolerate — the semantic root and the concrete owner are the same
     * retained tree (ast_internal.h). */
    return document->root;
}

markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error) {
    return error ? error->code : MARKDOWN_CORE_ERROR_NONE;
}

markdown_core_string_view markdown_core_error_get_message(const markdown_core_error *error) {
    markdown_core_string_view view = {NULL, 0};
    if (error && error->message) {
        view.data = (const uint8_t *)error->message;
        view.length = strlen(error->message);
    }
    return view;
}

bool markdown_core_error_get_scope(const markdown_core_error *error, markdown_core_scope *scope) {
    if (!error || !error->has_scope || !scope) {
        return false;
    }
    *scope = error->scope;
    return true;
}

void markdown_core_error_free(markdown_core_error *error) {
    if (!error) {
        return;
    }
    free(error->message);
    free(error);
}

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) {
    if (!node) {
        return MARKDOWN_CORE_KIND_NONE;
    }
    switch (node->type) {
    case MARKDOWN_CORE_NODE_DOCUMENT:
        return MARKDOWN_CORE_KIND_DOCUMENT;
    case MARKDOWN_CORE_NODE_BLOCK_QUOTE:
        return MARKDOWN_CORE_KIND_BLOCK_QUOTE;
    case MARKDOWN_CORE_NODE_PARAGRAPH:
        return MARKDOWN_CORE_KIND_PARAGRAPH;
    case MARKDOWN_CORE_NODE_HEADING:
        return MARKDOWN_CORE_KIND_HEADING;
    case MARKDOWN_CORE_NODE_THEMATIC_BREAK:
        return MARKDOWN_CORE_KIND_THEMATIC_BREAK;
    case MARKDOWN_CORE_NODE_LIST:
        return MARKDOWN_CORE_KIND_LIST;
    case MARKDOWN_CORE_NODE_LIST_ITEM:
        return MARKDOWN_CORE_KIND_LIST_ITEM;
    case MARKDOWN_CORE_NODE_CODE_BLOCK:
        return MARKDOWN_CORE_KIND_CODE_BLOCK;
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
        return MARKDOWN_CORE_KIND_HTML_BLOCK;
    case MARKDOWN_CORE_NODE_FORMULA_BLOCK:
        return MARKDOWN_CORE_KIND_FORMULA_BLOCK;
    case MARKDOWN_CORE_NODE_TABLE:
        return MARKDOWN_CORE_KIND_TABLE;
    case MARKDOWN_CORE_NODE_TABLE_ROW:
        return MARKDOWN_CORE_KIND_TABLE_ROW;
    case MARKDOWN_CORE_NODE_TABLE_CELL:
        return MARKDOWN_CORE_KIND_TABLE_CELL;
    case MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK:
        return MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK;
    case MARKDOWN_CORE_NODE_DIRECTIVE_LABEL:
        return MARKDOWN_CORE_KIND_DIRECTIVE_LABEL;
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
        return MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION;
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        return MARKDOWN_CORE_KIND_REFERENCE_DEFINITION;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
        return MARKDOWN_CORE_KIND_LINK_REFERENCE;
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        return MARKDOWN_CORE_KIND_IMAGE_REFERENCE;
    case MARKDOWN_CORE_NODE_TEXT:
        return MARKDOWN_CORE_KIND_TEXT;
    case MARKDOWN_CORE_NODE_SOFT_BREAK:
        return MARKDOWN_CORE_KIND_SOFT_BREAK;
    case MARKDOWN_CORE_NODE_LINE_BREAK:
        return MARKDOWN_CORE_KIND_LINE_BREAK;
    case MARKDOWN_CORE_NODE_CODE:
        return MARKDOWN_CORE_KIND_CODE;
    case MARKDOWN_CORE_NODE_HTML:
        return MARKDOWN_CORE_KIND_HTML;
    case MARKDOWN_CORE_NODE_FORMULA:
        return MARKDOWN_CORE_KIND_FORMULA;
    case MARKDOWN_CORE_NODE_EMPHASIS:
        return MARKDOWN_CORE_KIND_EMPHASIS;
    case MARKDOWN_CORE_NODE_STRONG:
        return MARKDOWN_CORE_KIND_STRONG;
    case MARKDOWN_CORE_NODE_STRIKETHROUGH:
        return MARKDOWN_CORE_KIND_STRIKETHROUGH;
    case MARKDOWN_CORE_NODE_LINK:
        return MARKDOWN_CORE_KIND_LINK;
    case MARKDOWN_CORE_NODE_IMAGE:
        return MARKDOWN_CORE_KIND_IMAGE;
    case MARKDOWN_CORE_NODE_DIRECTIVE:
        return MARKDOWN_CORE_KIND_DIRECTIVE;
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        return MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE;
    case MARKDOWN_CORE_NODE_CROSS_LINK:
        return MARKDOWN_CORE_KIND_CROSS_LINK;
    case MARKDOWN_CORE_NODE_EMBED:
        return MARKDOWN_CORE_KIND_EMBED;
    default:
        return MARKDOWN_CORE_KIND_NONE;
    }
}

const char *markdown_core_node_kind_name(markdown_core_node_kind kind) {
    static const char *const names[] = {
        "None",
        "Document",
        "BlockQuote",
        "Paragraph",
        "Heading",
        "ThematicBreak",
        "List",
        "ListItem",
        "CodeBlock",
        "HTMLBlock",
        "FormulaBlock",
        "Table",
        "TableRow",
        "TableCell",
        "DirectiveBlock",
        "DirectiveLabel",
        "FootnoteDefinition",
        "Text",
        "SoftBreak",
        "LineBreak",
        "Code",
        "HTML",
        "Formula",
        "Emphasis",
        "Strong",
        "Strikethrough",
        "Link",
        "Image",
        "Directive",
        "FootnoteReference",
        "CrossLink",
        "Embed",
        "ReferenceDefinition",
        "LinkReference",
        "ImageReference"
    };
    if (kind < MARKDOWN_CORE_KIND_NONE || kind > MARKDOWN_CORE_KIND_IMAGE_REFERENCE) {
        return "None";
    }
    return names[kind];
}

markdown_core_scope markdown_core_node_scope(const markdown_core_node *node) {
    markdown_core_scope scope = {{0, 0}, {0, 0}};
    const markdown_core_node *ancestor;
    int start_line, end_line;
    if (!node) {
        return scope;
    }
    start_line = node->start_line;
    end_line = node->end_line;
    if (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
        // Sealed storage is parent-relative (see node.h); the root's start
        // line stays absolute, so summing the parent chain resolves the
        // absolute start. An unsealed ancestor (a position-free node keeping
        // raw zeros) resolves to its own raw fields, so it contributes once
        // and ends the walk — exactly how the dump's accumulator resolves.
        for (ancestor = node->parent; ancestor; ancestor = ancestor->parent) {
            start_line += ancestor->start_line;
            if (!(ancestor->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE)) {
                break;
            }
        }
        end_line += start_line;
    }
    scope.start.line = start_line;
    scope.start.column = node->start_column;
    scope.end.line = end_line;
    scope.end.column = node->end_column;
    return scope;
}

static markdown_core_scope scope_with_parent_start(const markdown_core_node *node, int32_t parent_resolved_start_line) {
    markdown_core_scope scope = {{0, 0}, {0, 0}};
    int start_line, end_line;
    if (!node) {
        return scope;
    }
    start_line = node->start_line;
    end_line = node->end_line;
    if (node->flags & MARKDOWN_CORE_NODE__SEALED_RELATIVE) {
        start_line += parent_resolved_start_line;
        end_line += start_line;
    }
    scope.start.line = start_line;
    scope.start.column = node->start_column;
    scope.end.line = end_line;
    scope.end.column = node->end_column;
    return scope;
}

markdown_core_node_id markdown_core_node_get_id(const markdown_core_node *node) { return node ? node->id : 0; }

uint64_t markdown_core_node_get_revision(const markdown_core_node *node) { return node ? node->last_changed_rev : 0; }

const markdown_core_node *markdown_core_node_get_parent(const markdown_core_node *node) {
    return node ? node->parent : NULL;
}

const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node) {
    return node ? node->first_child : NULL;
}

const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node) {
    return node ? node->next : NULL;
}

size_t markdown_core_node_child_count(const markdown_core_node *node) {
    const markdown_core_node *child = markdown_core_node_get_first_child(node);
    size_t count = 0;
    while (child) {
        count++;
        child = markdown_core_node_get_next_sibling(child);
    }
    return count;
}

#define CANONICAL_WALK_INLINE_DEPTH 64
#define SCOPE_TABLE_INITIAL_CAPACITY 64

typedef struct canonical_walk_frame {
    int32_t resolved_start_line;
    const markdown_core_node *next_sibling;
} canonical_walk_frame;

typedef struct canonical_walk {
    const markdown_core_node *root;
    const markdown_core_node *next;
    canonical_walk_frame *frames;
    canonical_walk_frame inline_frames[CANONICAL_WALK_INLINE_DEPTH];
    size_t depth;
    size_t capacity;
    bool failed;
} canonical_walk;

/**
 * One streaming canonical-preorder traversal serves every AST consumer.
 * A frame per active depth carries both the resolved parent line needed by
 * descendants and the pending canonical sibling needed after ascent. That
 * same sibling state also renders dump connectors, so the dump owns no
 * second traversal stack.
 */
static void canonical_walk_init(canonical_walk *walk, const markdown_core_node *root) {
    memset(walk, 0, sizeof(*walk));
    walk->frames = walk->inline_frames;
    walk->capacity = CANONICAL_WALK_INLINE_DEPTH;
    walk->root = root;
    walk->next = root;
}

static bool canonical_walk_resize(canonical_walk *walk, size_t capacity) {
    canonical_walk_frame *resized;

    if (capacity <= walk->capacity) {
        return true;
    }
    if (capacity > SIZE_MAX / sizeof(*walk->frames)) {
        walk->failed = true;
        return false;
    }
    if (walk->frames == walk->inline_frames) {
        resized = (canonical_walk_frame *)malloc(capacity * sizeof(*resized));
        if (resized) {
            memcpy(resized, walk->inline_frames, walk->depth * sizeof(*resized));
        }
    } else {
        resized = (canonical_walk_frame *)realloc(walk->frames, capacity * sizeof(*resized));
    }
    if (!resized) {
        walk->failed = true;
        return false;
    }
    walk->frames = resized;
    walk->capacity = capacity;
    return true;
}

static bool canonical_walk_reserve_current_depth(canonical_walk *walk) {
    size_t needed;
    size_t capacity;

    if (walk->depth == SIZE_MAX) {
        walk->failed = true;
        return false;
    }
    needed = walk->depth + 1;
    if (needed <= walk->capacity) {
        return true;
    }
    capacity = walk->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    return canonical_walk_resize(walk, capacity);
}

static void canonical_walk_dispose(canonical_walk *walk) {
    if (walk->frames != walk->inline_frames) {
        free(walk->frames);
    }
}

/**
 * Emits one canonical-preorder node and resolves its scope from the
 * canonical parent's already-resolved start line. This is the sole linear
 * canonical traversal used by both public materialization and canonical
 * dumps.
 */
static bool canonical_walk_next(
    canonical_walk *walk,
    const markdown_core_node **node,
    markdown_core_scope *scope,
    size_t *depth,
    bool *has_next
) {
    const markdown_core_node *current = walk->next;
    const markdown_core_node *child;
    const markdown_core_node *sibling;
    canonical_walk_frame *frame;
    int32_t parent_start_line;

    if (!current) {
        return false;
    }
    if (!canonical_walk_reserve_current_depth(walk)) {
        return false;
    }
    parent_start_line = walk->depth ? walk->frames[walk->depth - 1].resolved_start_line : 0;
    *scope = scope_with_parent_start(current, parent_start_line);
    sibling = current == walk->root ? NULL : markdown_core_node_get_next_sibling(current);
    frame = &walk->frames[walk->depth];
    frame->resolved_start_line = scope->start.line;
    frame->next_sibling = sibling;
    *node = current;
    if (depth) {
        *depth = walk->depth;
    }
    if (has_next) {
        *has_next = sibling != NULL;
    }
    child = markdown_core_node_get_first_child(current);
    if (child) {
        walk->depth++;
        walk->next = child;
        return true;
    }
    while (!walk->frames[walk->depth].next_sibling) {
        if (walk->depth == 0) {
            walk->next = NULL;
            return true;
        }
        walk->depth--;
    }
    walk->next = walk->frames[walk->depth].next_sibling;
    return true;
}

static bool canonical_walk_branch_continues(const canonical_walk *walk, size_t depth) {
    return walk->frames[depth].next_sibling != NULL;
}

static bool scope_table_append(
    markdown_core_scope_entry **entries,
    size_t *count,
    size_t *capacity,
    const markdown_core_node *node,
    markdown_core_scope scope
) {
    const size_t maximum = SIZE_MAX / sizeof(**entries);
    markdown_core_scope_entry *resized;
    size_t new_capacity;

    if (*count == *capacity) {
        if (*capacity == maximum) {
            return false;
        }
        new_capacity = *capacity ? *capacity * 2 : SCOPE_TABLE_INITIAL_CAPACITY;
        if (new_capacity < *capacity || new_capacity > maximum) {
            new_capacity = maximum;
        }
        resized = (markdown_core_scope_entry *)realloc(*entries, new_capacity * sizeof(**entries));
        if (!resized) {
            return false;
        }
        *entries = resized;
        *capacity = new_capacity;
    }
    (*entries)[*count].id = markdown_core_node_get_id(node);
    (*entries)[*count].revision = markdown_core_node_get_revision(node);
    (*entries)[*count].scope = scope;
    (*count)++;
    return true;
}

bool markdown_core_document_scope_table(
    const markdown_core_document *document,
    markdown_core_scope_entry **output,
    size_t *count,
    markdown_core_error **error
) {
    canonical_walk walk;
    markdown_core_scope_entry *entries;
    const markdown_core_node *node;
    markdown_core_scope scope;
    size_t capacity = 0;
    size_t index = 0;

    clear_error(error);
    if (output) {
        *output = NULL;
    }
    if (count) {
        *count = 0;
    }
    if (!document || !document->root || !output || !count) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document, output, and count must not be null");
        return false;
    }
    canonical_walk_init(&walk, document->root);
    entries = NULL;
    while (canonical_walk_next(&walk, &node, &scope, NULL, NULL)) {
        if (!scope_table_append(&entries, &index, &capacity, node, scope)) {
            free(entries);
            canonical_walk_dispose(&walk);
            set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate scope table");
            return false;
        }
    }
    if (walk.failed) {
        free(entries);
        canonical_walk_dispose(&walk);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate scope traversal");
        return false;
    }
    canonical_walk_dispose(&walk);
    *output = entries;
    *count = index;
    return true;
}

void markdown_core_scope_table_free(markdown_core_scope_entry *output) { free(output); }

bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level) {
    if (!node || node->type != MARKDOWN_CORE_NODE_HEADING || !level) {
        return false;
    }
    *level = node->as.heading.level;
    return true;
}

bool markdown_core_node_list_properties(
    const markdown_core_node *node,
    markdown_core_list_flavor *flavor,
    markdown_core_optional_i64 *start,
    bool *tight
) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST || !flavor || !start || !tight) {
        return false;
    }
    *flavor = node->as.list.list_type == MARKDOWN_CORE_ORDERED_LIST ? MARKDOWN_CORE_LIST_FLAVOR_ORDERED
                                                                    : MARKDOWN_CORE_LIST_FLAVOR_BULLET;
    start->has_value = *flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    start->value = node->as.list.start;
    *tight = node->as.list.tight;
    return true;
}

bool markdown_core_node_list_item_checked(const markdown_core_node *node, markdown_core_optional_bool *checked) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST_ITEM || !checked) {
        return false;
    }
    checked->has_value =
        node->extension && strcmp(markdown_core_node_get_type_string((markdown_core_node *)node), "tasklist") == 0;
    checked->value = checked->has_value && node->as.list.checked;
    return true;
}

static void view_chunk(markdown_core_string_view *view, const markdown_core_chunk *chunk) {
    view->data = chunk->data;
    view->length = chunk->len < 0 ? 0 : (size_t)chunk->len;
}

bool markdown_core_node_code_block_properties(
    const markdown_core_node *node,
    markdown_core_string_view *info,
    markdown_core_string_view *language,
    markdown_core_string_view *literal,
    bool *fenced,
    bool *closed
) {
    size_t start = 0;
    size_t end;
    if (!node || node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !info || !language || !literal || !fenced || !closed) {
        return false;
    }
    view_chunk(info, &node->as.code.info);
    view_chunk(literal, &node->as.code.literal);
    if (info->length == 0) {
        info->data = NULL;
    }
    language->data = NULL;
    language->length = 0;
    while (start < info->length && (info->data[start] == ' ' || info->data[start] == '\t' ||
                                    info->data[start] == '\n' || info->data[start] == '\r')) {
        start++;
    }
    end = start;
    while (end < info->length && info->data[end] != ' ' && info->data[end] != '\t' && info->data[end] != '\n' &&
           info->data[end] != '\r') {
        end++;
    }
    if (end > start) {
        language->data = info->data + start;
        language->length = end - start;
    }
    *fenced = node->as.code.fenced != 0;
    *closed = !*fenced || node->as.code.fence_closed != 0;
    return true;
}

bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string_view *literal) {
    if (!node || !literal) {
        return false;
    }
    switch (node->type) {
    case MARKDOWN_CORE_NODE_HTML_BLOCK:
    case MARKDOWN_CORE_NODE_TEXT:
    case MARKDOWN_CORE_NODE_HTML:
    case MARKDOWN_CORE_NODE_CODE:
        view_chunk(literal, &node->as.literal);
        return true;
    default:
        return false;
    }
}

bool markdown_core_node_formula_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string_view *literal
) {
    const char *value;
    markdown_core_formula_mode native_mode;
    if (!node || !mode || !literal ||
        (node->type != MARKDOWN_CORE_NODE_FORMULA && node->type != MARKDOWN_CORE_NODE_FORMULA_BLOCK)) {
        return false;
    }
    native_mode = markdown_core_extensions_get_formula_mode((markdown_core_node *)node);
    *mode = native_mode == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                               : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    value = markdown_core_extensions_get_formula_literal((markdown_core_node *)node);
    literal->data = (const uint8_t *)value;
    literal->length = value ? strlen(value) : 0;
    return true;
}

bool markdown_core_node_table_column_count(const markdown_core_node *node, size_t *count) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !count) {
        return false;
    }
    *count = markdown_core_extensions_get_table_columns((markdown_core_node *)node);
    return true;
}

bool markdown_core_node_table_alignment_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_table_alignment *alignment
) {
    uint16_t count;
    uint8_t *alignments;
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !alignment) {
        return false;
    }
    count = markdown_core_extensions_get_table_columns((markdown_core_node *)node);
    if (index >= count) {
        return false;
    }
    alignments = markdown_core_extensions_get_table_alignments((markdown_core_node *)node);
    switch (alignments[index]) {
    case 'l':
        *alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT;
        break;
    case 'c':
        *alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER;
        break;
    case 'r':
        *alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT;
        break;
    default:
        *alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
        break;
    }
    return true;
}

bool markdown_core_node_table_row_is_header(const markdown_core_node *node, bool *is_header) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_ROW || !is_header) {
        return false;
    }
    *is_header = markdown_core_extensions_get_table_row_is_header((markdown_core_node *)node) != 0;
    return true;
}

bool markdown_core_node_directive_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string_view *name,
    markdown_core_string_view *attributes
) {
    const char *value;
    if (!node || !mode || !name || !attributes ||
        (node->type != MARKDOWN_CORE_NODE_DIRECTIVE && node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)) {
        return false;
    }
    *mode = node->type == MARKDOWN_CORE_NODE_DIRECTIVE ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                       : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    value = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
    name->data = (const uint8_t *)value;
    name->length = value ? strlen(value) : 0;
    value = markdown_core_extensions_get_directive_attributes((markdown_core_node *)node);
    attributes->data = (const uint8_t *)value;
    attributes->length = value ? strlen(value) : 0;
    return true;
}

const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node) {
    return markdown_core_directive_label((markdown_core_node *)node);
}

static bool link_properties(
    const markdown_core_node *node,
    uint16_t expected,
    markdown_core_string_view *url,
    markdown_core_string_view *title
) {
    if (!node || node->type != expected || !url || !title) {
        return false;
    }
    view_chunk(url, &node->as.link.url);
    view_chunk(title, &node->as.link.title);
    return true;
}

static const char *reference_form_name(markdown_core_reference_form form) {
    switch (form) {
    case MARKDOWN_CORE_REFERENCE_FULL:
        return "full";
    case MARKDOWN_CORE_REFERENCE_COLLAPSED:
        return "collapsed";
    case MARKDOWN_CORE_REFERENCE_SHORTCUT:
        break;
    }
    return "shortcut";
}

bool markdown_core_node_reference_definition_properties(
    const markdown_core_node *node,
    markdown_core_string_view *label,
    markdown_core_string_view *destination,
    markdown_core_string_view *title
) {
    if (!node || node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION || !label || !destination || !title) {
        return false;
    }
    view_chunk(label, &node->as.definition.label);
    view_chunk(destination, &node->as.definition.url);
    view_chunk(title, &node->as.definition.title);
    return true;
}

bool markdown_core_node_reference_properties(
    const markdown_core_node *node,
    markdown_core_string_view *label,
    markdown_core_reference_form *form
) {
    if (!node || !label || !form ||
        (node->type != MARKDOWN_CORE_NODE_LINK_REFERENCE && node->type != MARKDOWN_CORE_NODE_IMAGE_REFERENCE)) {
        return false;
    }
    view_chunk(label, &node->as.reference.label);
    switch (node->as.reference.form) {
    case MARKDOWN_CORE_FULL_REFERENCE:
        *form = MARKDOWN_CORE_REFERENCE_FULL;
        break;
    case MARKDOWN_CORE_COLLAPSED_REFERENCE:
        *form = MARKDOWN_CORE_REFERENCE_COLLAPSED;
        break;
    case MARKDOWN_CORE_SHORTCUT_REFERENCE:
        *form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        break;
    }
    return true;
}

bool markdown_core_node_link_properties(
    const markdown_core_node *node,
    markdown_core_string_view *destination,
    markdown_core_string_view *title
) {
    return link_properties(node, MARKDOWN_CORE_NODE_LINK, destination, title);
}

bool markdown_core_node_image_properties(
    const markdown_core_node *node,
    markdown_core_string_view *source,
    markdown_core_string_view *title
) {
    return link_properties(node, MARKDOWN_CORE_NODE_IMAGE, source, title);
}

bool markdown_core_node_footnote_id(const markdown_core_node *node, markdown_core_string_view *id) {
    if (!node || !id ||
        (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION && node->type != MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE)) {
        return false;
    }
    // Both kinds carry their own label exactly as written in the source;
    // resolution and numbering are session queries, not node content.
    view_chunk(id, &node->as.literal);
    return true;
}

static bool cross_reference(
    const markdown_core_node *node,
    markdown_core_node_type type,
    markdown_core_string_view *reference
) {
    const markdown_core_chunk *value;
    if (!node || !reference || node->type != type) {
        return false;
    }
    value = markdown_core_cross_reference_value((markdown_core_node *)node);
    if (!value) {
        return false;
    }
    view_chunk(reference, value);
    return true;
}

bool markdown_core_node_cross_link_reference(const markdown_core_node *node, markdown_core_string_view *reference) {
    return cross_reference(node, MARKDOWN_CORE_NODE_CROSS_LINK, reference);
}

bool markdown_core_node_embed_reference(const markdown_core_node *node, markdown_core_string_view *reference) {
    return cross_reference(node, MARKDOWN_CORE_NODE_EMBED, reference);
}

static void buffer_reserve(dump_buffer *buffer, size_t additional) {
    size_t needed;
    size_t capacity;
    uint8_t *data;
    if (buffer->failed || additional > SIZE_MAX - buffer->size - 1) {
        buffer->failed = true;
        return;
    }
    needed = buffer->size + additional + 1;
    if (needed <= buffer->capacity) {
        return;
    }
    capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    data = (uint8_t *)realloc(buffer->data, capacity);
    if (!data) {
        buffer->failed = true;
        return;
    }
    buffer->data = data;
    buffer->capacity = capacity;
}

static void buffer_bytes(dump_buffer *buffer, const void *bytes, size_t length) {
    buffer_reserve(buffer, length);
    if (buffer->failed) {
        return;
    }
    if (length) {
        memcpy(buffer->data + buffer->size, bytes, length);
    }
    buffer->size += length;
    buffer->data[buffer->size] = 0;
}

static void buffer_cstr(dump_buffer *buffer, const char *value) { buffer_bytes(buffer, value, strlen(value)); }

static void buffer_i64(dump_buffer *buffer, int64_t value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length > 0) {
        buffer_bytes(buffer, text, (size_t)length);
    }
}

static void buffer_json_string(dump_buffer *buffer, markdown_core_string_view value) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    buffer_cstr(buffer, "\"");
    for (i = 0; i < value.length; i++) {
        uint8_t c = value.data[i];
        switch (c) {
        case '\"':
            buffer_cstr(buffer, "\\\"");
            break;
        case '\\':
            buffer_cstr(buffer, "\\\\");
            break;
        case '\b':
            buffer_cstr(buffer, "\\b");
            break;
        case '\f':
            buffer_cstr(buffer, "\\f");
            break;
        case '\n':
            buffer_cstr(buffer, "\\n");
            break;
        case '\r':
            buffer_cstr(buffer, "\\r");
            break;
        case '\t':
            buffer_cstr(buffer, "\\t");
            break;
        default:
            if (c < 0x20) {
                char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0xf]};
                buffer_bytes(buffer, escaped, sizeof(escaped));
            } else {
                buffer_bytes(buffer, &c, 1);
            }
            break;
        }
    }
    buffer_cstr(buffer, "\"");
}

static void buffer_optional_string(dump_buffer *buffer, markdown_core_string_view value) {
    if (!value.data) {
        buffer_cstr(buffer, "null");
    } else {
        buffer_json_string(buffer, value);
    }
}

static const char *alignment_name(markdown_core_table_alignment alignment) {
    switch (alignment) {
    case MARKDOWN_CORE_TABLE_ALIGNMENT_LEFT:
        return "left";
    case MARKDOWN_CORE_TABLE_ALIGNMENT_CENTER:
        return "center";
    case MARKDOWN_CORE_TABLE_ALIGNMENT_RIGHT:
        return "right";
    default:
        return "none";
    }
}

static const char *mode_name(markdown_core_placement_mode mode) {
    return mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? "embedded" : "standalone";
}

static void dump_fields(dump_buffer *buffer, const markdown_core_node *node, markdown_core_node_kind kind) {
    markdown_core_string_view a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0};
    markdown_core_optional_i64 start;
    markdown_core_optional_bool checked;
    markdown_core_list_flavor flavor;
    markdown_core_placement_mode mode;
    markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
    bool x, y;
    size_t count, i;
    int32_t level;
    switch (kind) {
    case MARKDOWN_CORE_KIND_HEADING:
        markdown_core_node_heading_level(node, &level);
        buffer_cstr(buffer, " level=");
        buffer_i64(buffer, level);
        break;
    case MARKDOWN_CORE_KIND_LIST:
        markdown_core_node_list_properties(node, &flavor, &start, &x);
        buffer_cstr(buffer, " flavor=");
        buffer_cstr(buffer, flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? "ordered" : "bullet");
        buffer_cstr(buffer, " start=");
        if (start.has_value) {
            buffer_i64(buffer, start.value);
        } else {
            buffer_cstr(buffer, "null");
        }
        buffer_cstr(buffer, " tight=");
        buffer_cstr(buffer, x ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_checked(node, &checked);
        buffer_cstr(buffer, " checked=");
        if (checked.has_value) {
            buffer_cstr(buffer, checked.value ? "true" : "false");
        } else {
            buffer_cstr(buffer, "null");
        }
        break;
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
        markdown_core_node_code_block_properties(node, &a, &b, &c, &x, &y);
        buffer_cstr(buffer, " mode=standalone info=");
        buffer_optional_string(buffer, a);
        buffer_cstr(buffer, " language=");
        buffer_optional_string(buffer, b);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, c);
        buffer_cstr(buffer, " fenced=");
        buffer_cstr(buffer, x ? "true" : "false");
        buffer_cstr(buffer, " closed=");
        buffer_cstr(buffer, y ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
        markdown_core_node_literal(node, &a);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_CODE:
        markdown_core_node_literal(node, &a);
        buffer_cstr(buffer, " mode=embedded literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA:
        markdown_core_node_formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_TABLE:
        markdown_core_node_table_column_count(node, &count);
        buffer_cstr(buffer, " alignments=[");
        for (i = 0; i < count; i++) {
            markdown_core_table_alignment alignment;
            markdown_core_node_table_alignment_at(node, i, &alignment);
            if (i) {
                buffer_cstr(buffer, ",");
            }
            buffer_cstr(buffer, alignment_name(alignment));
        }
        buffer_cstr(buffer, "]");
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW:
        markdown_core_node_table_row_is_header(node, &x);
        buffer_cstr(buffer, " isHeader=");
        buffer_cstr(buffer, x ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE:
        markdown_core_node_directive_properties(node, &mode, &a, &b);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " name=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " attributes=");
        buffer_optional_string(buffer, b);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(node, &a);
        buffer_cstr(buffer, " id=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        markdown_core_node_cross_link_reference(node, &a);
        buffer_cstr(buffer, " reference=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_EMBED:
        markdown_core_node_embed_reference(node, &a);
        buffer_cstr(buffer, " reference=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &a, &b);
        buffer_cstr(buffer, " destination=");
        buffer_optional_string(buffer, a);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, b);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_reference_definition_properties(node, &a, &b, &c);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " destination=");
        buffer_optional_string(buffer, b);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, c);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE:
        markdown_core_node_reference_properties(node, &a, &form);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " form=");
        buffer_cstr(buffer, reference_form_name(form));
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &a, &b);
        buffer_cstr(buffer, " source=");
        buffer_optional_string(buffer, a);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, b);
        break;
    default:
        break;
    }
}

// Content equality; a NULL view and an empty view compare equal, matching
// the dump output both sides of a delta are held to.
static bool view_content_equal(markdown_core_string_view a, markdown_core_string_view b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

// Optional-string equality: the dump distinguishes an absent string (null)
// from a present empty one, so presence must match before content.
static bool view_optional_equal(markdown_core_string_view a, markdown_core_string_view b) {
    return (a.data == NULL) == (b.data == NULL) && view_content_equal(a, b);
}

bool markdown_core_ast_fields_equal(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(a);
    markdown_core_string_view a1 = {NULL, 0}, a2 = {NULL, 0}, a3 = {NULL, 0};
    markdown_core_string_view b1 = {NULL, 0}, b2 = {NULL, 0}, b3 = {NULL, 0};
    // Callers pair nodes by raw type, so the facade kinds already match.
    switch (kind) {
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level_a, level_b;
        markdown_core_node_heading_level(a, &level_a);
        markdown_core_node_heading_level(b, &level_b);
        return level_a == level_b;
    }
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor_a, flavor_b;
        markdown_core_optional_i64 start_a, start_b;
        bool tight_a, tight_b;
        markdown_core_node_list_properties(a, &flavor_a, &start_a, &tight_a);
        markdown_core_node_list_properties(b, &flavor_b, &start_b, &tight_b);
        return flavor_a == flavor_b && tight_a == tight_b && start_a.has_value == start_b.has_value &&
               (!start_a.has_value || start_a.value == start_b.value);
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked_a, checked_b;
        markdown_core_node_list_item_checked(a, &checked_a);
        markdown_core_node_list_item_checked(b, &checked_b);
        return checked_a.has_value == checked_b.has_value &&
               (!checked_a.has_value || checked_a.value == checked_b.value);
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced_a, closed_a, fenced_b, closed_b;
        markdown_core_node_code_block_properties(a, &a1, &a2, &a3, &fenced_a, &closed_a);
        markdown_core_node_code_block_properties(b, &b1, &b2, &b3, &fenced_b, &closed_b);
        return fenced_a == fenced_b && closed_a == closed_b && view_optional_equal(a1, b1) &&
               view_optional_equal(a2, b2) && view_content_equal(a3, b3);
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_CODE:
        markdown_core_node_literal(a, &a1);
        markdown_core_node_literal(b, &b1);
        return view_content_equal(a1, b1);
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: {
        markdown_core_node_reference_definition_properties(a, &a1, &a2, &a3);
        markdown_core_node_reference_definition_properties(b, &b1, &b2, &b3);
        return view_content_equal(a1, b1) && view_content_equal(a2, b2) && view_content_equal(a3, b3);
    }
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form_a, form_b;
        markdown_core_node_reference_properties(a, &a1, &form_a);
        markdown_core_node_reference_properties(b, &b1, &form_b);
        return form_a == form_b && view_content_equal(a1, b1);
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode_a, mode_b;
        markdown_core_node_formula_properties(a, &mode_a, &a1);
        markdown_core_node_formula_properties(b, &mode_b, &b1);
        return mode_a == mode_b && view_content_equal(a1, b1);
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count_a, count_b, i;
        markdown_core_node_table_column_count(a, &count_a);
        markdown_core_node_table_column_count(b, &count_b);
        if (count_a != count_b) {
            return false;
        }
        for (i = 0; i < count_a; i++) {
            markdown_core_table_alignment alignment_a, alignment_b;
            markdown_core_node_table_alignment_at(a, i, &alignment_a);
            markdown_core_node_table_alignment_at(b, i, &alignment_b);
            if (alignment_a != alignment_b) {
                return false;
            }
        }
        return true;
    }
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header_a, header_b;
        markdown_core_node_table_row_is_header(a, &header_a);
        markdown_core_node_table_row_is_header(b, &header_b);
        return header_a == header_b;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        const char *name_a = markdown_core_extensions_get_directive_name((markdown_core_node *)a);
        const char *name_b = markdown_core_extensions_get_directive_name((markdown_core_node *)b);
        const char *attributes_a = markdown_core_extensions_get_directive_attributes((markdown_core_node *)a);
        const char *attributes_b = markdown_core_extensions_get_directive_attributes((markdown_core_node *)b);
        a1.data = (const uint8_t *)name_a;
        a1.length = name_a ? strlen(name_a) : 0;
        b1.data = (const uint8_t *)name_b;
        b1.length = name_b ? strlen(name_b) : 0;
        a2.data = (const uint8_t *)attributes_a;
        a2.length = attributes_a ? strlen(attributes_a) : 0;
        b2.data = (const uint8_t *)attributes_b;
        b2.length = attributes_b ? strlen(attributes_b) : 0;
        return view_content_equal(a1, b1) && view_optional_equal(a2, b2);
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(a, &a1);
        markdown_core_node_footnote_id(b, &b1);
        return view_content_equal(a1, b1);
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        markdown_core_node_cross_link_reference(a, &a1);
        markdown_core_node_cross_link_reference(b, &b1);
        return view_content_equal(a1, b1);
    case MARKDOWN_CORE_KIND_EMBED:
        markdown_core_node_embed_reference(a, &a1);
        markdown_core_node_embed_reference(b, &b1);
        return view_content_equal(a1, b1);
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(a, &a1, &a2);
        markdown_core_node_link_properties(b, &b1, &b2);
        return view_optional_equal(a1, b1) && view_optional_equal(a2, b2);
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(a, &a1, &a2);
        markdown_core_node_image_properties(b, &b1, &b2);
        return view_optional_equal(a1, b1) && view_optional_equal(a2, b2);
    default:
        return true;
    }
}

// Depth is input-controlled (nested block quotes nest one node per two input
// bytes), so the canonical dump shares the one streaming scope walker: no
// recursion, child reversal, count pre-pass, or per-node ancestor walk.
static void dump_tree(dump_buffer *buffer, const markdown_core_node *root) {
    canonical_walk walk;
    const markdown_core_node *node;
    markdown_core_scope scope;
    size_t depth;
    bool has_next;

    canonical_walk_init(&walk, root);
    while (canonical_walk_next(&walk, &node, &scope, &depth, &has_next)) {
        markdown_core_node_kind kind = markdown_core_node_get_kind(node);
        size_t child_count = markdown_core_node_child_count(node);
        size_t i;
        if (kind == MARKDOWN_CORE_KIND_NONE) {
            buffer->failed = true;
            break;
        }
        if (depth) {
            for (i = 1; i < depth; i++) {
                buffer_cstr(buffer, canonical_walk_branch_continues(&walk, i) ? "│   " : "    ");
            }
            buffer_cstr(buffer, has_next ? "├── " : "└── ");
        }
        buffer_cstr(buffer, markdown_core_node_kind_name(kind));
        buffer_cstr(buffer, " scope=");
        buffer_i64(buffer, scope.start.line);
        buffer_cstr(buffer, ":");
        buffer_i64(buffer, scope.start.column);
        buffer_cstr(buffer, "..");
        buffer_i64(buffer, scope.end.line);
        buffer_cstr(buffer, ":");
        buffer_i64(buffer, scope.end.column);
        dump_fields(buffer, node, kind);
        buffer_cstr(buffer, " children=");
        buffer_i64(buffer, (int64_t)child_count);
        buffer_cstr(buffer, "\n");
        if (buffer->failed) {
            break;
        }
    }
    if (walk.failed) {
        buffer->failed = true;
    }
    canonical_walk_dispose(&walk);
}

bool markdown_core_document_dump(
    const markdown_core_document *document,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
) {
    dump_buffer buffer = {0};
    clear_error(error);
    if (!document || !document->root || !output || !length) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document, output, and length must not be null");
        return false;
    }
    *output = NULL;
    *length = 0;
    dump_tree(&buffer, document->root);
    if (buffer.failed) {
        free(buffer.data);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not produce canonical AST dump");
        return false;
    }
    *output = buffer.data;
    *length = buffer.size;
    return true;
}

void markdown_core_dump_free(uint8_t *output) { free(output); }
