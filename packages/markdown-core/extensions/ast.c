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

/* A parse failure, and NOTHING ELSE. `markdown_core_error` means there is no
 * document, and an input the parser could not turn into a document has no
 * extent to point at. There is therefore no scope here to offer, and no
 * accessor to offer one -- the two fields that used to be here were never
 * written by any path in the library, and they went with
 * `markdown_core_error_get_scope` at Step 13.
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

/* The one parser both public entries build -- `markdown_core_document_parse`
 * and the session (T12) -- so the option mapping and the extension mask have
 * one spelling. */
static markdown_core_parser *S_parser_for_options(
    const markdown_core_parse_options *requested_options,
    markdown_core_error **error
) {
    markdown_core_parse_options defaults;
    const markdown_core_parse_options *options = requested_options;
    markdown_core_parser *parser;
    unsigned extensions = 0;
    int native_options = MARKDOWN_CORE_OPT_VALIDATE_UTF8;

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
    return parser;
}

markdown_core_document *markdown_core_document_parse(
    const uint8_t *source,
    size_t length,
    const markdown_core_parse_options *requested_options,
    markdown_core_error **error
) {
    markdown_core_document *document;
    markdown_core_parser *parser;

    clear_error(error);
    if (!source && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "source must not be null when length is nonzero");
        return NULL;
    }
    parser = S_parser_for_options(requested_options, error);
    if (!parser) {
        return NULL;
    }

    if (length) {
        markdown_core_parser_feed(parser, (const char *)source, length);
    }
    document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        markdown_core_parser_free(parser);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    document->root = markdown_core_parser_finish(parser);
    markdown_core_parser_free(parser);
    if (!document->root) {
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
    return document;
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    markdown_core_node_free(document->root);
    free(document);
}

/* THE STREAM'S ENGINE SIDE (docs/STREAMING.md T12). One parser lives for the
 * session's whole life; every `feed` hands its bytes over and returns a
 * DERIVED document -- `markdown_core_parser_derive_tree`, the clone-and-
 * project re-projection, whose closed blocks borrow the projection cache's
 * lists (T9) under the holder count that lets a borrow outlive the parser
 * itself (T19). Nothing is copied out: the tree shares. */
struct markdown_core_session {
    markdown_core_parser *parser;
};

markdown_core_session *markdown_core_session_new(
    const markdown_core_parse_options *options,
    markdown_core_error **error
) {
    markdown_core_session *session;

    clear_error(error);
    session = (markdown_core_session *)calloc(1, sizeof(*session));
    if (!session) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate session");
        return NULL;
    }
    session->parser = S_parser_for_options(options, error);
    if (!session->parser) {
        free(session);
        return NULL;
    }
    return session;
}

markdown_core_document *markdown_core_session_feed(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    markdown_core_error **error
) {
    markdown_core_document *document;

    clear_error(error);
    if (!session || !session->parser) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "the session is finished or null");
        return NULL;
    }
    if (!chunk && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "chunk must not be null when length is nonzero");
        return NULL;
    }
    if (length) {
        markdown_core_parser_feed(session->parser, (const char *)chunk, length);
    }
    document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    document->root = markdown_core_parser_derive_tree(session->parser, session->parser->refmap);
    if (!document->root) {
        free(document);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "the parse lost bytes it could not allocate for");
        return NULL;
    }
    return document;
}

markdown_core_document *markdown_core_session_finish(markdown_core_session *session, markdown_core_error **error) {
    markdown_core_document *document;

    clear_error(error);
    if (!session || !session->parser) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "the session is finished or null");
        return NULL;
    }
    document = (markdown_core_document *)calloc(1, sizeof(*document));
    if (!document) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate document");
        return NULL;
    }
    document->root = markdown_core_parser_finish(session->parser);
    markdown_core_parser_free(session->parser);
    session->parser = NULL;
    if (!document->root) {
        free(document);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "the parse lost bytes it could not allocate for");
        return NULL;
    }
    return document;
}

bool markdown_core_session_advance(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    markdown_core_error **error
) {
    clear_error(error);
    if (!session || !session->parser) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "the session is finished or null");
        return false;
    }
    if (!chunk && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "chunk must not be null when length is nonzero");
        return false;
    }
    if (length) {
        markdown_core_parser_feed(session->parser, (const char *)chunk, length);
    }
    /* The sticky flag is the one failure a feed can bank without a projection
     * to surface it; answering it here keeps "advance then feed" and "feed
     * twice" indistinguishable. */
    if (session->parser->oom) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "the parse lost bytes it could not allocate for");
        return false;
    }
    return true;
}

void markdown_core_session_free(markdown_core_session *session) {
    if (!session) {
        return;
    }
    if (session->parser) {
        markdown_core_parser_free(session->parser);
    }
    free(session);
}

const markdown_core_node *markdown_core_document_semantic(const markdown_core_document *document) {
    return document ? document->root : NULL;
}

markdown_core_error_code markdown_core_error_get_code(const markdown_core_error *error) {
    return error ? error->code : MARKDOWN_CORE_ERROR_NONE;
}

markdown_core_string markdown_core_error_get_message(const markdown_core_error *error) {
    markdown_core_string value = {NULL, 0};
    if (error && error->message) {
        value.data = (const uint8_t *)error->message;
        value.length = strlen(error->message);
    }
    return value;
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
        "ImageReference"
    };
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
markdown_core_children markdown_core_node_children(const markdown_core_node *node) {
    markdown_core_children cursor = {node, NULL, 0};
    markdown_core_child_cursor inner;
    if (node) {
        cursor.child = markdown_core_child_first(node, &inner);
        cursor.index = inner.index;
    }
    return cursor;
}

markdown_core_children markdown_core_children_next(markdown_core_children cursor) {
    markdown_core_child_cursor inner;
    if (!cursor.parent || !cursor.child) {
        cursor.child = NULL;
        return cursor;
    }
    inner.index = cursor.index;
    cursor.child = markdown_core_child_after(cursor.parent, cursor.child, &inner);
    cursor.index = inner.index;
    return cursor;
}

size_t markdown_core_node_child_count(const markdown_core_node *node) {
    if (!node) {
        return 0;
    }
    if (MARKDOWN_CORE_NODE_ARRAY_P(node)) {
        return node->children.count;
    }
    {
        size_t count = 0;
        const markdown_core_node *child = node->first_child;
        while (child) {
            count++;
            child = child->next;
        }
        return count;
    }
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

/* The chunk's bytes are LENT, not copied: `out` points into the document and
 * dies with it, which is what `markdown_core_string` documents. */
static void string_from_chunk(markdown_core_string *out, const markdown_core_chunk *chunk) {
    out->data = chunk->data;
    out->length = chunk->len < 0 ? 0 : (size_t)chunk->len;
}

/* THE FACADE FOLDS NOTHING (requirement 14). It carries the presence the
 * engine recorded and does not re-derive it from a length or a pointer. */
static void optional_string_from_chunk(markdown_core_optional_string *out, const markdown_core_optional_chunk *chunk) {
    out->has_value = chunk->has_value;
    string_from_chunk(&out->value, &chunk->value);
}

bool markdown_core_node_code_block_properties(
    const markdown_core_node *node,
    markdown_core_optional_string *info,
    markdown_core_optional_string *language,
    markdown_core_string *literal,
    bool *fenced,
    bool *closed
) {
    size_t start = 0;
    size_t end;
    if (!node || node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !info || !language || !literal || !fenced || !closed) {
        return false;
    }
    optional_string_from_chunk(info, &node->as.code.info);
    string_from_chunk(literal, &node->as.code.literal);
    /* `if (info->length == 0) info->data = NULL;` STOOD HERE, and it is the
     * fold requirement 14 names: the parse had already decided whether a fence
     * wrote an info string, and this line decided it again from a length. */
    language->has_value = false;
    language->value.data = NULL;
    language->value.length = 0;
    while (start < info->value.length && (info->value.data[start] == ' ' || info->value.data[start] == '\t' ||
                                             info->value.data[start] == '\n' || info->value.data[start] == '\r')) {
        start++;
    }
    end = start;
    while (end < info->value.length && info->value.data[end] != ' ' && info->value.data[end] != '\t' &&
           info->value.data[end] != '\n' && info->value.data[end] != '\r') {
        end++;
    }
    if (info->has_value && end > start) {
        language->has_value = true;
        language->value.data = info->value.data + start;
        language->value.length = end - start;
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
        string_from_chunk(literal, &node->as.literal);
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
    const char *value = NULL;
    size_t value_length = 0;
    markdown_core_formula_mode native_mode;
    if (!node || !mode || !literal ||
        (node->type != MARKDOWN_CORE_NODE_FORMULA && node->type != MARKDOWN_CORE_NODE_FORMULA_BLOCK)) {
        return false;
    }
    native_mode = markdown_core_extensions_get_formula_mode((markdown_core_node *)node);
    *mode = native_mode == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                               : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    /* The VIEW, not the cstr materialization (#153): the cstr getter writes
     * the chunk it reads, and this node may sit in an inline list shared by
     * several derived documents read concurrently. The facade mutates
     * nothing it reads. */
    markdown_core_extensions_formula_literal_view(node, &value, &value_length);
    literal->data = (const uint8_t *)value;
    literal->length = value_length;
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

/* ATTRIBUTES ARE AN ORDERED SEQUENCE, sorted by name (Q19). The JSON string
 * this used to hand out was a second representation of the list the parser
 * already holds, with a parser of its own to read it back; both are gone.
 * `has_attributes` distinguishes `:n` from `:n{}` -- absent from empty -- which
 * the old `null` versus `"{}"` said and a count alone cannot. */
bool markdown_core_node_directive_properties(
    const markdown_core_node *node,
    markdown_core_string *name,
    bool *has_attributes,
    size_t *attribute_count
) {
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

bool markdown_core_node_directive_attribute_at(
    const markdown_core_node *node,
    size_t index,
    markdown_core_string *name,
    markdown_core_string *value
) {
    const char *name_bytes;
    const char *value_bytes;
    size_t name_length;
    size_t value_length;
    if (!node || !name || !value) {
        return false;
    }
    if (!markdown_core_extensions_directive_attribute_at(
            (markdown_core_node *)node,
            index,
            &name_bytes,
            &name_length,
            &value_bytes,
            &value_length
        )) {
        return false;
    }
    name->data = (const uint8_t *)name_bytes;
    name->length = name_length;
    value->data = (const uint8_t *)value_bytes;
    value->length = value_length;
    return true;
}

static bool link_properties(
    const markdown_core_node *node,
    uint16_t expected,
    markdown_core_string *url,
    markdown_core_optional_string *title
) {
    if (!node || node->type != expected || !url || !title) {
        return false;
    }
    string_from_chunk(url, &node->as.link.url);
    optional_string_from_chunk(title, &node->as.link.title);
    return true;
}

bool markdown_core_node_link_properties(
    const markdown_core_node *node,
    markdown_core_string *destination,
    markdown_core_optional_string *title
) {
    return link_properties(node, MARKDOWN_CORE_NODE_LINK, destination, title);
}

bool markdown_core_node_image_properties(
    const markdown_core_node *node,
    markdown_core_string *source,
    markdown_core_optional_string *title
) {
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
bool markdown_core_node_association(
    const markdown_core_node *node,
    markdown_core_string *label,
    markdown_core_string *identifier
) {
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
        association = &node->as.association;
        break;
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        association = &node->as.footnote_reference.association;
        break;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        association = &node->as.reference.association;
        break;
    default:
        return false;
    }
    string_from_chunk(label, &association->label);
    string_from_chunk(identifier, &association->identifier);
    return true;
}

bool markdown_core_node_definition_resource(
    const markdown_core_node *node,
    markdown_core_string *destination,
    markdown_core_optional_string *title
) {
    if (!node || node->type != MARKDOWN_CORE_NODE_REFERENCE_DEFINITION || !node->as.definition || !destination ||
        !title) {
        return false;
    }
    string_from_chunk(destination, &node->as.definition->url);
    optional_string_from_chunk(title, &node->as.definition->title);
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

/* The whole pair from the node alone (D4): a block is its own owner, and an
 * inline learned its owner from the numbering pass -- the one moment anything
 * stood inside the block and beside the inline at once, which matters because
 * a shared child list carries no parent link to climb (T19). */
markdown_core_identity markdown_core_node_identifier(const markdown_core_node *node) {
    markdown_core_identity identity = {0, 0};
    if (!node) {
        return identity;
    }
    if (MARKDOWN_CORE_NODE_TYPE_BLOCK_P((markdown_core_node_type)node->type)) {
        identity.block = node->identifier;
    } else {
        identity.block = node->owner;
        identity.ordinal = node->identifier;
    }
    return identity;
}

bool markdown_core_node_reference_definition(const markdown_core_node *node, markdown_core_identity *definition) {
    if (!node || !definition) {
        return false;
    }
    definition->ordinal = 0;
    switch (node->type) {
    case MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE:
        definition->block = node->as.footnote_reference.definition;
        return true;
    case MARKDOWN_CORE_NODE_LINK_REFERENCE:
    case MARKDOWN_CORE_NODE_IMAGE_REFERENCE:
        definition->block = node->as.reference.definition;
        return true;
    default:
        return false;
    }
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

/* `null` and `""` are two answers, not one, and this reads the presence flag
 * rather than the pointer -- which is the same rule the dump already applied
 * to an optional Int and an optional Bool (requirement 14). */
static void buffer_optional_string(dump_buffer *buffer, markdown_core_optional_string value) {
    if (!value.has_value) {
        buffer_cstr(buffer, "null");
    } else {
        buffer_json_string(buffer, value.value);
    }
}

/* An identity prints as `block:ordinal` -- the same pair everywhere it
 * appears, whether as a node's own `id=` or as the `definition=` a reference
 * names. */
static void buffer_identity(dump_buffer *buffer, uint32_t block, uint32_t ordinal) {
    buffer_i64(buffer, (int64_t)block);
    buffer_cstr(buffer, ":");
    buffer_i64(buffer, (int64_t)ordinal);
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
    markdown_core_string a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0};
    markdown_core_optional_string oa = {false, {NULL, 0}}, ob = {false, {NULL, 0}};
    /* Every accessor below fills its out-parameters before this reads them
     * -- the kinds match by construction -- but the initializers keep that
     * fact out of the compiler's hands: -Werror builds cannot see through
     * the switch, and dump output must never depend on what they guess. */
    markdown_core_optional_i64 start = {false, 0};
    markdown_core_optional_bool checked = {false, false};
    markdown_core_list_flavor flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
    markdown_core_placement_mode mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED;
    markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
    markdown_core_identity definition = {0, 0};
    bool x = false, y = false, has_attributes = false;
    size_t count = 0, i;
    int32_t level = 0;
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
        markdown_core_node_code_block_properties(node, &oa, &ob, &c, &x, &y);
        buffer_cstr(buffer, " info=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " language=");
        buffer_optional_string(buffer, ob);
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
     * field is the failure mode that produced three accessors.
     *
     * A DEFINITION prints `norm=` -- the match key its label folds to -- and a
     * REFERENCE prints `definition=`, the identity of the definition it
     * resolved to. The reference's own match key is not printed: it equals the
     * winning definition's `norm` by construction, and printing it twice under
     * two names is the failure mode this comment already records. */
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
        markdown_core_node_association(node, &a, &b);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " norm=");
        buffer_json_string(buffer, b);
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_association(node, &a, &b);
        markdown_core_node_reference_definition(node, &definition);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " definition=");
        buffer_identity(buffer, definition.block, definition.ordinal);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE:
        markdown_core_node_association(node, &a, &b);
        markdown_core_node_reference_form(node, &form);
        markdown_core_node_reference_definition(node, &definition);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " form=");
        buffer_cstr(buffer, form_name(form));
        buffer_cstr(buffer, " definition=");
        buffer_identity(buffer, definition.block, definition.ordinal);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_association(node, &a, &b);
        markdown_core_node_definition_resource(node, &c, &oa);
        buffer_cstr(buffer, " label=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " norm=");
        buffer_json_string(buffer, b);
        /* `destination=` is printed as a string and never as `null`: a
         * definition that could not build one is not emitted (Q7, Q26), so an
         * empty destination here means the source wrote `<>` and meant it. */
        buffer_cstr(buffer, " destination=");
        buffer_json_string(buffer, c);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, oa);
        break;
    /* A DESTINATION IS REQUIRED (Q26) and prints as a string. `[a]()` used to
     * print `destination=null`, which said the author wrote no destination
     * when the empty parentheses are the destination they wrote. */
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &a, &oa);
        buffer_cstr(buffer, " destination=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, oa);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &a, &oa);
        buffer_cstr(buffer, " source=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, oa);
        break;
    default:
        break;
    }
}

static void dump_node(dump_buffer *buffer, const markdown_core_node *node, size_t depth) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_scope scope = markdown_core_node_scope(node);
    size_t count = markdown_core_node_child_count(node);
    size_t i;
    markdown_core_identity identity = markdown_core_node_identifier(node);
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
    buffer_cstr(buffer, " id=");
    buffer_identity(buffer, identity.block, identity.ordinal);
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

    {
        markdown_core_children cursor = markdown_core_node_children(node);
        while (cursor.child) {
            markdown_core_children next = markdown_core_children_next(cursor);
            if (!ensure_more(buffer, depth)) {
                return;
            }
            buffer->more[depth] = next.child != NULL;
            dump_node(buffer, cursor.child, depth + 1);
            cursor = next;
        }
    }
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

/* THE WIRE. The dump above is the canonical TEXT of a document; this is its
 * canonical BYTES -- one buffer per read for a binding whose boundary is
 * expensive to cross -- and the two are kept side by side so a field cannot
 * change in one and not the other. The layout is stated once, on
 * `markdown_core_document_wire` in the public header. */

/* The fixed-size writes store directly into the reserved tail rather than
 * routing a 1- or 4-byte memcpy through `buffer_bytes`: every node leads
 * with an identity and a scope, so these are the wire's hottest stores and
 * must not depend on the compiler electing to specialize the general path. */
static void wire_u8(dump_buffer *buffer, uint8_t value) {
    buffer_reserve(buffer, 1);
    if (buffer->failed) {
        return;
    }
    buffer->data[buffer->size++] = value;
    buffer->data[buffer->size] = 0;
}

static void wire_u32(dump_buffer *buffer, uint32_t value) {
    uint8_t *at;
    buffer_reserve(buffer, 4);
    if (buffer->failed) {
        return;
    }
    at = buffer->data + buffer->size;
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
    buffer->size += 4;
    buffer->data[buffer->size] = 0;
}

static void wire_i32(dump_buffer *buffer, int32_t value) { wire_u32(buffer, (uint32_t)value); }

static void wire_i64(dump_buffer *buffer, int64_t value) {
    uint64_t bits = (uint64_t)value;
    uint8_t bytes[8];
    size_t index;
    for (index = 0; index < 8; index++) {
        bytes[index] = (uint8_t)(bits >> (index * 8));
    }
    buffer_bytes(buffer, bytes, sizeof(bytes));
}

static void wire_string(dump_buffer *buffer, markdown_core_string value) {
    if (value.length > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    wire_i32(buffer, (int32_t)value.length);
    buffer_bytes(buffer, value.data, value.length);
}

/* PRESENCE COMES FROM THE VALUE (requirement 14): length -1 carries absence,
 * so `null` and `""` stay two answers on the wire. */
static void wire_optional_string(dump_buffer *buffer, markdown_core_optional_string value) {
    if (!value.has_value) {
        wire_i32(buffer, -1);
    } else {
        wire_string(buffer, value.value);
    }
}

static void wire_identity(dump_buffer *buffer, markdown_core_identity identity) {
    wire_u32(buffer, identity.block);
    wire_u32(buffer, identity.ordinal);
}

static void wire_node(dump_buffer *buffer, const markdown_core_node *node);

static void wire_children(dump_buffer *buffer, const markdown_core_node *node) {
    markdown_core_children cursor;
    size_t count = markdown_core_node_child_count(node);
    if (count > INT32_MAX) {
        buffer->failed = true;
        return;
    }
    wire_i32(buffer, (int32_t)count);
    for (cursor = markdown_core_node_children(node); cursor.child; cursor = markdown_core_children_next(cursor)) {
        wire_node(buffer, cursor.child);
    }
}

static void wire_node(dump_buffer *buffer, const markdown_core_node *node) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_string first = {NULL, 0};
    markdown_core_string second = {NULL, 0};
    markdown_core_string third = {NULL, 0};
    markdown_core_optional_string optional_first = {false, {NULL, 0}};
    markdown_core_optional_string optional_second = {false, {NULL, 0}};
    markdown_core_scope scope = markdown_core_node_scope(node);
    markdown_core_identity definition = {0, 0};

    wire_u8(buffer, (uint8_t)kind);
    wire_identity(buffer, markdown_core_node_identifier(node));
    wire_i32(buffer, scope.start.line);
    wire_i32(buffer, scope.start.column);
    wire_i32(buffer, scope.end.line);
    wire_i32(buffer, scope.end.column);

    switch (kind) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE:
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        wire_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_HEADING: {
        int32_t level = 0;
        markdown_core_node_heading_level(node, &level);
        wire_i32(buffer, level);
        wire_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
        break;
    case MARKDOWN_CORE_KIND_LIST: {
        markdown_core_list_flavor flavor = MARKDOWN_CORE_LIST_FLAVOR_BULLET;
        markdown_core_optional_i64 start = {false, 0};
        bool tight = false;
        markdown_core_node_list_properties(node, &flavor, &start, &tight);
        wire_i32(buffer, (int32_t)flavor);
        wire_i64(buffer, start.value);
        wire_u8(buffer, start.has_value ? 1 : 0);
        wire_u8(buffer, tight ? 1 : 0);
        wire_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_LIST_ITEM: {
        markdown_core_optional_bool checked = {false, false};
        markdown_core_node_list_item_checked(node, &checked);
        wire_u8(buffer, checked.has_value ? (checked.value ? 1 : 0) : UINT8_MAX);
        wire_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_CODE_BLOCK: {
        bool fenced = false;
        bool closed = false;
        markdown_core_node_code_block_properties(node, &optional_first, &optional_second, &third, &fenced, &closed);
        wire_optional_string(buffer, optional_first);
        wire_optional_string(buffer, optional_second);
        wire_string(buffer, third);
        wire_u8(buffer, fenced ? 1 : 0);
        wire_u8(buffer, closed ? 1 : 0);
        break;
    }
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
        markdown_core_node_literal(node, &first);
        wire_string(buffer, first);
        break;
    case MARKDOWN_CORE_KIND_FORMULA: {
        /* The one kind whose mode is a fact about the source rather than about
         * the kind; the other five stopped carrying it at Q29. */
        markdown_core_placement_mode mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED;
        markdown_core_node_formula_properties(node, &mode, &first);
        wire_i32(buffer, (int32_t)mode);
        wire_string(buffer, first);
        break;
    }
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: {
        markdown_core_placement_mode mode = MARKDOWN_CORE_PLACEMENT_EMBEDDED;
        markdown_core_node_formula_properties(node, &mode, &first);
        wire_string(buffer, first);
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
        wire_i32(buffer, (int32_t)count);
        for (index = 0; index < count; index++) {
            markdown_core_table_alignment alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE;
            markdown_core_node_table_alignment_at(node, index, &alignment);
            wire_u8(buffer, (uint8_t)alignment);
        }
        wire_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
    case MARKDOWN_CORE_KIND_DIRECTIVE: {
        /* The label is a CHILD NODE, so it needs no wire slot of its own: the
         * children are written whole and the decoder tells a label from
         * content by its kind. What is still spelled out is the attribute
         * sequence, because a count of pairs is not something wire_children
         * can carry. */
        bool has_attributes = false;
        size_t count = 0;
        size_t index;
        markdown_core_node_directive_properties(node, &first, &has_attributes, &count);
        wire_string(buffer, first);
        wire_u8(buffer, has_attributes ? 1 : 0);
        if (count > INT32_MAX) {
            buffer->failed = true;
            return;
        }
        wire_i32(buffer, has_attributes ? (int32_t)count : 0);
        for (index = 0; has_attributes && index < count; index++) {
            if (!markdown_core_node_directive_attribute_at(node, index, &first, &second)) {
                buffer->failed = true;
                return;
            }
            wire_string(buffer, first);
            wire_string(buffer, second);
        }
        wire_children(buffer, node);
        break;
    }
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
        markdown_core_node_association(node, &first, &second);
        wire_string(buffer, first);
        wire_string(buffer, second);
        wire_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION:
        markdown_core_node_association(node, &first, &second);
        markdown_core_node_definition_resource(node, &third, &optional_first);
        wire_string(buffer, first);
        wire_string(buffer, second);
        wire_string(buffer, third);
        wire_optional_string(buffer, optional_first);
        break;
    /* A reference carries its label and the identity of the definition it
     * resolved to; its own match key is not on the wire, because it equals the
     * winning definition's by construction. */
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_association(node, &first, &second);
        markdown_core_node_reference_definition(node, &definition);
        wire_string(buffer, first);
        wire_identity(buffer, definition);
        break;
    case MARKDOWN_CORE_KIND_LINK_REFERENCE:
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: {
        markdown_core_reference_form form = MARKDOWN_CORE_REFERENCE_SHORTCUT;
        markdown_core_node_association(node, &first, &second);
        markdown_core_node_reference_form(node, &form);
        markdown_core_node_reference_definition(node, &definition);
        wire_string(buffer, first);
        wire_i32(buffer, (int32_t)form);
        wire_identity(buffer, definition);
        wire_children(buffer, node);
        break;
    }
    /* A DESTINATION IS REQUIRED (Q26) and always has a length on the wire. */
    case MARKDOWN_CORE_KIND_LINK:
        markdown_core_node_link_properties(node, &first, &optional_first);
        wire_string(buffer, first);
        wire_optional_string(buffer, optional_first);
        wire_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_image_properties(node, &first, &optional_first);
        wire_string(buffer, first);
        wire_optional_string(buffer, optional_first);
        wire_children(buffer, node);
        break;
    case MARKDOWN_CORE_KIND_TABLE_ROW: {
        bool header = false;
        markdown_core_node_table_row_is_header(node, &header);
        wire_u8(buffer, header ? 1 : 0);
        wire_children(buffer, node);
        break;
    }
    default:
        buffer->failed = true;
        break;
    }
}

/* THE MANAGED FEED (#146): feed, derive, and serialize in one synchronous
 * call. The composed path -- `markdown_core_session_feed` then
 * `markdown_core_document_wire` then `markdown_core_document_free` -- builds
 * a fully-owned document whose only reader is the serializer, and the
 * document dies before the call returns; here the derived tree is freed
 * before returning, so the owned document's lifecycle drops out of every
 * managed feed. The bytes are the composed path's, gated byte-for-byte by
 * the equivalence test. C consumers keep `markdown_core_session_feed`'s
 * owned document; this entry is for bridges whose document never escapes
 * the delivering call. */
bool markdown_core_session_feed_wire(
    markdown_core_session *session,
    const uint8_t *chunk,
    size_t length,
    size_t prefix,
    uint8_t **output,
    size_t *output_length,
    markdown_core_error **error
) {
    markdown_core_parser *parser;
    markdown_core_node *root;
    dump_buffer buffer = {0};

    clear_error(error);
    if (!output || !output_length) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "output and length must not be null");
        return false;
    }
    *output = NULL;
    *output_length = 0;
    if (!session || !session->parser) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "the session is finished or null");
        return false;
    }
    if (!chunk && length != 0) {
        set_error(error, MARKDOWN_CORE_ERROR_INVALID_ARGUMENT, "chunk must not be null when length is nonzero");
        return false;
    }
    parser = session->parser;
    if (length) {
        markdown_core_parser_feed(parser, (const char *)chunk, length);
    }
    root = markdown_core_parser_derive_tree(parser, parser->refmap);
    if (!root) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "the parse lost bytes it could not allocate for");
        return false;
    }
    buffer_reserve(&buffer, prefix);
    if (!buffer.failed && prefix != 0) {
        memset(buffer.data, 0, prefix);
        buffer.size = prefix;
    }
    wire_node(&buffer, root);
    markdown_core_node_free(root);
    if (buffer.failed) {
        free(buffer.data);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not serialize the document");
        return false;
    }
    *output = buffer.data;
    *output_length = buffer.size;
    return true;
}

bool markdown_core_document_wire(
    const markdown_core_document *document,
    size_t prefix,
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
    /* The caller's envelope room, zeroed, IN the one allocation: a transport
     * that wrapped the payload afterwards was allocating a second full-size
     * buffer and copying the first into it while both were live. */
    buffer_reserve(&buffer, prefix);
    if (!buffer.failed && prefix != 0) {
        memset(buffer.data, 0, prefix);
        buffer.size = prefix;
    }
    wire_node(&buffer, document->root);
    if (buffer.failed) {
        free(buffer.data);
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not serialize the document");
        return false;
    }
    *output = buffer.data;
    *length = buffer.size;
    return true;
}

void markdown_core_wire_free(uint8_t *output) { free(output); }
