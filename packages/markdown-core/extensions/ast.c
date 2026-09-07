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

/* A parse error means there is no document and carries no source scope. Error
 * values are immutable process-lifetime sentinels. Reporting allocation
 * failure must itself allocate nothing; otherwise the consumer can receive
 * neither a document nor the error that explains its absence. */
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

static const markdown_core_error ERROR_INVALID_SOURCE = {MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
                                                         "source must not be null when length is nonzero"};
static const markdown_core_error ERROR_INVALID_ALLOCATOR = {MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
                                                            "memory allocator must not be null"};
static const markdown_core_error ERROR_DOCUMENT_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                              "could not allocate document"};
static const markdown_core_error ERROR_PARSE_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                           "the parse could not complete an allocation"};
static const markdown_core_error ERROR_INVALID_DUMP = {MARKDOWN_CORE_ERROR_INVALID_ARGUMENT,
                                                       "document, output, and length must not be null"};
static const markdown_core_error ERROR_DUMP_ALLOCATION = {MARKDOWN_CORE_ERROR_ALLOCATION_FAILED,
                                                          "could not produce canonical AST dump"};

static void set_error(markdown_core_error **error, const markdown_core_error *value) {
    if (!error) {
        return;
    }
    *error = (markdown_core_error *)(uintptr_t)value;
}

static bool configure_facade_parse(markdown_core_parser *parser, void *context) {
    (void)context;
    /* The facade never says WHICH extensions: `core-extensions.c` owns the one
     * list and the one order used by every product entry. */
    return markdown_core_core_extensions_attach(parser);
}

/* THE ONE PARSE TRANSACTION. Every caller runs it over the whole dialect:
 * the public entry supplies the default allocator, and the allocation-failure
 * tests supply an injected one. Nothing else builds a parser, so there is
 * exactly one language and no way to parse a part of it. */
markdown_core_document *markdown_core_document_parse_with_mem(const uint8_t *source, size_t length,
                                                              markdown_core_mem *mem, markdown_core_error **error) {
    markdown_core_document *document;

    clear_error(error);
    if (!source && length != 0) {
        set_error(error, &ERROR_INVALID_SOURCE);
        return NULL;
    }
    if (!mem) {
        set_error(error, &ERROR_INVALID_ALLOCATOR);
        return NULL;
    }
    document = (markdown_core_document *)mem->calloc(1, sizeof(*document));
    if (!document) {
        set_error(error, &ERROR_DOCUMENT_ALLOCATION);
        return NULL;
    }
    document->mem = mem;

    document->root = markdown_core_parse_document_with_mem((const char *)source, length, MARKDOWN_CORE_DIALECT_OPTIONS,
                                                           mem, configure_facade_parse, NULL);
    if (!document->root) {
        mem->free(document);
        set_error(error, &ERROR_PARSE_ALLOCATION);
        return NULL;
    }
    return document;
}

markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                     markdown_core_error **error) {
    return markdown_core_document_parse_with_mem(source, length, markdown_core_get_default_mem_allocator(), error);
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document) {
        return;
    }
    markdown_core_node_free(document->root);
    document->mem->free(document);
}

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) {
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

void markdown_core_error_free(markdown_core_error *error) { (void)error; }

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) {
    if (!node) {
        return MARKDOWN_CORE_KIND_NONE;
    }
    if (node->type == MARKDOWN_CORE_NODE_DOCUMENT) {
        return MARKDOWN_CORE_KIND_DOCUMENT;
    }
    if (node->type == MARKDOWN_CORE_NODE_CALLOUT) {
        return MARKDOWN_CORE_KIND_CALLOUT;
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
    /* One public kind for both internal types: the parent edge says which
     * content a comment sits in, and the node stores no placement. */
    if (node->type == MARKDOWN_CORE_NODE_COMMENT || node->type == MARKDOWN_CORE_NODE_COMMENT_BLOCK) {
        return MARKDOWN_CORE_KIND_COMMENT;
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
    if (node->type == MARKDOWN_CORE_NODE_CITE) {
        return MARKDOWN_CORE_KIND_CITE;
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
    /* One name per line: the projection audit reads this table as data. */
    /* clang-format off */
    static const char *const names[] = {
        "None",
        "Document",
        "Callout",
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
        "Cite",
        "TableRow",
        "TableCell",
        "DirectiveLabel",
        "Comment"};
    /* clang-format on */
    if (kind < MARKDOWN_CORE_KIND_NONE || kind > MARKDOWN_CORE_KIND_COMMENT) {
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

static bool is_directive(const markdown_core_node *node) {
    return node && (node->type == MARKDOWN_CORE_NODE_DIRECTIVE || node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK);
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

bool markdown_core_node_heading_level(const markdown_core_node *node, int32_t *level) {
    if (!node || node->type != MARKDOWN_CORE_NODE_HEADING || !level) {
        return false;
    }
    *level = node->as.heading.level;
    return true;
}

bool markdown_core_node_list_properties(const markdown_core_node *node, markdown_core_list_flavor *flavor,
                                        markdown_core_optional_i64 *start, markdown_core_ordered_list_variant *variant,
                                        markdown_core_ordered_list_delimiter *delimiter, bool *tight) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST || !flavor || !start || !variant || !delimiter || !tight) {
        return false;
    }
    *flavor = node->as.list.list_type == MARKDOWN_CORE_ORDERED_LIST ? MARKDOWN_CORE_LIST_FLAVOR_ORDERED
                                                                    : MARKDOWN_CORE_LIST_FLAVOR_BULLET;
    start->has_value = *flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    start->value = node->as.list.start;
    *variant = MARKDOWN_CORE_ORDERED_LIST_VARIANT_DECIMAL;
    delimiter->kind = node->as.list.delimiter == MARKDOWN_CORE_PAREN_DELIM
                          ? MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS
                          : MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD;
    delimiter->closed = false;
    *tight = node->as.list.tight;
    return true;
}

bool markdown_core_node_list_item_properties(const markdown_core_node *node, markdown_core_optional_string *marker,
                                              markdown_core_optional_string *example_label) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST_ITEM || !marker || !example_label) {
        return false;
    }
    marker->has_value =
        node->extension && strcmp(markdown_core_node_get_type_string((markdown_core_node *)node), "tasklist") == 0;
    marker->value.data = &node->as.list.task_marker;
    marker->value.length = marker->has_value ? 1 : 0;
    example_label->has_value = false;
    example_label->value.data = NULL;
    example_label->value.length = 0;
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

bool markdown_core_node_code_block_properties(const markdown_core_node *node, markdown_core_optional_string *info,
                                              markdown_core_optional_string *language, markdown_core_string *literal,
                                              bool *fenced, bool *closed) {
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
    case MARKDOWN_CORE_NODE_COMMENT:
    case MARKDOWN_CORE_NODE_COMMENT_BLOCK:
        string_from_chunk(literal, &node->as.literal);
        return true;
    default:
        return false;
    }
}

bool markdown_core_node_formula_properties(const markdown_core_node *node, markdown_core_placement_mode *mode,
                                           markdown_core_string *literal) {
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

/* ATTRIBUTES ARE AN ORDERED SEQUENCE in first-occurrence source order. The JSON string
 * this used to hand out was a second representation of the list the parser
 * already holds, with a parser of its own to read it back; both are gone.
 * `has_attributes` distinguishes `:n` from `:n{}` -- absent from empty -- which
 * the old `null` versus `"{}"` said and a count alone cannot. */
bool markdown_core_node_directive_properties(const markdown_core_node *node, markdown_core_string *name,
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

bool markdown_core_node_directive_attribute_at(const markdown_core_node *node, size_t index, markdown_core_string *name,
                                               markdown_core_string *value) {
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

const markdown_core_node *markdown_core_node_directive_label(const markdown_core_node *node) {
    return is_directive(node) ? markdown_core_directive_label((markdown_core_node *)node) : NULL;
}

static bool is_callout(const markdown_core_node *node) { return node && node->type == MARKDOWN_CORE_NODE_CALLOUT; }

bool markdown_core_node_callout_properties(const markdown_core_node *node, markdown_core_optional_string *variant,
                                           markdown_core_optional_bool *collapsed) {
    if (!is_callout(node) || !variant || !collapsed) {
        return false;
    }
    /* Every `>` container is metadata-free until the callouts module's
     * metadata rule lands with O8: no variant, no fold marker, no title. */
    variant->has_value = false;
    variant->value.data = NULL;
    variant->value.length = 0;
    collapsed->has_value = false;
    collapsed->value = false;
    return true;
}

const markdown_core_node *markdown_core_node_callout_title(const markdown_core_node *node) {
    /* No callout carries a title until O8, and a non-callout never does; a
     * present title holds at least one node, so its first node is its
     * presence. */
    (void)node;
    return NULL;
}

static bool is_link(const markdown_core_node *node) {
    return node && (node->type == MARKDOWN_CORE_NODE_LINK || node->type == MARKDOWN_CORE_NODE_IMAGE);
}

/* Every link and image the parser produces reads through a resource, and
 * only the parser creates one. A node built by hand has none and is the link
 * `[a]()` is: the empty url and no title. */
static const markdown_core_chunk empty_url = {(unsigned char *)"", 0, 0};
static const markdown_core_optional_chunk absent_title = {{NULL, 0, 0}, false};

bool markdown_core_node_destination(const markdown_core_node *node, markdown_core_destination *destination) {
    if (!is_link(node) || !destination) {
        return false;
    }
    /* Every link and image the inherited grammar produces is the `url`
     * branch; the `cross` branch arrives with the cross links of `O1`. The
     * other branch's fields are zeroed, not left over. */
    memset(destination, 0, sizeof(*destination));
    destination->kind = MARKDOWN_CORE_DESTINATION_URL;
    string_from_chunk(&destination->url, node->as.link.resource ? &node->as.link.resource->url : &empty_url);
    return true;
}

bool markdown_core_node_title(const markdown_core_node *node, markdown_core_optional_string *title) {
    if (!is_link(node) || !title) {
        return false;
    }
    optional_string_from_chunk(title, node->as.link.resource ? &node->as.link.resource->title : &absent_title);
    return true;
}

const markdown_core_resource *markdown_core_node_resource(const markdown_core_node *node) {
    return is_link(node) ? node->as.link.resource : NULL;
}

/* THE VALUES (M4). A citation and a footnote are nodes inside the engine and
 * opaque handles outside it: the handle types are never defined, so the only
 * way through one is these accessors, and each of them checks the node's
 * type rather than trusting the cast. */
static const markdown_core_node *citation_node(const markdown_core_citation *citation) {
    const markdown_core_node *node = (const markdown_core_node *)citation;
    return node && node->type == MARKDOWN_CORE_NODE_CITATION ? node : NULL;
}

static const markdown_core_node *footnote_node(const markdown_core_footnote *footnote) {
    const markdown_core_node *node = (const markdown_core_node *)footnote;
    return node && node->type == MARKDOWN_CORE_NODE_FOOTNOTE ? node : NULL;
}

const markdown_core_citation *markdown_core_node_cite_citations(const markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_CITE ? (const markdown_core_citation *)node->as.cite.citations
                                                         : NULL;
}

const markdown_core_citation *markdown_core_citation_next(const markdown_core_citation *citation) {
    const markdown_core_node *node = citation_node(citation);
    return node ? (const markdown_core_citation *)node->next : NULL;
}

markdown_core_scope markdown_core_citation_scope(const markdown_core_citation *citation) {
    return markdown_core_node_scope(citation_node(citation));
}

bool markdown_core_citation_referent(const markdown_core_citation *citation, markdown_core_referent *referent) {
    const markdown_core_node *node = citation_node(citation);
    if (!node || !referent) {
        return false;
    }
    memset(referent, 0, sizeof(*referent));
    if (node->as.citation.referent == MARKDOWN_CORE_NODE_REFERENT_BIB) {
        referent->kind = MARKDOWN_CORE_REFERENT_BIB;
        string_from_chunk(&referent->key, &node->as.citation.value);
        referent->mode = (markdown_core_bib_mode)node->as.citation.mode;
    } else {
        referent->kind = MARKDOWN_CORE_REFERENT_FOOTNOTE;
        string_from_chunk(&referent->id, &node->as.citation.value);
    }
    return true;
}

const markdown_core_node *markdown_core_citation_prefix(const markdown_core_citation *citation) {
    const markdown_core_node *node = citation_node(citation);
    return node ? node->as.citation.prefix : NULL;
}

const markdown_core_node *markdown_core_citation_suffix(const markdown_core_citation *citation) {
    const markdown_core_node *node = citation_node(citation);
    return node ? node->as.citation.suffix : NULL;
}

const markdown_core_footnote *markdown_core_node_document_footnotes(const markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_DOCUMENT
               ? (const markdown_core_footnote *)node->as.document.footnotes
               : NULL;
}

const markdown_core_footnote *markdown_core_footnote_next(const markdown_core_footnote *footnote) {
    const markdown_core_node *node = footnote_node(footnote);
    return node ? (const markdown_core_footnote *)node->next : NULL;
}

markdown_core_scope markdown_core_footnote_scope(const markdown_core_footnote *footnote) {
    return markdown_core_node_scope(footnote_node(footnote));
}

bool markdown_core_footnote_id(const markdown_core_footnote *footnote, markdown_core_string *id) {
    const markdown_core_node *node = footnote_node(footnote);
    if (!node || !id) {
        return false;
    }
    string_from_chunk(id, &node->as.footnote.id);
    return true;
}

const markdown_core_node *markdown_core_footnote_content(const markdown_core_footnote *footnote) {
    const markdown_core_node *node = footnote_node(footnote);
    return node ? node->first_child : NULL;
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

static const char *mode_name(markdown_core_placement_mode mode) {
    return mode == MARKDOWN_CORE_PLACEMENT_EMBEDDED ? "embedded" : "standalone";
}

/* A tagged value prints its branch and its named fields with no spaces
 * (canonical-ast-dump.md): `url("...")`, or `cross(path="...",anchor=null)`.
 * Kept OUTSIDE `dump_fields`, whose body the projection audit reads for the
 * `name=` literals a kind prints: the branch fields are the value's, not the
 * node's. */
static void buffer_destination(dump_buffer *buffer, markdown_core_destination destination) {
    if (destination.kind == MARKDOWN_CORE_DESTINATION_CROSS) {
        buffer_cstr(buffer, "cross(path=");
        buffer_json_string(buffer, destination.path);
        buffer_cstr(buffer, ",anchor=");
        buffer_optional_string(buffer, destination.anchor);
        buffer_cstr(buffer, ")");
        return;
    }
    buffer_cstr(buffer, "url(");
    buffer_json_string(buffer, destination.url);
    buffer_cstr(buffer, ")");
}

static void buffer_optional_bool(dump_buffer *buffer, markdown_core_optional_bool value) {
    if (value.has_value) {
        buffer_cstr(buffer, value.value ? "true" : "false");
    } else {
        buffer_cstr(buffer, "null");
    }
}

static void dump_fields(dump_buffer *buffer, const markdown_core_node *node, markdown_core_node_kind kind) {
    markdown_core_string a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0};
    markdown_core_optional_string oa = {false, {NULL, 0}}, ob = {false, {NULL, 0}};
    markdown_core_optional_i64 start;
    markdown_core_optional_bool collapsed;
    markdown_core_ordered_list_variant variant;
    markdown_core_ordered_list_delimiter delimiter;
    markdown_core_list_flavor flavor;
    markdown_core_placement_mode mode;
    markdown_core_destination destination;
    bool x, y, has_attributes;
    size_t count, i;
    int32_t level;
    switch (kind) {
    case MARKDOWN_CORE_KIND_CALLOUT:
        markdown_core_node_callout_properties(node, &oa, &collapsed);
        buffer_cstr(buffer, " variant=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " collapsed=");
        buffer_optional_bool(buffer, collapsed);
        break;
    case MARKDOWN_CORE_KIND_HEADING:
        markdown_core_node_heading_level(node, &level);
        buffer_cstr(buffer, " level=");
        buffer_i64(buffer, level);
        break;
    case MARKDOWN_CORE_KIND_LIST:
        markdown_core_node_list_properties(node, &flavor, &start, &variant, &delimiter, &x);
        buffer_cstr(buffer, " flavor=");
        buffer_cstr(buffer, flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? "ordered" : "bullet");
        buffer_cstr(buffer, " start=");
        if (start.has_value) {
            buffer_i64(buffer, start.value);
        } else {
            buffer_cstr(buffer, "null");
        }
        buffer_cstr(buffer, " variant=");
        buffer_cstr(buffer, flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED ? "decimal" : "null");
        buffer_cstr(buffer, " delimiter=");
        if (flavor == MARKDOWN_CORE_LIST_FLAVOR_BULLET) {
            buffer_cstr(buffer, "null");
        } else {
            if (delimiter.kind == MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS) {
                buffer_cstr(buffer, "parenthesis(closed");
                buffer_cstr(buffer, "=false)");
            } else {
                buffer_cstr(buffer, "period");
            }
        }
        buffer_cstr(buffer, " tight=");
        buffer_cstr(buffer, x ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_properties(node, &oa, &ob);
        buffer_cstr(buffer, " marker=");
        buffer_optional_string(buffer, oa);
        buffer_cstr(buffer, " exampleLabel=");
        buffer_optional_string(buffer, ob);
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
    case MARKDOWN_CORE_KIND_COMMENT:
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
        } else {
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
        }
        break;
    /* A DESTINATION IS REQUIRED (Q26): `dest=` is the tagged value and is
     * never `null`. `[a]()` used to print `destination=null`, which said the
     * author wrote no destination when the empty parentheses are the
     * destination they wrote; it is `dest=url("")` now. */
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_IMAGE:
        markdown_core_node_destination(node, &destination);
        markdown_core_node_title(node, &oa);
        buffer_cstr(buffer, " dest=");
        buffer_destination(buffer, destination);
        buffer_cstr(buffer, " title=");
        buffer_optional_string(buffer, oa);
        break;
    default:
        break;
    }
}

static void dump_node(dump_buffer *buffer, const markdown_core_node *node, size_t depth);

/* The file-tree connectors that lead a line at `depth`. */
static void dump_prefix(dump_buffer *buffer, size_t depth) {
    size_t i;
    if (!depth) {
        return;
    }
    for (i = 0; i + 1 < depth; i++) {
        buffer_cstr(buffer, buffer->more[i] ? "│   " : "    ");
    }
    buffer_cstr(buffer, buffer->more[depth - 1] ? "├── " : "└── ");
}

/* The file-tree drawing can nest both child nodes and node-valued fields.  The
 * caller states their total so connectors remain a formatting concern rather
 * than redefining either relation as the other. */
static void dump_nested_node(dump_buffer *buffer, const markdown_core_node *node, size_t depth, bool has_next) {
    if (!ensure_more(buffer, depth)) {
        return;
    }
    buffer->more[depth] = has_next;
    dump_node(buffer, node, depth + 1);
}

static void dump_children(dump_buffer *buffer, const markdown_core_node *node, size_t depth, size_t remaining_nested) {
    const markdown_core_node *child = markdown_core_node_get_first_child(node);
    while (child) {
        const markdown_core_node *next = markdown_core_node_get_next_sibling(child);
        remaining_nested--;
        dump_nested_node(buffer, child, depth, remaining_nested != 0);
        child = next;
    }
}

static void dump_directive_nodes(dump_buffer *buffer, const markdown_core_node *node, size_t depth,
                                 size_t child_count) {
    const markdown_core_node *label = markdown_core_node_directive_label(node);
    size_t remaining = child_count + (label ? 1u : 0u);
    if (label) {
        remaining--;
        dump_nested_node(buffer, label, depth, remaining != 0);
    }
    dump_children(buffer, node, depth, remaining);
}

/* A group line nests a node-valued list under its owner: `Kind children=N`
 * with no scope and no fields, at the owner's nesting depth, and the list's
 * nodes one level below it. */
static void dump_group_line(dump_buffer *buffer, const char *name, size_t count, size_t depth, bool has_next) {
    if (!ensure_more(buffer, depth)) {
        return;
    }
    buffer->more[depth] = has_next;
    dump_prefix(buffer, depth + 1);
    buffer_cstr(buffer, name);
    buffer_cstr(buffer, " children=");
    buffer_i64(buffer, (int64_t)count);
    buffer_cstr(buffer, "\n");
}

/* A callout's `title` is a node-valued field, never callout content: a
 * non-null title is a `Title` group before the content, and a null one
 * prints nothing (M3). */
static void dump_callout_nodes(dump_buffer *buffer, const markdown_core_node *node, size_t depth, size_t child_count) {
    const markdown_core_node *title = markdown_core_node_callout_title(node);
    size_t remaining = child_count + (title ? 1u : 0u);
    if (title) {
        const markdown_core_node *cursor;
        size_t count = 0;
        for (cursor = title; cursor; cursor = markdown_core_node_get_next_sibling(cursor)) {
            count++;
        }
        remaining--;
        dump_group_line(buffer, "Title", count, depth, remaining != 0);
        for (cursor = title; cursor; cursor = markdown_core_node_get_next_sibling(cursor)) {
            count--;
            dump_nested_node(buffer, cursor, depth + 1, count != 0);
        }
    }
    dump_children(buffer, node, depth, remaining);
}

static size_t chain_length(const markdown_core_node *first) {
    size_t count = 0;
    for (; first; first = first->next) {
        count++;
    }
    return count;
}

static void buffer_scope(dump_buffer *buffer, markdown_core_scope scope) {
    buffer_i64(buffer, scope.start.line);
    buffer_cstr(buffer, ":");
    buffer_i64(buffer, scope.start.column);
    buffer_cstr(buffer, "..");
    buffer_i64(buffer, scope.end.line);
    buffer_cstr(buffer, ":");
    buffer_i64(buffer, scope.end.column);
}

static const char *bib_mode_name(markdown_core_bib_mode mode) {
    switch (mode) {
    case MARKDOWN_CORE_BIB_MODE_AUTHOR_IN_TEXT:
        return "authorInText";
    case MARKDOWN_CORE_BIB_MODE_SUPPRESS_AUTHOR:
        return "suppressAuthor";
    default:
        return "normal";
    }
}

/* A tagged value prints its branch and named fields with no spaces, as
 * `dest` does. */
static void buffer_referent(dump_buffer *buffer, markdown_core_referent referent) {
    if (referent.kind == MARKDOWN_CORE_REFERENT_BIB) {
        buffer_cstr(buffer, "bib(key=");
        buffer_json_string(buffer, referent.key);
        buffer_cstr(buffer, ",mode=");
        buffer_cstr(buffer, bib_mode_name(referent.mode));
        buffer_cstr(buffer, ")");
    } else {
        buffer_cstr(buffer, "footnote(id=");
        buffer_json_string(buffer, referent.id);
        buffer_cstr(buffer, ")");
    }
}

/* An affix is a group under its item: the group line names the affix and
 * counts its nodes, which nest one level below it. */
static void dump_affix_group(dump_buffer *buffer, const char *name, const markdown_core_node *first, size_t depth,
                             bool has_next) {
    size_t count = chain_length(first);
    dump_group_line(buffer, name, count, depth, has_next);
    for (; first; first = first->next) {
        count--;
        dump_nested_node(buffer, first, depth + 1, count != 0);
    }
}

/* A cite's items are scoped values nested under it (M4): each prints a
 * `Citation` value line -- scope, referent, and a `children` of zero, since
 * its affixes are groups, not children -- and then a `CitationPrefix` and a
 * `CitationSuffix` group. The cite's own `children` counts the items. */
static void dump_cite_nodes(dump_buffer *buffer, const markdown_core_node *node, size_t depth, size_t item_count) {
    const markdown_core_node *item = node->as.cite.citations;
    size_t remaining = item_count;
    for (; item; item = item->next) {
        markdown_core_referent referent;
        remaining--;
        if (!ensure_more(buffer, depth)) {
            return;
        }
        buffer->more[depth] = remaining != 0;
        dump_prefix(buffer, depth + 1);
        buffer_cstr(buffer, "Citation scope=");
        buffer_scope(buffer, markdown_core_node_scope(item));
        markdown_core_citation_referent((const markdown_core_citation *)item, &referent);
        buffer_cstr(buffer, " referent=");
        buffer_referent(buffer, referent);
        buffer_cstr(buffer, " children=0\n");
        dump_affix_group(buffer, "CitationPrefix", item->as.citation.prefix, depth + 1, true);
        dump_affix_group(buffer, "CitationSuffix", item->as.citation.suffix, depth + 1, false);
    }
}

/* The document's footnotes are scoped values nested after its content (M4):
 * each prints a `Footnote` value line with its id and its content count, then
 * its block content one level below. The document's own `children` counts
 * the content alone. */
static void dump_document_nodes(dump_buffer *buffer, const markdown_core_node *node, size_t depth, size_t child_count) {
    const markdown_core_node *footnote = node->as.document.footnotes;
    size_t remaining = child_count + chain_length(footnote);
    dump_children(buffer, node, depth, remaining);
    remaining -= child_count;
    for (; footnote; footnote = footnote->next) {
        size_t content = markdown_core_node_child_count(footnote);
        markdown_core_string id;
        remaining--;
        if (!ensure_more(buffer, depth)) {
            return;
        }
        buffer->more[depth] = remaining != 0;
        dump_prefix(buffer, depth + 1);
        buffer_cstr(buffer, "Footnote scope=");
        buffer_scope(buffer, markdown_core_node_scope(footnote));
        markdown_core_footnote_id((const markdown_core_footnote *)footnote, &id);
        buffer_cstr(buffer, " id=");
        buffer_json_string(buffer, id);
        buffer_cstr(buffer, " children=");
        buffer_i64(buffer, (int64_t)content);
        buffer_cstr(buffer, "\n");
        dump_children(buffer, footnote, depth + 1, content);
    }
}

static void dump_node(dump_buffer *buffer, const markdown_core_node *node, size_t depth) {
    markdown_core_node_kind kind = markdown_core_node_get_kind(node);
    markdown_core_scope scope = markdown_core_node_scope(node);
    /* `children` counts structural children: a cite's are its items. */
    size_t child_count =
        kind == MARKDOWN_CORE_KIND_CITE ? chain_length(node->as.cite.citations) : markdown_core_node_child_count(node);
    if (kind == MARKDOWN_CORE_KIND_NONE) {
        buffer->failed = true;
        return;
    }
    dump_prefix(buffer, depth);
    buffer_cstr(buffer, markdown_core_node_kind_name(kind));
    buffer_cstr(buffer, " scope=");
    buffer_scope(buffer, scope);
    dump_fields(buffer, node, kind);
    buffer_cstr(buffer, " children=");
    buffer_i64(buffer, (int64_t)child_count);
    buffer_cstr(buffer, "\n");

    /* Like cmark's render callback, this switch belongs to the node being
     * emitted.  Generic child traversal never discovers fields. A directive
     * explicitly emits its label field before its independent content list. */
    switch (kind) {
    case MARKDOWN_CORE_KIND_DIRECTIVE:
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        dump_directive_nodes(buffer, node, depth, child_count);
        break;
    case MARKDOWN_CORE_KIND_CALLOUT:
        dump_callout_nodes(buffer, node, depth, child_count);
        break;
    case MARKDOWN_CORE_KIND_DOCUMENT:
        dump_document_nodes(buffer, node, depth, child_count);
        break;
    case MARKDOWN_CORE_KIND_CITE:
        dump_cite_nodes(buffer, node, depth, child_count);
        break;
    case MARKDOWN_CORE_KIND_PARAGRAPH:
    case MARKDOWN_CORE_KIND_HEADING:
    case MARKDOWN_CORE_KIND_LIST:
    case MARKDOWN_CORE_KIND_LIST_ITEM:
    case MARKDOWN_CORE_KIND_TABLE:
    case MARKDOWN_CORE_KIND_TABLE_ROW:
    case MARKDOWN_CORE_KIND_TABLE_CELL:
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
    case MARKDOWN_CORE_KIND_EMPHASIS:
    case MARKDOWN_CORE_KIND_STRONG:
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
    case MARKDOWN_CORE_KIND_LINK:
    case MARKDOWN_CORE_KIND_IMAGE:
        dump_children(buffer, node, depth, child_count);
        break;
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK:
    case MARKDOWN_CORE_KIND_CODE_BLOCK:
    case MARKDOWN_CORE_KIND_HTML_BLOCK:
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK:
    case MARKDOWN_CORE_KIND_TEXT:
    case MARKDOWN_CORE_KIND_SOFT_BREAK:
    case MARKDOWN_CORE_KIND_LINE_BREAK:
    case MARKDOWN_CORE_KIND_CODE:
    case MARKDOWN_CORE_KIND_HTML:
    case MARKDOWN_CORE_KIND_COMMENT:
    case MARKDOWN_CORE_KIND_FORMULA:
    case MARKDOWN_CORE_KIND_NONE:
        break;
    }
}

bool markdown_core_document_dump(const markdown_core_document *document, uint8_t **output, size_t *length,
                                 markdown_core_error **error) {
    dump_buffer buffer = {0};
    clear_error(error);
    if (!document || !document->root || !output || !length) {
        set_error(error, &ERROR_INVALID_DUMP);
        return false;
    }
    *output = NULL;
    *length = 0;
    dump_node(&buffer, document->root, 0);
    free(buffer.more);
    if (buffer.failed) {
        free(buffer.data);
        set_error(error, &ERROR_DUMP_ALLOCATION);
        return false;
    }
    *output = buffer.data;
    *length = buffer.size;
    return true;
}

void markdown_core_dump_free(uint8_t *output) { free(output); }
