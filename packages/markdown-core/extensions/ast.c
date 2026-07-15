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
#include <registry.h>

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
    bool *more;
    size_t more_capacity;
} dump_buffer;

static void clear_error(markdown_core_error **error) {
    if (error)
        *error = NULL;
}

static void set_error(markdown_core_error **error, markdown_core_error_code code, const char *message) {
    markdown_core_error *value;
    size_t length;
    if (!error)
        return;
    value = (markdown_core_error *)calloc(1, sizeof(*value));
    if (!value)
        return;
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

void markdown_core_parse_options_init(markdown_core_parse_options *options) {
    if (!options)
        return;
    options->smart_punctuation = true;
    options->footnotes = true;
    options->strip_html_comments = true;
    options->tables = true;
    options->strikethrough = true;
    options->autolinks = true;
    options->task_lists = true;
    options->formulas = true;
    options->dollar_formula_delimiters = true;
    options->latex_formula_delimiters = true;
    options->directives = true;
}

static bool attach_extension(markdown_core_parser *parser, const char *name) {
    markdown_core_syntax_extension *extension = markdown_core_find_syntax_extension(name);
    return extension && markdown_core_parser_attach_syntax_extension(parser, extension) != 0;
}

markdown_core_document *markdown_core_document_parse(const uint8_t *source, size_t length,
                                                     const markdown_core_parse_options *requested_options,
                                                     markdown_core_error **error) {
    markdown_core_parse_options defaults;
    const markdown_core_parse_options *options = requested_options;
    markdown_core_document *document;
    markdown_core_parser *parser;
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
    if (options->smart_punctuation)
        native_options |= MARKDOWN_CORE_OPT_SMART;
    if (options->footnotes)
        native_options |= MARKDOWN_CORE_OPT_FOOTNOTES;
    if (options->strip_html_comments)
        native_options |= MARKDOWN_CORE_OPT_STRIP_HTML_COMMENTS;
    if (options->formulas && options->dollar_formula_delimiters)
        native_options |= MARKDOWN_CORE_OPT_DOLLAR_FORMULA_DELIMITERS;
    if (options->formulas && options->latex_formula_delimiters)
        native_options |= MARKDOWN_CORE_OPT_LATEX_FORMULA_DELIMITERS;
    if (options->directives)
        native_options |= MARKDOWN_CORE_OPT_DIRECTIVE;

    markdown_core_core_extensions_ensure_registered();
    parser = markdown_core_parser_new(native_options);
    if (!parser) {
        set_error(error, MARKDOWN_CORE_ERROR_ALLOCATION_FAILED, "could not allocate parser");
        return NULL;
    }

#define ATTACH_IF(enabled, name)                                                                                       \
    do {                                                                                                               \
        if ((enabled) && !attach_extension(parser, (name))) {                                                          \
            markdown_core_parser_free(parser);                                                                         \
            set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "required syntax extension is unavailable");                \
            return NULL;                                                                                               \
        }                                                                                                              \
    } while (0)
    ATTACH_IF(options->tables, "table");
    ATTACH_IF(options->strikethrough, "strikethrough");
    ATTACH_IF(options->autolinks, "autolink");
    ATTACH_IF(options->task_lists, "tasklist");
    ATTACH_IF(options->formulas, "formula");
    ATTACH_IF(options->directives, "directive");
#undef ATTACH_IF

    if (length)
        markdown_core_parser_feed(parser, (const char *)source, length);
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
        set_error(error, MARKDOWN_CORE_ERROR_INTERNAL, "parser did not produce a document");
        return NULL;
    }
    return document;
}

void markdown_core_document_free(markdown_core_document *document) {
    if (!document)
        return;
    markdown_core_node_free(document->root);
    free(document);
}

const markdown_core_node *markdown_core_document_root(const markdown_core_document *document) {
    return document ? document->root : NULL;
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
    if (!error || !error->has_scope || !scope)
        return false;
    *scope = error->scope;
    return true;
}

void markdown_core_error_free(markdown_core_error *error) {
    if (!error)
        return;
    free(error->message);
    free(error);
}

markdown_core_node_kind markdown_core_node_get_kind(const markdown_core_node *node) {
    if (!node)
        return MARKDOWN_CORE_KIND_NONE;
    if (node->type == MARKDOWN_CORE_NODE_DOCUMENT)
        return MARKDOWN_CORE_KIND_DOCUMENT;
    if (node->type == MARKDOWN_CORE_NODE_BLOCK_QUOTE)
        return MARKDOWN_CORE_KIND_BLOCK_QUOTE;
    if (node->type == MARKDOWN_CORE_NODE_PARAGRAPH)
        return MARKDOWN_CORE_KIND_PARAGRAPH;
    if (node->type == MARKDOWN_CORE_NODE_HEADING)
        return MARKDOWN_CORE_KIND_HEADING;
    if (node->type == MARKDOWN_CORE_NODE_THEMATIC_BREAK)
        return MARKDOWN_CORE_KIND_THEMATIC_BREAK;
    if (node->type == MARKDOWN_CORE_NODE_LIST)
        return MARKDOWN_CORE_KIND_LIST;
    if (node->type == MARKDOWN_CORE_NODE_LIST_ITEM)
        return MARKDOWN_CORE_KIND_LIST_ITEM;
    if (node->type == MARKDOWN_CORE_NODE_CODE_BLOCK)
        return MARKDOWN_CORE_KIND_CODE_BLOCK;
    if (node->type == MARKDOWN_CORE_NODE_HTML_BLOCK)
        return MARKDOWN_CORE_KIND_HTML_BLOCK;
    if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION)
        return MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION;
    if (node->type == MARKDOWN_CORE_NODE_TEXT)
        return MARKDOWN_CORE_KIND_TEXT;
    if (node->type == MARKDOWN_CORE_NODE_SOFT_BREAK)
        return MARKDOWN_CORE_KIND_SOFT_BREAK;
    if (node->type == MARKDOWN_CORE_NODE_LINE_BREAK)
        return MARKDOWN_CORE_KIND_LINE_BREAK;
    if (node->type == MARKDOWN_CORE_NODE_CODE)
        return MARKDOWN_CORE_KIND_CODE;
    if (node->type == MARKDOWN_CORE_NODE_HTML)
        return MARKDOWN_CORE_KIND_HTML;
    if (node->type == MARKDOWN_CORE_NODE_EMPHASIS)
        return MARKDOWN_CORE_KIND_EMPHASIS;
    if (node->type == MARKDOWN_CORE_NODE_STRONG)
        return MARKDOWN_CORE_KIND_STRONG;
    if (node->type == MARKDOWN_CORE_NODE_LINK)
        return MARKDOWN_CORE_KIND_LINK;
    if (node->type == MARKDOWN_CORE_NODE_IMAGE)
        return MARKDOWN_CORE_KIND_IMAGE;
    if (node->type == MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE)
        return MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE;
    if (node->type == MARKDOWN_CORE_NODE_TABLE)
        return MARKDOWN_CORE_KIND_TABLE;
    if (node->type == MARKDOWN_CORE_NODE_TABLE_ROW)
        return MARKDOWN_CORE_KIND_TABLE_ROW;
    if (node->type == MARKDOWN_CORE_NODE_TABLE_CELL)
        return MARKDOWN_CORE_KIND_TABLE_CELL;
    if (node->type == MARKDOWN_CORE_NODE_STRIKETHROUGH)
        return MARKDOWN_CORE_KIND_STRIKETHROUGH;
    if (node->type == MARKDOWN_CORE_NODE_FORMULA)
        return MARKDOWN_CORE_KIND_FORMULA;
    if (node->type == MARKDOWN_CORE_NODE_FORMULA_BLOCK)
        return MARKDOWN_CORE_KIND_FORMULA_BLOCK;
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE)
        return MARKDOWN_CORE_KIND_DIRECTIVE;
    if (node->type == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)
        return MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK;
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
                                        "TableCell"};
    if (kind < MARKDOWN_CORE_KIND_NONE || kind > MARKDOWN_CORE_KIND_TABLE_CELL)
        return "None";
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

static bool is_label(const markdown_core_node *node) {
    return node && node->type == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

const markdown_core_node *markdown_core_node_get_first_child(const markdown_core_node *node) {
    const markdown_core_node *child;
    if (!node)
        return NULL;
    child = node->first_child;
    if (is_label(child))
        return child->first_child ? child->first_child : child->next;
    return child;
}

const markdown_core_node *markdown_core_node_get_next_sibling(const markdown_core_node *node) {
    const markdown_core_node *next;
    if (!node)
        return NULL;
    next = node->next;
    if (next && is_label(next))
        return next->first_child ? next->first_child : next->next;
    if (!next && is_label(node->parent))
        return node->parent->next;
    return next;
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
    if (!node || node->type != MARKDOWN_CORE_NODE_HEADING || !level)
        return false;
    *level = node->as.heading.level;
    return true;
}

bool markdown_core_node_list_properties(const markdown_core_node *node, markdown_core_list_flavor *flavor,
                                        markdown_core_optional_i64 *start, bool *tight) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST || !flavor || !start || !tight)
        return false;
    *flavor = node->as.list.list_type == MARKDOWN_CORE_ORDERED_LIST ? MARKDOWN_CORE_LIST_FLAVOR_ORDERED
                                                                    : MARKDOWN_CORE_LIST_FLAVOR_BULLET;
    start->has_value = *flavor == MARKDOWN_CORE_LIST_FLAVOR_ORDERED;
    start->value = node->as.list.start;
    *tight = node->as.list.tight;
    return true;
}

bool markdown_core_node_list_item_checked(const markdown_core_node *node, markdown_core_optional_bool *checked) {
    if (!node || node->type != MARKDOWN_CORE_NODE_LIST_ITEM || !checked)
        return false;
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
    if (!node || node->type != MARKDOWN_CORE_NODE_CODE_BLOCK || !info || !language || !literal || !fenced || !closed)
        return false;
    view_chunk(info, &node->as.code.info);
    view_chunk(literal, &node->as.code.literal);
    if (info->length == 0)
        info->data = NULL;
    language->data = NULL;
    language->length = 0;
    while (start < info->length && (info->data[start] == ' ' || info->data[start] == '\t' ||
                                    info->data[start] == '\n' || info->data[start] == '\r'))
        start++;
    end = start;
    while (end < info->length && info->data[end] != ' ' && info->data[end] != '\t' && info->data[end] != '\n' &&
           info->data[end] != '\r')
        end++;
    if (end > start) {
        language->data = info->data + start;
        language->length = end - start;
    }
    *fenced = node->as.code.fenced != 0;
    *closed = !*fenced || node->as.code.fence_closed != 0;
    return true;
}

bool markdown_core_node_literal(const markdown_core_node *node, markdown_core_string_view *literal) {
    if (!node || !literal)
        return false;
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
        (node->type != MARKDOWN_CORE_NODE_FORMULA && node->type != MARKDOWN_CORE_NODE_FORMULA_BLOCK))
        return false;
    native_mode = markdown_core_extensions_get_formula_mode((markdown_core_node *)node);
    *mode = native_mode == MARKDOWN_CORE_FORMULA_MODE_EMBEDDED ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                               : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    value = markdown_core_extensions_get_formula_literal((markdown_core_node *)node);
    literal->data = (const uint8_t *)value;
    literal->length = value ? strlen(value) : 0;
    return true;
}

bool markdown_core_node_table_column_count(const markdown_core_node *node, size_t *count) {
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !count)
        return false;
    *count = markdown_core_extensions_get_table_columns((markdown_core_node *)node);
    return true;
}

bool markdown_core_node_table_alignment_at(const markdown_core_node *node, size_t index,
                                           markdown_core_table_alignment *alignment) {
    uint16_t count;
    uint8_t *alignments;
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE || !alignment)
        return false;
    count = markdown_core_extensions_get_table_columns((markdown_core_node *)node);
    if (index >= count)
        return false;
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
    if (!node || node->type != MARKDOWN_CORE_NODE_TABLE_ROW || !is_header)
        return false;
    *is_header = markdown_core_extensions_get_table_row_is_header((markdown_core_node *)node) != 0;
    return true;
}

static const markdown_core_node *directive_label_node(const markdown_core_node *node) {
    if (!node || (node->type != MARKDOWN_CORE_NODE_DIRECTIVE && node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK))
        return NULL;
    return is_label(node->first_child) ? node->first_child : NULL;
}

bool markdown_core_node_directive_properties(const markdown_core_node *node, markdown_core_placement_mode *mode,
                                             markdown_core_string_view *name, markdown_core_string_view *attributes,
                                             bool *has_label, size_t *label_count) {
    const char *value;
    const markdown_core_node *label;
    const markdown_core_node *child;
    if (!node || !mode || !name || !attributes || !has_label || !label_count ||
        (node->type != MARKDOWN_CORE_NODE_DIRECTIVE && node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK))
        return false;
    *mode = node->type == MARKDOWN_CORE_NODE_DIRECTIVE ? MARKDOWN_CORE_PLACEMENT_EMBEDDED
                                                       : MARKDOWN_CORE_PLACEMENT_STANDALONE;
    value = markdown_core_extensions_get_directive_name((markdown_core_node *)node);
    name->data = (const uint8_t *)value;
    name->length = value ? strlen(value) : 0;
    value = markdown_core_extensions_get_directive_attributes((markdown_core_node *)node);
    attributes->data = (const uint8_t *)value;
    attributes->length = value ? strlen(value) : 0;
    *has_label = markdown_core_directive_has_label((markdown_core_node *)node) != 0;
    *label_count = 0;
    label = directive_label_node(node);
    child = label ? label->first_child : NULL;
    while (child) {
        (*label_count)++;
        child = child->next;
    }
    return true;
}

const markdown_core_node *markdown_core_node_directive_first_label_child(const markdown_core_node *node) {
    const markdown_core_node *label = directive_label_node(node);
    return label ? label->first_child : NULL;
}

const markdown_core_node *markdown_core_node_directive_first_content_child(const markdown_core_node *node) {
    const markdown_core_node *label;
    if (!node || node->type != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK)
        return NULL;
    label = directive_label_node(node);
    return label ? label->next : node->first_child;
}

static bool link_properties(const markdown_core_node *node, uint16_t expected, markdown_core_string_view *url,
                            markdown_core_string_view *title) {
    if (!node || node->type != expected || !url || !title)
        return false;
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

bool markdown_core_node_footnote_id(const markdown_core_node *node, markdown_core_string_view *id) {
    const markdown_core_node *definition;
    if (!node || !id ||
        (node->type != MARKDOWN_CORE_NODE_FOOTNOTE_DEFINITION && node->type != MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE))
        return false;
    definition = node->type == MARKDOWN_CORE_NODE_FOOTNOTE_REFERENCE ? node->parent_footnote_def : node;
    if (!definition) {
        id->data = NULL;
        id->length = 0;
        return false;
    }
    view_chunk(id, &definition->as.literal);
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
    if (needed <= buffer->capacity)
        return;
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
    if (buffer->failed)
        return;
    if (length)
        memcpy(buffer->data + buffer->size, bytes, length);
    buffer->size += length;
    buffer->data[buffer->size] = 0;
}

static void buffer_cstr(dump_buffer *buffer, const char *value) { buffer_bytes(buffer, value, strlen(value)); }

static void buffer_i64(dump_buffer *buffer, int64_t value) {
    char text[32];
    int length = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (length > 0)
        buffer_bytes(buffer, text, (size_t)length);
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
    if (!value.data)
        buffer_cstr(buffer, "null");
    else
        buffer_json_string(buffer, value);
}

static bool ensure_more(dump_buffer *buffer, size_t depth) {
    bool *more;
    size_t capacity;
    if (depth < buffer->more_capacity)
        return true;
    capacity = buffer->more_capacity ? buffer->more_capacity : 16;
    while (capacity <= depth)
        capacity *= 2;
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

static void dump_fields(dump_buffer *buffer, const markdown_core_node *node, markdown_core_node_kind kind) {
    markdown_core_string_view a = {NULL, 0}, b = {NULL, 0}, c = {NULL, 0};
    markdown_core_optional_i64 start;
    markdown_core_optional_bool checked;
    markdown_core_list_flavor flavor;
    markdown_core_placement_mode mode;
    bool x, y, has_label;
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
        if (start.has_value)
            buffer_i64(buffer, start.value);
        else
            buffer_cstr(buffer, "null");
        buffer_cstr(buffer, " tight=");
        buffer_cstr(buffer, x ? "true" : "false");
        break;
    case MARKDOWN_CORE_KIND_LIST_ITEM:
        markdown_core_node_list_item_checked(node, &checked);
        buffer_cstr(buffer, " checked=");
        if (checked.has_value)
            buffer_cstr(buffer, checked.value ? "true" : "false");
        else
            buffer_cstr(buffer, "null");
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
            if (i)
                buffer_cstr(buffer, ",");
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
        markdown_core_node_directive_properties(node, &mode, &a, &b, &has_label, &count);
        buffer_cstr(buffer, " mode=");
        buffer_cstr(buffer, mode_name(mode));
        buffer_cstr(buffer, " name=");
        buffer_json_string(buffer, a);
        buffer_cstr(buffer, " attributes=");
        buffer_optional_string(buffer, b);
        buffer_cstr(buffer, " label=");
        if (has_label)
            buffer_i64(buffer, (int64_t)count);
        else
            buffer_cstr(buffer, "null");
        break;
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION:
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE:
        markdown_core_node_footnote_id(node, &a);
        buffer_cstr(buffer, " id=");
        buffer_json_string(buffer, a);
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
        for (i = 0; i + 1 < depth; i++)
            buffer_cstr(buffer, buffer->more[i] ? "│   " : "    ");
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
        if (!ensure_more(buffer, depth))
            return;
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
