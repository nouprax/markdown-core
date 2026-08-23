#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/markdown_core.h"

#include "ast_internal.h"
#include "directive.h"
#include "formula.h"
#include "markdown-core-extensions.h"
#include "strikethrough.h"
#include "table.h"

#include <markdown-core.h>
#include <node.h>
#include <parser.h>

/* A parse failure, and NOTHING ELSE. It carries no scope -- requirement 13's
 * converse -- and the two fields that used to offer one were never written by
 * any path in the library, so `markdown_core_error_get_scope` returned false
 * for every error it could ever be handed.
 *
 * `message` is a STRING LITERAL and is not copied. Every one of the eight
 * failures this file can report names a constant, and the copy was the second
 * allocation in a function whose whole job is to report that an allocation
 * failed: on a `malloc` that returned NULL it freed the error and returned,
 * leaving the caller with no document AND no error. One allocation, one
 * failure mode. */
struct markdown_core_error {
    markdown_core_error_code code;
    const char *message;
    /* DEAD, and requirement 13's converse says so: a parse failure carries no
     * scope, because an input the parser could not turn into a document has no
     * extent to point at. No path in the library has ever written these, so
     * `markdown_core_error_get_scope` returns false for every error it can be
     * handed. They go with the binding that mirrors them (13.2), because
     * deleting the accessor here alone would leave the Swift and Kotlin
     * bridges calling a symbol that is not there. */
    bool has_scope;
    markdown_core_scope scope;
};

typedef struct dump_buffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
    bool *more;
    size_t more_capacity;
} dump_buffer;

static void clear_error(markdown_core_error **error) {
    if (error) {
        *error = NULL;
    }
}

static void set_error(markdown_core_error **error, markdown_core_error_code code, const char *message) {
    markdown_core_error *value;
    if (!error) {
        return;
    }
    value = (markdown_core_error *)calloc(1, sizeof(*value));
    if (!value) {
        return;
    }
    value->message = message;
    value->code = code;
    *error = value;
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
}

markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                     const markdown_core_parse_options *requested_options,
                                                     markdown_core_error **error) {
    markdown_core_parse_options defaults;
    const markdown_core_parse_options *options = requested_options;
    markdown_core_document *document;
    markdown_core_parser *parser;
    markdown_core_diagnostics pending_diagnostics;
    unsigned extensions = 0;
    int native_options = MARKDOWN_CORE_OPT_VALIDATE_UTF8;

    clear_error(error);
    if (!source && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "source must not be null when length is nonzero");
        return NULL;
    }
    if (!options) {
        markdown_core_parse_options_init(&defaults);
        options = &defaults;
    }
    if (options->smart_punctuation) {
        native_options |= MARKDOWN_CORE_OPT_SMART;
    }
    if (options->footnotes) {
        native_options |= MARKDOWN_CORE_OPT_FOOTNOTES;
    }
    if (options->strip_html_comments) {
        native_options |= MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
    }

    parser = markdown_core_parser_new(native_options);
    if (!parser) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

    /* The facade says WHICH extensions, never in what order; `core-extensions.c`
     * owns the order and `core/main.c` asks the same question the same way. The
     * two used to disagree -- this side attached `directive` last and the CLI
     * attached it first -- which is D15. */
    if (options->tables) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_TABLE;
    }
    if (options->strikethrough) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_STRIKETHROUGH;
    }
    if (options->autolinks) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_AUTOLINK;
    }
    if (options->task_lists) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_TASKLIST;
    }
    if (options->formulas) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_FORMULA;
    }
    if (options->directives) {
        extensions |= MARKDOWN_CORE_CORE_EXTENSION_DIRECTIVE;
    }
    if (!markdown_core_core_extensions_attach(parser, extensions)) {
        markdown_core_parser_free(parser);
        set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "required syntax extension is unavailable");
        return NULL;
    }

    /* REQUIREMENT 13, and it is asked for BEFORE the first byte is fed because
     * recording happens as the lines are read. Diagnostics are not optional
     * here for the same reason the concrete view is not (Q24): they are part of
     * the model, and the switch exists so the LAW can be checked -- the tree
     * and the records must be byte-identical either way -- not so that a
     * consumer can choose a different engine. */
    markdown_core_parser_retain_diagnostics(parser, &pending_diagnostics);

    if (length) {
        markdown_core_parser_feed(parser, (const char *)source, length);
    }
    document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        markdown_core_parser_free(parser);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    markdown_core_parser_retain_concrete(parser, &document->concrete);
    document->root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!document->root) {
        markdown_core_concrete_dispose(&document->concrete);
        markdown_core_diagnostics_dispose(&pending_diagnostics);
        free(document);
        /* A3, carried here from 3a: A FAILURE IS A RETURNED STATUS, and this is
         * the vocabulary the surface has for it. `finish` returns NULL for
         * exactly one reason on a parser that has just been fed -- every one of
         * the sticky flag's write sites is an allocation or a size cap -- so
         * reporting INTERNAL was reporting the commonest cause as the one code
         * that means "no cause is known", and ALLOCATION_FAILED was unreachable
         * from this path. */
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "the parse lost bytes it could not allocate for");
        return NULL;
    }
    document->diagnostics = pending_diagnostics;
    return document;
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    /* The regions name nodes of this tree, so the two are freed together and
     * in this order: nothing may read a region after its owner is gone. */
    markdown_core_concrete_dispose(&document->concrete);
    markdown_core_diagnostics_dispose(&document->diagnostics);
    markdown_core_node_free(document->root);
    free(document);
}

const markdown_core_node *markdown_core_document_semantic(const markdown_core_document *document) {
    return document ? document->root : NULL;
}

markdown_core_string_view markdown_core_document_source(const markdown_core_document *document) {
    markdown_core_string_view view = {NULL, 0};
    if (document && document->concrete.source.ptr) {
        view.data = document->concrete.source.ptr;
        view.length = (size_t)document->concrete.source.size;
    }
    return view;
}

size_t markdown_core_document_line_count(const markdown_core_document *document) {
    return document ? (size_t)document->concrete.line_starts_size : 0;
}

bool markdown_core_document_line_start(const markdown_core_document *document, size_t line, size_t *offset) {
    if (!document || !offset || line < 1 || line > (size_t)document->concrete.line_starts_size) {
        return false;
    }
    *offset = (size_t)document->concrete.line_starts[line - 1];
    return true;
}

size_t markdown_core_document_region_count(const markdown_core_document *document) {
    return document ? (size_t)document->concrete.regions_size : 0;
}

bool markdown_core_document_region_at(const markdown_core_document *document, size_t index,
                                      markdown_core_region *region) {
    const markdown_core_region_record *source;
    if (!document || !region || index >= (size_t)document->concrete.regions_size) {
        return false;
    }
    source = &document->concrete.regions[index];
    region->start = (size_t)source->start;
    region->length = (size_t)source->length;
    region->role = (markdown_core_region_role)source->role;
    region->owner = source->owner;
    return true;
}

bool markdown_core_document_region_owner_path(const markdown_core_document *document, size_t index, int32_t *path,
                                              size_t capacity, size_t *length) {
    const markdown_core_node *node;
    const markdown_core_node *root;
    size_t depth = 0;
    size_t at;

    if (!document || !length || index >= (size_t)document->concrete.regions_size) {
        return false;
    }
    root = document->root;
    node = document->concrete.regions[index].owner;
    /* Counted UPWARD and then reversed: a node knows its parent and its
     * previous sibling, and neither knows its own index. */
    for (; node && node != root; node = markdown_core_node_parent((markdown_core_node *)node)) {
        depth++;
    }
    *length = depth;
    if (node != root) {
        return false;
    }
    if (depth > capacity || (depth > 0 && !path)) {
        return false;
    }
    node = document->concrete.regions[index].owner;
    for (at = depth; at > 0; at--) {
        const markdown_core_node *sibling;
        int32_t position = 0;
        for (sibling = markdown_core_node_previous((markdown_core_node *)node); sibling;
             sibling = markdown_core_node_previous((markdown_core_node *)sibling)) {
            position++;
        }
        path[at - 1] = position;
        node = markdown_core_node_parent((markdown_core_node *)node);
    }
    return true;
}

/* One remembered (parent, child, index) per bucket, so a pass in source order
 * can start where the last one stopped instead of counting from scratch. */
typedef struct {
    const markdown_core_node *parent;
    const markdown_core_node *child;
    int32_t index;
} owner_memo;

#define OWNER_MEMO_BUCKETS 64u

static int32_t S_sibling_index(owner_memo *memo, const markdown_core_node *parent, const markdown_core_node *child) {
    owner_memo *entry = &memo[((uintptr_t)parent >> 4) % OWNER_MEMO_BUCKETS];
    const markdown_core_node *at;
    int32_t index = 0;
    if (entry->parent == parent) {
        int32_t steps = 0;
        for (at = entry->child; at && at != child; at = markdown_core_node_next((markdown_core_node *)at)) {
            steps++;
        }
        /* Only forward: a region whose owner sits BEFORE the last one -- which
         * the containment ledger says happens -- falls through to the count. */
        if (at == child) {
            entry->child = child;
            entry->index += steps;
            return entry->index;
        }
    }
    for (at = markdown_core_node_previous((markdown_core_node *)child); at;
         at = markdown_core_node_previous((markdown_core_node *)at)) {
        index++;
    }
    entry->parent = parent;
    entry->child = child;
    entry->index = index;
    return index;
}

/* How deep `owner` sits below `root`, or -1 if it is not below it at all. */
static int32_t S_owner_depth(const markdown_core_node *owner, const markdown_core_node *root) {
    const markdown_core_node *node;
    int32_t depth = 0;
    for (node = owner; node && node != root; node = markdown_core_node_parent((markdown_core_node *)node)) {
        depth++;
    }
    return node == root ? depth : -1;
}

bool markdown_core_document_region_owner_paths(const markdown_core_document *document, int32_t *paths,
                                               size_t paths_capacity, uint32_t *offsets, size_t offsets_capacity) {
    owner_memo memo[OWNER_MEMO_BUCKETS];
    const markdown_core_node *root;
    size_t count;
    size_t index;
    size_t at = 0;

    if (!document || !offsets) {
        return false;
    }
    count = (size_t)document->concrete.regions_size;
    if (offsets_capacity < count + 1) {
        return false;
    }
    root = document->root;
    memset(memo, 0, sizeof(memo));

    /* The offsets are written whether or not the paths fit, so the caller
     * learns the total it needs from the same call that refused. */
    offsets[0] = 0;
    for (index = 0; index < count; index++) {
        int32_t depth = S_owner_depth(document->concrete.regions[index].owner, root);
        if (depth < 0) {
            return false;
        }
        at += (size_t)depth;
        if (at > UINT32_MAX) {
            return false;
        }
        offsets[index + 1] = (uint32_t)at;
    }
    if (at > paths_capacity || (at > 0 && !paths)) {
        return false;
    }

    for (index = 0; index < count; index++) {
        const markdown_core_node *node = document->concrete.regions[index].owner;
        size_t depth = offsets[index + 1] - offsets[index];
        for (; depth > 0; depth--) {
            const markdown_core_node *parent = markdown_core_node_parent((markdown_core_node *)node);
            paths[offsets[index] + depth - 1] = S_sibling_index(memo, parent, node);
            node = parent;
        }
    }
    return true;
}

size_t markdown_core_document_diagnostic_count(const markdown_core_document *document) {
    return document ? (size_t)document->diagnostics.entries_size : 0;
}

bool markdown_core_document_diagnostic_at(const markdown_core_document *document, size_t index,
                                          markdown_core_diagnostic *diagnostic) {
    const markdown_core_diagnostic_record *entry;
    if (!document || !diagnostic || index >= (size_t)document->diagnostics.entries_size) {
        return false;
    }
    entry = &document->diagnostics.entries[index];
    diagnostic->severity = (markdown_core_diagnostic_severity)entry->severity;
    diagnostic->code = (markdown_core_diagnostic_code)entry->code;
    diagnostic->scope.start.line = entry->start_line;
    diagnostic->scope.start.column = entry->start_column;
    diagnostic->scope.end.line = entry->end_line;
    diagnostic->scope.end.column = entry->end_column;
    diagnostic->message.data = document->diagnostics.messages.ptr + entry->message_start;
    diagnostic->message.length = (size_t)entry->message_length;
    return true;
}

const char *markdown_core_diagnostic_code_name(markdown_core_diagnostic_code code) {
    return markdown_core_diagnostic_code_string(code);
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

void markdown_core_error_free(markdown_core_error *error) { free(error); }

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) {
    if (!node) {
        return MARKDOWN_CORE_KIND_NONE;
    }
    if (node->type == MARKDOWN_CORE_NODE_DOCUMENT) {
        return MARKDOWN_CORE_KIND_DOCUMENT;
    }
    if (node->type == MARKDOWN_CORE_NODE_BLOCK_QUOTE) {
        return MARKDOWN_CORE_KIND_BLOCK_QUOTE;
    }
    if (node->type == MARKDOWN_CORE_NODE_PARAGRAPH) {
        return MARKDOWN_CORE_KIND_PARAGRAPH;
    }
    if (node->type == MARKDOWN_CORE_NODE_HEADING) {
        return MARKDOWN_CORE_KIND_HEADING;
    }
    if (node->type == MARKDOWN_CORE_NODE_THEMATIC_BREAK) {
        return MARKDOWN_CORE_KIND_THEMATIC_BREAK;
    }
    if (node->type == MARKDOWN_CORE_NODE_LIST) {
        return MARKDOWN_CORE_KIND_LIST;
    }
    if (node->type == MARKDOWN_CORE_NODE_LIST_ITEM) {
        return MARKDOWN_CORE_KIND_LIST_ITEM;
    }
    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK) {
        return MARKDOWN_CORE_KIND_CODE_BLOCK;
    }
    if (node->type == MARKDOWN_CORE_NODE_HTML_BLOCK) {
        return MARKDOWN_CORE_KIND_HTML_BLOCK;
    }
    if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION) {
        return MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION;
    }
    if (node->type == MARKDOWN_CORE_NODE_REFERENCE_DEFINITION) {
        return MARKDOWN_CORE_KIND_REFERENCE_DEFINITION;
    }
    if (node->type == MARKDOWN_CORE_NODE_LINK_REFERENCE) {
        return MARKDOWN_CORE_KIND_LINK_REFERENCE;
    }
    if (node->type == MARKDOWN_CORE_NODE_IMAGE_REFERENCE) {
        return MARKDOWN_CORE_KIND_IMAGE_REFERENCE;
    }
    if (node->type == MARKDOWN_CORE_NODE_TEXT) {
        return MARKDOWN_CORE_KIND_TEXT;
    }
    if (node->type == MARKDOWN_CORE_NODE_SOFT_BREAK) {
        return MARKDOWN_CORE_KIND_SOFT_BREAK;
    }
    if (node->type == MARKDOWN_CORE_NODE_LINE_BREAK) {
        return MARKDOWN_CORE_KIND_LINE_BREAK;
    }
    if (node->type == MARKDOWN_CORE_NODE_CODE) {
        return MARKDOWN_CORE_KIND_CODE;
    }
    if (node->type == MARKDOWN_CORE_NODE_HTML) {
        return MARKDOWN_CORE_KIND_HTML;
    }
    if (node->type == MARKDOWN_CORE_NODE_EMPHASIS) {
        return MARKDOWN_CORE_KIND_EMPHASIS;
    }
    if (node->type == MARKDOWN_CORE_NODE_STRONG) {
        return MARKDOWN_CORE_KIND_STRONG;
    }
    if (node->type == MARKDOWN_CORE_NODE_LINK) {
        return MARKDOWN_CORE_KIND_LINK;
    }
    if (node->type == MARKDOWN_CORE_NODE_IMAGE) {
        return MARKDOWN_CORE_KIND_IMAGE;
    }
    if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE) {
        return MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE) {
        return MARKDOWN_CORE_KIND_TABLE;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW) {
        return MARKDOWN_CORE_KIND_TABLE_ROW;
    }
    if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL) {
        return MARKDOWN_CORE_KIND_TABLE_CELL;
    }
    if (node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH) {
        return MARKDOWN_CORE_KIND_STRIKETHROUGH;
    }
    if (node->type == MARKDOWN_CORE_NODE_FORMULA) {
        return MARKDOWN_CORE_KIND_FORMULA;
    }
    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK) {
        return MARKDOWN_CORE_KIND_FORMULA_BLOCK;
    }
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE) {
        return MARKDOWN_CORE_KIND_DIRECTIVE;
    }
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK;
    }
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return MARKDOWN_CORE_KIND_DIRECTIVE_LABEL;
    }
    return MARKDOWN_CORE_KIND_NONE;
}

const char *markdown_core_node_kind_name(markdown_core_node_kind kind) {
    static const char *const names[] = {"None",
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
                                        "DirectiveBlock",
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
                                        "TableRow",
                                        "TableCell",
                                        "DirectiveLabel",
                                        "ReferenceDefinition",
                                        "LinkReference",
                                        "ImageReference"};
    if (kind < MARKDOWN_CORE_KIND_NONE || kind > MARKDOWN_CORE_KIND_IMAGE_REFERENCE) {
        return "None";
    }
    return names[kind];
}

markdown_core_scope markdown_core_node_scope(const markdown_core_node *node) {
    markdown_core_scope scope = {{0, 0}, {0, 0}};
    if (node) {
        scope.start.line = node->start_line;
        scope.start.column = node->start_column;
        scope.end.line = node->end_line;
        scope.end.column = node->end_column;
    }
    return scope;
}

/* THE LABEL IS A NODE. It always was one in the tree; this facade used to
 * splice it out -- first_child skipped past it into its children and
 * next_sibling climbed back out -- so a directive's label reached every
 * binding as a COUNT on the parent and a run of children with no container.
 * Step 7 stops hiding it: `DirectiveLabel` is the 29th kind, its scope spans
 * its brackets, and `label=` is gone from the dump because the node is there
 * to be seen. The two accessors that existed only to name where the label's
 * children began and ended went with it. */
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

bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level) {
    if (!node || node->type != MARKDOWN_CORE_NODE_HEADING || !level) {
        return false;
    }
    *level = node->as.heading.level;
    return true;
}

bool markdown_core_node_list_properties(const markdown_core_node *node, markdown_core_list_flavor *flavor,
                                        markdown_core_optional_i64 *start, bool *tight) {
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

bool markdown_core_node_code_block_properties(const markdown_core_node *node, markdown_core_string_view *info,
                                              markdown_core_string_view *language, markdown_core_string_view *literal,
                                              bool *fenced, bool *closed) {
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

bool markdown_core_node_formula_properties(const markdown_core_node *node, markdown_core_placement_mode *mode,
                                           markdown_core_string_view *literal) {
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

bool markdown_core_node_table_alignment_at(const markdown_core_node *node, size_t index,
                                           markdown_core_table_alignment *alignment) {
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

/* ATTRIBUTES ARE AN ORDERED SEQUENCE, sorted by name (Q19). The JSON string
 * this used to hand out was a second representation of the list the parser
 * already holds, with a parser of its own to read it back; both are gone.
 * `has_attributes` distinguishes `:n` from `:n{}` -- absent from empty -- which
 * the old `null` versus `"{}"` said and a count alone cannot. */
bool markdown_core_node_directive_properties(const markdown_core_node *node, markdown_core_string_view *name,
                                             bool *has_attributes, size_t *attribute_count) {
    const char *value;
    if (!node || !name || !has_attributes || !attribute_count ||
        (node->type != MARKDOWN_CORE_NODE_DIRECTIVE && node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)) {
        return false;
    }
    value = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
    name->data = (const uint8_t *)value;
    name->length = value ? strlen(value) : 0;
    *has_attributes = markdown_core_extensions_directive_has_attributes((markdown_core_node *)node) != 0;
    *attribute_count = markdown_core_extensions_directive_attribute_count((markdown_core_node *)node);
    return true;
}

bool markdown_core_node_directive_attribute_at(const markdown_core_node *node, size_t index,
                                               markdown_core_string_view *name, markdown_core_string_view *value) {
    const char *name_bytes;
    const char *value_bytes;
    size_t name_length;
    size_t value_length;
    if (!node || !name || !value) {
        return false;
    }
    if (!markdown_core_extensions_directive_attribute_at((markdown_core_node *)node, index, &name_bytes, &name_length,
                                                         &value_bytes, &value_length)) {
        return false;
    }
    name->data = (const uint8_t *)name_bytes;
    name->length = name_length;
    value->data = (const uint8_t *)value_bytes;
    value->length = value_length;
    return true;
}

static bool link_properties(const markdown_core_node *node, uint16_t expected, markdown_core_string_view *url,
                            markdown_core_string_view *title) {
    if (!node || node->type != expected || !url || !title) {
        return false;
    }
    view_chunk(url, &node->as.link.url);
    view_chunk(title, &node->as.link.title);
    return true;
}

bool markdown_core_node_link_properties(const markdown_core_node *node, markdown_core_string_view *destination,
                                        markdown_core_string_view *title) {
    return link_properties(node, MARKDOWN_CORE_NODE_LINK, destination, title);
}

bool markdown_core_node_image_properties(const markdown_core_node *node, markdown_core_string_view *source,
                                         markdown_core_string_view *title) {
    return link_properties(node, MARKDOWN_CORE_NODE_IMAGE, source, title);
}

/* ONE accessor for all five reference kinds, dispatched on the type and
 * relying on no layout at all.
 *
 * The union arms genuinely differ -- a definition is BOXED and the other four
 * are inline -- so the common-initial-sequence read that would have made this
 * a single load is not merely unlicensed, it is impossible: `as.association`
 * on a definition node would read a POINTER as `chunk.data`. It costs a branch
 * and buys a guarantee the union trick never had. */
bool markdown_core_node_association(const markdown_core_node *node, markdown_core_string_view *label,
                                    markdown_core_string_view *identifier) {
    const markdown_core_association *association;
    if (!node || !label || !identifier) {
        return false;
    }
    switch (node->type) {
    case MARKDOWN_CORE_NODE_REFERENCE_DEFINITION:
        if (!node->as.definition) {
            return false;
        }
        association = &node->as.definition->association;
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        association = &node->as.association;
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        association = &node->as.reference.association;
        break;
    default:
        return false;
    }
    view_chunk(label, &association->label);
    view_chunk(identifier, &association->identifier);
    return true;
}

bool markdown_core_node_definition_resource(const markdown_core_node *node, markdown_core_string_view *destination,
                                            markdown_core_string_view *title) {
    if (!node || node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION || !node->as.definition || !destination ||
        !title) {
        return false;
    }
    view_chunk(destination, &node->as.definition->url);
    view_chunk(title, &node->as.definition->title);
    return true;
}

bool markdown_core_node_reference_form(const markdown_core_node *node, markdown_core_reference_form *form) {
    if (!node || !form ||
        (node->type != MARKDOWN_CORE_NODE_LINK_REFERENCE && node->type != MARKDOWN_CORE_NODE_IMAGE_REFERENCE)) {
        return false;
    }
    *form = node->as.reference.form;
    return true;
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

static bool ensure_more(dump_buffer *buffer, size_t depth) {
    bool *more;
    size_t capacity;
    if (depth < buffer->more_capacity) {
        return true;
    }
    capacity = buffer->more_capacity ? buffer->more_capacity : 16;
    while (capacity <= depth) {
        capacity *= 2;
    }
    more = (bool *)realloc(buffer->more, capacity * sizeof(*more));
    if (!more) {
        buffer->failed = true;
        return false;
    }
    buffer->more = more;
    buffer->more_capacity = capacity;
    return true;
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

static const char *form_name(markdown_core_reference_form form) {
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

static const char *mode_name(markdown_core_placement_mode mode) {
    return mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? "embedded" : "standalone";
}

static void dump_fields(dump_buffer *buffer, const markdown_core_node *node, markdown_core_node_kind kind) {
    markdown_core_string_view a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0}, d = {NULL, 0};
    markdown_core_optional_i64 start;
    markdown_core_optional_bool checked;
    markdown_core_list_flavor flavor;
    markdown_core_placement_mode mode;
    markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
    bool x, y, has_attributes;
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
        buffer_cstr(buffer, " info=");
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
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_FORMULA:
        /* The only kind whose mode is a fact about the SOURCE: `$x$` is
         * embedded and `$$x$$` is standalone inside the same paragraph.  The
         * other five carried a mode that their kind already implied, and Q29
         * deleted all five at 15A.4. */
        markdown_core_node_formula_properties(node, &mode, &a);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " literal=");
        buffer_json_string(buffer, a);
        break;
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
        markdown_core_node_formula_properties(node, &mode, &a);
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
        markdown_core_node_directive_properties(node, &a, &has_attributes, &count);
        buffer_cstr(buffer, " name=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " attributes=");
        if (!has_attributes) {
            buffer_cstr(buffer, "null");
            break;
        }
        buffer_cstr(buffer, "[");
        for (i = 0; i < count; i++) {
            if (!markdown_core_node_directive_attribute_at(node, i, &a, &b)) {
                continue;
            }
            if (i) {
                buffer_cstr(buffer, " ");
            }
            buffer_bytes(buffer, a.data, a.length);
            buffer_cstr(buffer, "=");
            buffer_json_string(buffer, b);
        }
        buffer_cstr(buffer, "]");
        break;
    /* `label=`, not `id=` (Q5). Two names for one field after unifying the
     * field is the failure mode that produced three accessors. */
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_association(node, &a, &b);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " identifier=");
        buffer_json_string(buffer, b);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE:
        markdown_core_node_association(node, &a, &b);
        markdown_core_node_reference_form(node, &form);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " identifier=");
        buffer_json_string(buffer, b);
        buffer_cstr(buffer, " form=");
        buffer_cstr(buffer, form_name(form));
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_association(node, &a, &b);
        markdown_core_node_definition_resource(node, &c, &d);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " identifier=");
        buffer_json_string(buffer, b);
        /* `destination=` is printed as a string and never as `null`: a
         * definition that could not build one is not emitted (Q7, Q26), so an
         * empty destination here means the source wrote `<>` and meant it. */
        buffer_cstr(buffer, " destination=");
        buffer_json_string(buffer, c);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, d);
        break;
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &a, &b);
        buffer_cstr(buffer, " destination=");
        buffer_optional_string(buffer, a);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, b);
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

static void dump_node(dump_buffer *buffer, const markdown_core_node *node, size_t depth) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_scope scope = markdown_core_node_scope(node);
    const markdown_core_node *child;
    size_t count = markdown_core_node_child_count(node);
    size_t i;
    if (kind == MARKDOWN_CORE_KIND_NONE) {
        buffer->failed = true;
        return;
    }
    if (depth) {
        for (i = 0; i + 1 < depth; i++) {
            buffer_cstr(buffer, buffer->more[i] ? "│   " : "    ");
        }
        buffer_cstr(buffer, buffer->more[depth - 1] ? "├── " : "└── ");
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
    buffer_i64(buffer, (int64_t)count);
    buffer_cstr(buffer, "\n");

    child = markdown_core_node_get_first_child(node);
    while (child) {
        const markdown_core_node *next = markdown_core_node_get_next_sibling(child);
        if (!ensure_more(buffer, depth)) {
            return;
        }
        buffer->more[depth] = next != NULL;
        dump_node(buffer, child, depth + 1);
        child = next;
    }
}

bool markdown_core_document_dump(const markdown_core_document *document, uint8_t **output, size_t *length,
                                 markdown_core_error **error) {
    dump_buffer buffer = {0};
    clear_error(error);
    if (!document || !document->root || !output || !length) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "document, output, and length must not be null");
        return false;
    }
    *output = NULL;
    *length = 0;
    dump_node(&buffer, document->root, 0);
    free(buffer.more);
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
