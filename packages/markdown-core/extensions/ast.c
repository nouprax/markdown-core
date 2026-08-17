#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "buffer.h"
#include "document_internal.h"
#include "cross_reference.h"
#include "directive.h"
#include "formula.h"
#include "markdown-core-extensions.h"
#include "strikethrough.h"
#include "table.h"

#include <markdown-core.h>
#include <node.h>
#include <parser.h>

/* No scope. An error here is the parse failing to RUN — a rejected argument,
 * a failed allocation, a broken invariant — and none of those is attributable
 * to an extent of the input. What IS attributable, a directive's `{...}` that
 * did not parse, is a markdown_core_diagnostic and carries a scope that is
 * not optional. */
struct markdown_core_error {
    markdown_core_error_code code;
    char *message;
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
    options->tables = true;
    options->strikethrough = true;
    options->autolinks = true;
    options->task_lists = true;
    options->formulas = true;
    options->directives = true;
    options->cross_links = true;
    options->embeds = true;
}

/* Whether this handle is still the chain's head — the one document whose
 * tree the chain keeps — on a chain that is still whole. Reads that would
 * otherwise answer with a tree route through here, so a superseded handle
 * answers as if it had none rather than describing text that has since
 * grown past it, and a poisoned chain answers as if it had none rather than
 * showing the tree a failed append left half-grown: "only free remains" is
 * enforced, not just documented. */
static bool document_is_head(const markdown_core_document *document) {
    return document && !document->chain->poisoned && document->revision + 1 == document->chain->next_revision;
}

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) {
    return document_is_head(document) ? document_generation_root(&document->chain->head) : NULL;
}

const markdown_core_node *markdown_core_document_concrete(const markdown_core_document *document) {
    /* Internal boundary: callers hold a parsed document, so there is no NULL
     * to tolerate — the semantic root and the concrete owner are the same
     * retained tree (ast_internal.h). */
    return document_generation_root(&document->chain->head);
}

size_t markdown_core_document_diagnostics(
    const markdown_core_document *document,
    const markdown_core_diagnostic **diagnostics
) {
    if (!document_is_head(document)) {
        if (diagnostics) {
            *diagnostics = NULL;
        }
        return 0;
    }
    if (diagnostics) {
        *diagnostics = document->chain->head.diagnostics;
    }
    return document->chain->head.diagnostic_count;
}

markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error) {
    return error ? error->code : MARKDOWN_CORE_ERROR_NONE;
}

markdown_core_string markdown_core_error_get_message(const markdown_core_error *error) {
    markdown_core_string view = {NULL, 0};
    if (error && error->message) {
        view.data = (const uint8_t *)error->message;
        view.length = strlen(error->message);
    }
    return view;
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
    int start_line, end_line;
    if (!node) {
        return scope;
    }
    // Positions are absolute, as the parser wrote them. They used to be
    // stored parent-relative and resolved here by summing the parent chain —
    // an encoding whose whole purpose was that an INCREMENTAL commit could
    // line-shift an ancestor and have every descendant follow for free. There
    // is no incremental commit, nothing shifts, and a scope read no longer
    // pays an ancestor walk.
    start_line = node->start_line;
    end_line = node->end_line;
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
    (void)parent_start_line;
    *scope = markdown_core_node_scope(current);
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

static void view_chunk(markdown_core_string *view, const markdown_core_chunk *chunk) {
    view->data = chunk->data;
    view->length = chunk->len < 0 ? 0 : (size_t)chunk->len;
}

bool markdown_core_node_code_block_properties(
    const markdown_core_node *node,
    markdown_core_string *info,
    markdown_core_string *language,
    markdown_core_string *literal,
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

bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string *literal) {
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
    markdown_core_string *literal
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

/* One complete comment and nothing else: after surrounding whitespace the
 * literal opens with `<!--` and the first `-->` in it is the terminal
 * bytes. Comment-prefixed HTML with a same-line tail (`<!-- a --> tail`)
 * is not a comment — the bit never lies about trailing bytes — and a
 * literal that is several comments is html to be read, not skipped. The
 * classification is a pure function of the literal, so every binding can
 * derive the same answer from the bytes it already holds. */
bool markdown_core_node_html_comment(const markdown_core_node *node, bool *comment) {
    const unsigned char *data;
    markdown_core_bufsize length;
    markdown_core_bufsize offset = 0;
    markdown_core_bufsize close;
    if (!node || (node->type != MARKDOWN_CORE_NODE_HTML_BLOCK && node->type != MARKDOWN_CORE_NODE_HTML) || !comment) {
        return false;
    }
    data = node->as.literal.data;
    length = node->as.literal.len;
    /* Neither trim can run off the literal: an HTML literal always holds
     * the non-whitespace `<` that opened it, so both walks stop there at
     * the latest. Leading trim is spaces only — a block whose first line
     * leads with a tab sits at column four and parses as indented code,
     * never an HTML block, and an inline literal starts at its `<`. */
    while (data[length - 1] == '\n' || data[length - 1] == ' ' || data[length - 1] == '\t') {
        length--;
    }
    while (data[offset] == ' ') {
        offset++;
    }
    *comment = false;
    if (length - offset >= 4 && memcmp(data + offset, "<!--", 4) == 0) {
        for (close = offset + 1; close + 3 <= length; close++) {
            if (memcmp(data + close, "-->", 3) == 0) {
                *comment = close + 3 == length;
                break;
            }
        }
    }
    return true;
}

static bool is_directive_kind(const markdown_core_node *node) {
    return node && (node->type == MARKDOWN_CORE_NODE_DIRECTIVE || node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK);
}

bool markdown_core_node_directive_properties(
    const markdown_core_node *node,
    markdown_core_placement_mode *mode,
    markdown_core_string *name,
    bool *has_attributes
) {
    const char *value;
    if (!node || !mode || !name || !has_attributes || !is_directive_kind(node)) {
        return false;
    }
    *mode = node->type == MARKDOWN_CORE_NODE_DIRECTIVE ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                       : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    value = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
    name->data = (const uint8_t *)value;
    name->length = value ? strlen(value) : 0;
    *has_attributes = markdown_core_extensions_directive_attributes_present(node);
    return true;
}

bool markdown_core_node_directive_attribute_count(const markdown_core_node *node, size_t *count) {
    if (!node || !count || !is_directive_kind(node)) {
        return false;
    }
    *count = markdown_core_extensions_directive_attribute_count(node);
    return true;
}

bool markdown_core_node_directive_attribute_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_string *key,
    markdown_core_string *value
) {
    const uint8_t *key_data = NULL;
    const uint8_t *value_data = NULL;
    size_t key_length = 0;
    size_t value_length = 0;

    if (!node || !key || !value || !is_directive_kind(node)) {
        return false;
    }
    if (!markdown_core_extensions_directive_attribute_at(
            node,
            index,
            &key_data,
            &key_length,
            &value_data,
            &value_length
        )) {
        return false;
    }
    key->data = key_data;
    key->length = key_length;
    value->data = value_data;
    value->length = value_length;
    return true;
}

const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node) {
    return markdown_core_directive_label((markdown_core_node *)node);
}

static bool link_properties(
    const markdown_core_node *node,
    uint16_t expected,
    markdown_core_string *url,
    markdown_core_string *title
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
    markdown_core_string *label,
    markdown_core_string *destination,
    markdown_core_string *title
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
    markdown_core_string *label,
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
    markdown_core_string *destination,
    markdown_core_string *title
) {
    return link_properties(node, MARKDOWN_CORE_NODE_LINK, destination, title);
}

bool markdown_core_node_image_properties(
    const markdown_core_node *node,
    markdown_core_string *source,
    markdown_core_string *title
) {
    return link_properties(node, MARKDOWN_CORE_NODE_IMAGE, source, title);
}

bool markdown_core_node_footnote_id(const markdown_core_node *node, markdown_core_string *id) {
    if (!node || !id ||
        (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION && node->type != MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE)) {
        return false;
    }
    // Both kinds carry their own label exactly as written in the source;
    // resolution and numbering are presentation, not node content.
    view_chunk(id, &node->as.literal);
    return true;
}

static bool cross_reference(
    const markdown_core_node *node,
    markdown_core_node_type type,
    markdown_core_string *reference
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

bool markdown_core_node_cross_link_reference(const markdown_core_node *node, markdown_core_string *reference) {
    return cross_reference(node, MARKDOWN_CORE_NODE_CROSS_LINK, reference);
}

bool markdown_core_node_embed_reference(const markdown_core_node *node, markdown_core_string *reference) {
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

static void buffer_json_string(dump_buffer *buffer, markdown_core_string value) {
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

/* Byte-lexicographic, shorter first on a common prefix. Attribute names are
 * unique, so this is a total order over them. */
static bool string_before(markdown_core_string left, markdown_core_string right) {
    size_t shortest = left.length < right.length ? left.length : right.length;
    int order = shortest ? memcmp(left.data, right.data, shortest) : 0;
    return order != 0 ? order < 0 : left.length < right.length;
}

static void buffer_optional_string(dump_buffer *buffer, markdown_core_string value) {
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
    markdown_core_string a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0};
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
    case MARKDOWN_CORE_KIND_TEXT:
        markdown_core_node_literal(node, &a);
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_HTML:
        markdown_core_node_literal(node, &a);
        markdown_core_node_html_comment(node, &x);
        buffer_cstr(buffer, " comment=");
        buffer_cstr(buffer, x ? "true" : "false");
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
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        bool present = false;
        size_t attribute_count = 0;
        size_t index;
        markdown_core_string previous = {NULL, 0};
        markdown_core_node_directive_properties(node, &mode, &a, &present);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " name=");
        buffer_json_string(buffer, a);
        // Pair by pair, the way every other field prints: `null` for no
        // container at all, nothing after `attributes=` for an empty one, and
        // no serialization format inside a diagnostic dump.
        //
        // SORTED BY NAME, not in source order. Four dumpers have to agree
        // byte for byte, and Swift's map is unordered -- so the order the
        // engine holds belongs to the VALUE, and the dump takes the one order
        // every platform can reproduce.
        buffer_cstr(buffer, " attributes=");
        if (!present) {
            buffer_cstr(buffer, "null");
            break;
        }
        // Bracketed, like the table's `alignments=[...]`: a value may contain
        // spaces, so the group needs a delimiter for the field to be one
        // field -- and an attribute named `children` must not read as the
        // record's own `children=`.
        buffer_cstr(buffer, "[");
        markdown_core_node_directive_attribute_count(node, &attribute_count);
        for (index = 0; index < attribute_count; index++) {
            size_t candidate;
            size_t chosen = attribute_count;
            markdown_core_string best = {NULL, 0};
            // Selection sort over the pairs: a directive has a handful of
            // attributes, and this needs no allocation on the dump path.
            for (candidate = 0; candidate < attribute_count; candidate++) {
                if (!markdown_core_node_directive_attribute_at(node, candidate, &a, &b)) {
                    continue;
                }
                if (index > 0 && !(string_before(previous, a))) {
                    continue;
                }
                if (chosen != attribute_count && !string_before(a, best)) {
                    continue;
                }
                chosen = candidate;
                best = a;
            }
            if (chosen == attribute_count) {
                break;
            }
            markdown_core_node_directive_attribute_at(node, chosen, &a, &b);
            if (index > 0) {
                buffer_cstr(buffer, " ");
            }
            buffer_bytes(buffer, a.data, a.length);
            buffer_cstr(buffer, "=");
            buffer_json_string(buffer, b);
            previous = a;
        }
        buffer_cstr(buffer, "]");
        break;
    }
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
// the dump output the dump-equivalence oracle holds every revision to.
static bool view_content_equal(markdown_core_string a, markdown_core_string b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

// Optional-string equality: the dump distinguishes an absent string (null)
// from a present empty one, so presence must match before content.
static bool view_optional_equal(markdown_core_string a, markdown_core_string b) {
    return (a.data == NULL) == (b.data == NULL) && view_content_equal(a, b);
}

bool markdown_core_ast_projection_changed(const markdown_core_node *a, const markdown_core_node *b) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(a);
    markdown_core_string a1 = {NULL, 0}, a2 = {NULL, 0}, a3 = {NULL, 0};
    markdown_core_string b1 = {NULL, 0}, b2 = {NULL, 0}, b3 = {NULL, 0};
    bool value = false;
    bool text = false;
    // Callers pair nodes by raw type, so the facade kinds already match.
    switch (kind) {
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level_a, level_b;
        markdown_core_node_heading_level(a, &level_a);
        markdown_core_node_heading_level(b, &level_b);
        value = level_a != level_b;
        break;
    }
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor_a, flavor_b;
        markdown_core_optional_i64 start_a, start_b;
        bool tight_a, tight_b;
        markdown_core_node_list_properties(a, &flavor_a, &start_a, &tight_a);
        markdown_core_node_list_properties(b, &flavor_b, &start_b, &tight_b);
        value =
            !(flavor_a == flavor_b && tight_a == tight_b && start_a.has_value == start_b.has_value &&
              (!start_a.has_value || start_a.value == start_b.value));
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked_a, checked_b;
        markdown_core_node_list_item_checked(a, &checked_a);
        markdown_core_node_list_item_checked(b, &checked_b);
        value =
            !(checked_a.has_value == checked_b.has_value &&
              (!checked_a.has_value || checked_a.value == checked_b.value));
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced_a, closed_a, fenced_b, closed_b;
        markdown_core_node_code_block_properties(a, &a1, &a2, &a3, &fenced_a, &closed_a);
        markdown_core_node_code_block_properties(b, &b1, &b2, &b3, &fenced_b, &closed_b);
        value =
            !(fenced_a == fenced_b && closed_a == closed_b && view_optional_equal(a1, b1) &&
              view_optional_equal(a2, b2));
        text = !view_content_equal(a3, b3);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_CODE:
        markdown_core_node_literal(a, &a1);
        markdown_core_node_literal(b, &b1);
        text = !view_content_equal(a1, b1);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: {
        markdown_core_node_reference_definition_properties(a, &a1, &a2, &a3);
        markdown_core_node_reference_definition_properties(b, &b1, &b2, &b3);
        value = !(view_content_equal(a1, b1) && view_content_equal(a2, b2) && view_content_equal(a3, b3));
        break;
    }
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form_a, form_b;
        markdown_core_node_reference_properties(a, &a1, &form_a);
        markdown_core_node_reference_properties(b, &b1, &form_b);
        value = !(form_a == form_b && view_content_equal(a1, b1));
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode_a, mode_b;
        markdown_core_node_formula_properties(a, &mode_a, &a1);
        markdown_core_node_formula_properties(b, &mode_b, &b1);
        value = mode_a != mode_b;
        text = !view_content_equal(a1, b1);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count_a, count_b, i;
        markdown_core_node_table_column_count(a, &count_a);
        markdown_core_node_table_column_count(b, &count_b);
        if (count_a != count_b) {
            value = true;
            break;
        }
        for (i = 0; i < count_a && !value; i++) {
            markdown_core_table_alignment alignment_a, alignment_b;
            markdown_core_node_table_alignment_at(a, i, &alignment_a);
            markdown_core_node_table_alignment_at(b, i, &alignment_b);
            value = alignment_a != alignment_b;
        }
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header_a, header_b;
        markdown_core_node_table_row_is_header(a, &header_a);
        markdown_core_node_table_row_is_header(b, &header_b);
        value = header_a != header_b;
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* The name, and the attribute LIST as each node holds it — presence,
         * count, each pair in source order — which is what the rendered
         * JSON is a rendering of, read without the rendering: the render
         * allocates and answers NULL when it cannot, and two lost renders
         * would compare equal. A name the accessor could not produce (it
         * allocates only for a node no parser made) reports "differs", so a
         * revision bump can never be missed. */
        const char *name_a = markdown_core_extensions_get_directive_name((markdown_core_node *)a);
        const char *name_b = markdown_core_extensions_get_directive_name((markdown_core_node *)b);
        bool present_a = markdown_core_extensions_directive_attributes_present(a);
        bool present_b = markdown_core_extensions_directive_attributes_present(b);
        size_t count_a = present_a ? markdown_core_extensions_directive_attribute_count(a) : 0;
        size_t count_b = present_b ? markdown_core_extensions_directive_attribute_count(b) : 0;
        size_t i;
        if (!name_a || !name_b) {
            value = true;
            break;
        }
        a1.data = (const uint8_t *)name_a;
        a1.length = strlen(name_a);
        b1.data = (const uint8_t *)name_b;
        b1.length = strlen(name_b);
        value = !view_content_equal(a1, b1) || present_a != present_b || count_a != count_b;
        for (i = 0; i < count_a && !value; i++) {
            const uint8_t *key_a = NULL, *value_a = NULL, *key_b = NULL, *value_b = NULL;
            size_t key_a_length = 0, value_a_length = 0, key_b_length = 0, value_b_length = 0;
            markdown_core_extensions_directive_attribute_at(a, i, &key_a, &key_a_length, &value_a, &value_a_length);
            markdown_core_extensions_directive_attribute_at(b, i, &key_b, &key_b_length, &value_b, &value_b_length);
            a2.data = key_a;
            a2.length = key_a_length;
            b2.data = key_b;
            b2.length = key_b_length;
            a3.data = value_a;
            a3.length = value_a_length;
            b3.data = value_b;
            b3.length = value_b_length;
            value = !(view_content_equal(a2, b2) && view_content_equal(a3, b3));
        }
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(a, &a1);
        markdown_core_node_footnote_id(b, &b1);
        value = !view_content_equal(a1, b1);
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        markdown_core_node_cross_link_reference(a, &a1);
        markdown_core_node_cross_link_reference(b, &b1);
        value = !view_content_equal(a1, b1);
        break;
    case MARKDOWN_CORE_KIND_EMBED:
        markdown_core_node_embed_reference(a, &a1);
        markdown_core_node_embed_reference(b, &b1);
        value = !view_content_equal(a1, b1);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(a, &a1, &a2);
        markdown_core_node_link_properties(b, &b1, &b2);
        value = !(view_optional_equal(a1, b1) && view_optional_equal(a2, b2));
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(a, &a1, &a2);
        markdown_core_node_image_properties(b, &b1, &b2);
        value = !(view_optional_equal(a1, b1) && view_optional_equal(a2, b2));
        break;
    default:
        break;
    }
    return value || text;
}

/* The same fields, written. One byte says whether a string is present
 * (the dump distinguishes null from empty), then its length and bytes;
 * scalars are written as bytes and 64-bit words. Two nodes whose blobs are
 * equal are nodes markdown_core_ast_projection_changed calls unchanged, and
 * the concrete runner's projection_write_agrees case holds the two to that
 * over every node of every fixture. */
static void projection_view(markdown_core_strbuf *out, markdown_core_string view) {
    uint64_t length = (uint64_t)view.length;
    markdown_core_strbuf_putc(out, view.data ? 1 : 0);
    markdown_core_strbuf_put(out, (const unsigned char *)&length, sizeof(length));
    if (view.length) {
        markdown_core_strbuf_put(out, view.data, (markdown_core_bufsize)view.length);
    }
}

/* A GROWING text, witnessed by its length alone: presence and length, no
 * bytes (markdown_core_ast_projection_witness). */
static void projection_view_length(markdown_core_strbuf *out, markdown_core_string view) {
    uint64_t length = (uint64_t)view.length;
    markdown_core_strbuf_putc(out, view.data ? 1 : 0);
    markdown_core_strbuf_put(out, (const unsigned char *)&length, sizeof(length));
}

static void projection_u64(markdown_core_strbuf *out, uint64_t value) {
    markdown_core_strbuf_put(out, (const unsigned char *)&value, sizeof(value));
}

/* The one field list behind both writers: exact, or with a block's own
 * content buffer witnessed by its length. */
static bool projection_write_fields(const markdown_core_node *node, markdown_core_strbuf *out, bool growing_by_length) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string v1 = {NULL, 0}, v2 = {NULL, 0}, v3 = {NULL, 0};

    projection_u64(out, (uint64_t)kind);
    switch (kind) {
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        markdown_core_node_heading_level(node, &level);
        projection_u64(out, (uint64_t)level);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor;
        markdown_core_optional_i64 start;
        bool tight = false;
        markdown_core_node_list_properties(node, &flavor, &start, &tight);
        projection_u64(out, (uint64_t)flavor);
        markdown_core_strbuf_putc(out, tight ? 1 : 0);
        markdown_core_strbuf_putc(out, start.has_value ? 1 : 0);
        projection_u64(out, start.has_value ? (uint64_t)start.value : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked;
        markdown_core_node_list_item_checked(node, &checked);
        markdown_core_strbuf_putc(out, checked.has_value ? 1 : 0);
        markdown_core_strbuf_putc(out, checked.has_value && checked.value ? 1 : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced = false, closed = false;
        markdown_core_node_code_block_properties(node, &v1, &v2, &v3, &fenced, &closed);
        markdown_core_strbuf_putc(out, fenced ? 1 : 0);
        markdown_core_strbuf_putc(out, closed ? 1 : 0);
        projection_view(out, v1);
        projection_view(out, v2);
        /* The literal is the block's content buffer, moved whole. */
        if (growing_by_length) {
            projection_view_length(out, v3);
        } else {
            projection_view(out, v3);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
        /* Likewise: the literal is the buffer. */
        markdown_core_node_literal(node, &v1);
        if (growing_by_length) {
            projection_view_length(out, v1);
        } else {
            projection_view(out, v1);
        }
        break;
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_CODE:
        markdown_core_node_literal(node, &v1);
        projection_view(out, v1);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_reference_definition_properties(node, &v1, &v2, &v3);
        projection_view(out, v1);
        projection_view(out, v2);
        projection_view(out, v3);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form;
        markdown_core_node_reference_properties(node, &v1, &form);
        projection_u64(out, (uint64_t)form);
        projection_view(out, v1);
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA: {
        markdown_core_placement_mode mode;
        markdown_core_node_formula_properties(node, &mode, &v1);
        /* A formula always has a literal (an empty one is written and
         * empty); the accessor materializes it and answers NULL only for an
         * allocation it lost — a loss this write reports, not records. */
        if (!v1.data) {
            out->oom = 1;
        }
        projection_u64(out, (uint64_t)mode);
        projection_view(out, v1);
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE: {
        size_t count = 0, i;
        markdown_core_node_table_column_count(node, &count);
        projection_u64(out, (uint64_t)count);
        for (i = 0; i < count; i++) {
            markdown_core_table_alignment alignment;
            markdown_core_node_table_alignment_at(node, i, &alignment);
            projection_u64(out, (uint64_t)alignment);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        markdown_core_node_table_row_is_header(node, &header);
        markdown_core_strbuf_putc(out, header ? 1 : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* The name (every directive has one; NULL is an allocation the
         * accessor lost, reported here) and the attribute LIST as the node
         * holds it — presence, count, and each pair in source order — which
         * is what the JSON the comparison reads is a rendering of, read
         * without the rendering, so a lost render cannot be written down as
         * "no attributes". */
        const char *name = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
        bool present = markdown_core_extensions_directive_attributes_present(node);
        size_t count = present ? markdown_core_extensions_directive_attribute_count(node) : 0;
        size_t i;
        if (!name) {
            out->oom = 1;
        }
        v1.data = (const uint8_t *)name;
        v1.length = name ? strlen(name) : 0;
        projection_view(out, v1);
        markdown_core_strbuf_putc(out, present ? 1 : 0);
        projection_u64(out, (uint64_t)count);
        for (i = 0; i < count; i++) {
            const uint8_t *key = NULL;
            const uint8_t *value = NULL;
            size_t key_length = 0;
            size_t value_length = 0;
            markdown_core_string k;
            markdown_core_string v;
            markdown_core_extensions_directive_attribute_at(node, i, &key, &key_length, &value, &value_length);
            k.data = key;
            k.length = key_length;
            v.data = value;
            v.length = value_length;
            projection_view(out, k);
            projection_view(out, v);
        }
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(node, &v1);
        projection_view(out, v1);
        break;
    case MARKDOWN_CORE_KIND_CROSS_LINK:
        markdown_core_node_cross_link_reference(node, &v1);
        projection_view(out, v1);
        break;
    case MARKDOWN_CORE_KIND_EMBED:
        markdown_core_node_embed_reference(node, &v1);
        projection_view(out, v1);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &v1, &v2);
        projection_view(out, v1);
        projection_view(out, v2);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &v1, &v2);
        projection_view(out, v1);
        projection_view(out, v2);
        break;
    default:
        break;
    }
    return !out->oom;
}

bool markdown_core_ast_projection_write(const markdown_core_node *node, markdown_core_strbuf *out) {
    return projection_write_fields(node, out, false);
}

bool markdown_core_ast_projection_witness(const markdown_core_node *node, markdown_core_strbuf *out) {
    return projection_write_fields(node, out, true);
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
    return markdown_core_ast_dump_root(
        document_is_head(document) ? document_generation_root(&document->chain->head) : NULL,
        output,
        length,
        error
    );
}

bool markdown_core_ast_dump_root(
    const markdown_core_node *document_root,
    uint8_t **output,
    size_t *length,
    markdown_core_error **error
) {
    dump_buffer buffer = {0};
    clear_error(error);
    if (!document_root || !output || !length) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document, output, and length must not be null");
        return false;
    }
    *output = NULL;
    *length = 0;
    dump_tree(&buffer, document_root);
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
