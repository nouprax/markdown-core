#include "directive.h"
#include "extension.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <markdown-core.h>
#include <inlines.h>
#include <node.h>
#include <parser.h>
#include <utf8.h>

#include "ext_scanners.h"

typedef struct {
    markdown_core_chunk name;
    markdown_core_node *label;
    int fence_length;
    int closed;
    int consume_line;
} node_directive;

typedef struct {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t label_start;
    bufsize_t label_len;
    int has_label;
    markdown_core_attributes attributes;
    bufsize_t end;
    /* Set when attribute parsing failed from allocation loss rather than
     * invalid syntax; the caller flags the parser instead of silently
     * treating the directive as plain text. */
    int oom;
} parsed_directive;

static int is_directive_node(markdown_core_node *node) {
    return node && (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE || node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK);
}

static node_directive *get_directive(markdown_core_node *node) {
    if (!is_directive_node(node)) {
        return NULL;
    }

    return (node_directive *)node->opaque;
}

static int ascii_is_line_space(unsigned char c) { return c == ' ' || c == '\t'; }

static int is_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    return pos >= len || data[pos] == '\n' || data[pos] == '\r';
}

static int has_only_spaces_until_line_end(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    while (pos < len && ascii_is_line_space(data[pos])) {
        pos++;
    }

    return is_line_end(data, len, pos);
}

/* Names use the dialect's Unicode letter/number/mark categories. */
static int scan_name(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *name_start,
                     bufsize_t *name_len) {
    bufsize_t start = pos;
    int32_t cp;
    if (pos >= len) {
        return 0;
    }
    int width = markdown_core_utf8proc_iterate(data + pos, len - pos, &cp);
    if (!markdown_core_utf8proc_is_letter(cp)) {
        return 0;
    }
    pos += width;
    while (pos < len) {
        width = markdown_core_utf8proc_iterate(data + pos, len - pos, &cp);
        if (!(markdown_core_utf8proc_is_letter(cp) || markdown_core_utf8proc_is_number(cp) ||
              markdown_core_utf8proc_is_mark(cp) || cp == '-' || cp == '_')) {
            break;
        }
        pos += width;
    }
    if (data[pos - 1] == '-' || data[pos - 1] == '_') {
        return 0;
    }
    *name_start = start;
    *name_len = pos - start;
    return 1;
}

static int scan_label(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *label_start,
                      bufsize_t *label_len, bufsize_t *end) {
    int depth = 1;
    bufsize_t i;

    if (pos >= len || data[pos] != '[') {
        return 0;
    }

    i = pos + 1;
    while (i < len) {
        if (data[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }

        /* micromark's factory-label.js counts the INNER brackets and fails at
         * the 33rd (`if (code === 91 && ++balance > 32)`), so 32 nested labels
         * are a label and 33 are prose. `depth` here starts at 1 for the
         * label's own `[`, which is the off-by-one this bound accounts for. */
        if (data[i] == '[') {
            if (++depth > 33) {
                return 0;
            }
            i++;
            continue;
        }

        if (data[i] == ']') {
            depth--;
            if (depth == 0) {
                *label_start = pos + 1;
                *label_len = i - (pos + 1);
                *end = i + 1;
                return 1;
            }
        }

        i++;
    }

    return 0;
}

static int set_chunk_bytes(markdown_core_mem *mem, markdown_core_chunk *chunk, const unsigned char *data,
                           bufsize_t len) {
    markdown_core_chunk_free(mem, chunk);
    chunk->data = (unsigned char *)data;
    chunk->len = len;
    chunk->alloc = 0;
    if (!markdown_core_chunk_to_cstr(mem, chunk)) {
        /* Never keep borrowing the transient line buffer. */
        chunk->data = NULL;
        chunk->len = 0;
        return 0;
    }
    return 1;
}

const char *markdown_core_extensions_get_directive_name(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    if (!directive) {
        return NULL;
    }

    return markdown_core_chunk_to_cstr(markdown_core_node_mem(node), &directive->name);
}

int markdown_core_directive_has_label(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    return directive && directive->label;
}

markdown_core_node *markdown_core_directive_label(markdown_core_node *node) {
    node_directive *directive = get_directive(node);
    return directive ? directive->label : NULL;
}

static int directive_name_is_valid(markdown_core_mem *mem, const char *name) {
    size_t raw_len;
    unsigned char *copy;
    bufsize_t len;
    bufsize_t name_start;
    bufsize_t name_len;
    int valid;

    if (!name) {
        return 0;
    }

    raw_len = strlen(name);
    if (raw_len == 0 || raw_len > INT_MAX) {
        return 0;
    }

    len = (bufsize_t)raw_len;
    copy = (unsigned char *)mem->calloc((size_t)len + 1, 1);
    if (!copy) {
        return 0;
    }

    memcpy(copy, name, (size_t)len);
    valid = scan_name(copy, len, 0, &name_start, &name_len) && name_start == 0 && name_len == len;
    mem->free(copy);
    return valid;
}

int markdown_core_extensions_set_directive_name(markdown_core_node *node, const char *name) {
    node_directive *directive = get_directive(node);

    if (!directive || !directive_name_is_valid(markdown_core_node_mem(node), name)) {
        return 0;
    }

    if (!markdown_core_chunk_set_cstr(markdown_core_node_mem(node), &directive->name, name)) {
        return 0;
    }
    return 1;
}

static void directive_opaque_alloc(const markdown_core_extension *extension, markdown_core_mem *mem,
                                   markdown_core_node *node) {
    if (is_directive_node(node)) {
        node->opaque = mem->calloc(1, sizeof(node_directive));
    }
}

static void directive_opaque_free(const markdown_core_extension *extension, markdown_core_mem *mem,
                                  markdown_core_node *node) {
    node_directive *directive = (node_directive *)node->opaque;
    if (!directive) {
        return;
    }

    if (directive->label) {
        markdown_core_node_free(directive->label);
    }
    markdown_core_chunk_free(mem, &directive->name);
    mem->free(directive);
    node->opaque = NULL;
}

static void free_parsed_directive(markdown_core_mem *mem, parsed_directive *parsed) {
    markdown_core_attributes_free(mem, &parsed->attributes);
}

static int parse_directive_suffix(markdown_core_mem *mem, unsigned char *data, bufsize_t len, bufsize_t pos,
                                  parsed_directive *parsed) {
    memset(parsed, 0, sizeof(*parsed));

    if (!scan_name(data, len, pos, &parsed->name_start, &parsed->name_len)) {
        return 0;
    }

    pos = parsed->name_start + parsed->name_len;

    if (pos < len && data[pos] == '[') {
        parsed->has_label = 1;
        if (!scan_label(data, len, pos, &parsed->label_start, &parsed->label_len, &pos)) {
            return 0;
        }
    }

    if (pos < len && data[pos] == '{') {
        markdown_core_attribute_parser attributes = {0};
        attributes.mem = mem;
        attributes.data = data;
        attributes.length = len;
        int matched = markdown_core_attributes_parse(&attributes, pos, &parsed->attributes, &pos);
        parsed->oom = attributes.oom;
        markdown_core_attribute_parser_free(&attributes);
        if (!matched) {
            return 0;
        }
    }

    parsed->end = pos;
    return 1;
}

static markdown_core_node *make_label_node(const markdown_core_extension *extension, markdown_core_mem *mem,
                                           const unsigned char *label, bufsize_t label_len, int start_line,
                                           int start_column, int end_column) {
    markdown_core_node *label_node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, mem, extension);
    if (!label_node) {
        return NULL;
    }

    markdown_core_strbuf_put(&label_node->content, label, label_len);
    if (label_node->content.oom) {
        markdown_core_node_free(label_node);
        return NULL;
    }
    label_node->start_line = label_node->end_line = start_line;
    label_node->start_column = start_column;
    label_node->end_column = end_column;
    /* The scope starts ON the `[` and the content starts after it. This is
     * what `internal_offset` is for -- a heading's `#` and a table cell's
     * leading pipe use the same field -- and it is why making the scope
     * bracket-inclusive did not move the label's own children. */
    label_node->internal_offset = 1;
    return label_node;
}

static int attach_label_node(const markdown_core_extension *extension, markdown_core_node *directive_node,
                             const unsigned char *label, bufsize_t label_len, int start_line, int start_column,
                             int end_column) {
    markdown_core_node *label_node;

    label_node = make_label_node(extension, markdown_core_node_mem(directive_node), label, label_len, start_line,
                                 start_column, end_column);
    if (!label_node) {
        return 0;
    }

    node_directive *directive = get_directive(directive_node);
    if (!directive || directive->label) {
        markdown_core_node_free(label_node);
        return 0;
    }

    directive->label = label_node;

    return 1;
}

static int apply_parsed_directive(const markdown_core_extension *extension, markdown_core_node *node,
                                  const unsigned char *data, parsed_directive *parsed, int start_line,
                                  int start_column) {
    node_directive *directive = get_directive(node);
    markdown_core_mem *mem = markdown_core_node_mem(node);

    if (!directive) {
        return 0;
    }

    if (!set_chunk_bytes(mem, &directive->name, data + parsed->name_start, parsed->name_len)) {
        return 0;
    }
    node->attributes = parsed->attributes;
    memset(&parsed->attributes, 0, sizeof(parsed->attributes));

    if (parsed->has_label) {
        /* THE LABEL'S SCOPE SPANS ITS BRACKETS. It used to span the content
         * only, which made `[]` a NEGATIVE range -- end one column before
         * start -- because there was no content to point at. A label always
         * has its two brackets, so the bracket-inclusive range is a place for
         * every label there is, and `:red[]:` reads `1:5..1:6`. */
        int label_start_column = start_column + (int)parsed->label_start;
        int label_end_column = label_start_column + (int)parsed->label_len + 1;

        if (!attach_label_node(extension, node, data + parsed->label_start, parsed->label_len, start_line,
                               label_start_column, label_end_column)) {
            return 0;
        }
    }

    return 1;
}

static markdown_core_node *make_directive_node(const markdown_core_extension *extension, markdown_core_parser *parser,
                                               const unsigned char *name, bufsize_t name_len, int start_line,
                                               int start_column, int end_line, int end_column) {
    markdown_core_node *node =
        markdown_core_node_new_with_mem_and_ext(MARKDOWN_CORE_NODE_DIRECTIVE, parser->mem, extension);
    node_directive *directive;

    if (!node) {
        parser->oom = true;
        return NULL;
    }

    directive = get_directive(node);
    if (!directive) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    if (!set_chunk_bytes(parser->mem, &directive->name, name, name_len)) {
        parser->oom = true;
        markdown_core_node_free(node);
        return NULL;
    }
    node->start_line = start_line;
    node->end_line = end_line;
    node->start_column = start_column;
    node->end_column = end_column;
    return node;
}

/* THE WHOLE CONSTRUCT IS SCANNED AT THE COLON, and that is the difference
 * between this and what was here before.
 *
 * `:name[label]{attrs}` used to be started by PUSHING A DELIMITER for `:name[`
 * and betting that a `]` would turn up later to pair with it. Two things
 * follow from that bet and both are wrong. When no `]` arrives the directive
 * is lost -- `:a[b` was one Text node where micromark gives a directive named
 * `a` and the prose `[b`. And while the bet is open the label has no boundary,
 * so whatever reaches the `]` first takes the directive with it: a GFM
 * autolink literal, an emphasis run, a code span (D36).
 *
 * micromark decides at the colon instead, with
 * `effects.attempt(label, afterLabel, afterLabel)` -- it SCANS the label, and
 * BOTH branches continue. A label that closes is a label; one that does not is
 * prose, and the directive stands either way. That is the same shape 7.1 gave
 * the attribute block, and it is what this does.
 *
 * Scanning it here also means the bytes are CONSUMED here, so no other
 * extension is ever offered them. There is nothing left to protect. */
static markdown_core_node *match_colon_directive(const markdown_core_extension *extension, markdown_core_parser *parser,
                                                 markdown_core_node *parent, markdown_core_inline_parser *inline_parser,
                                                 markdown_core_chunk *chunk, bufsize_t offset) {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t pos;
    bufsize_t label_start = 0;
    bufsize_t label_len = 0;
    bufsize_t label_open = 0;
    int has_label = 0;
    markdown_core_attributes attributes;
    markdown_core_node *node;
    markdown_core_node *label_node = NULL;
    node_directive *directive;
    int start_line = markdown_core_inline_parser_get_line(inline_parser);
    int start_column = markdown_core_inline_parser_get_column(inline_parser);

    (void)parent;
    memset(&attributes, 0, sizeof(attributes));

    /* A TEXT DIRECTIVE'S COLON MAY NOT SIT NEXT TO ANOTHER COLON, on either
     * side. The trailing half keeps `:red:` available to emoji; the leading
     * half keeps a run of colons whole, so `x ::a y` is text rather than
     * `x :` plus a directive named `a`, and `x:::a` is text rather than `x::`
     * plus one. `::name` and `:::name` at the start of a line are leaf and
     * container directives and open through the block path, not this one. */
    if (offset > 0 && chunk->data[offset - 1] == ':') {
        return NULL;
    }

    if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':') {
        return NULL;
    }

    if (!scan_name(chunk->data, chunk->len, offset + 1, &name_start, &name_len)) {
        return NULL;
    }

    pos = name_start + name_len;
    if (pos < chunk->len && chunk->data[pos] == ':') {
        return NULL;
    }

    if (pos < chunk->len && chunk->data[pos] == '[') {
        bufsize_t label_end;
        if (scan_label(chunk->data, chunk->len, pos, &label_start, &label_len, &label_end)) {
            has_label = 1;
            label_open = pos;
            pos = label_end;
        }
    }

    if (pos < chunk->len && chunk->data[pos] == '{') {
        markdown_core_inline_parser_attributes(inline_parser, pos, &attributes, &pos);
    }

    node = make_directive_node(extension, parser, chunk->data + name_start, name_len, start_line, start_column,
                               start_line, start_column);
    if (!node) {
        markdown_core_attributes_free(parser->mem, &attributes);
        parser->oom = true;
        return NULL;
    }
    directive = get_directive(node);
    node->attributes = attributes;

    if (has_label) {
        /* Consume to the `]` first and read the label's end back from the
         * subject, because a label may span a line ending and a column
         * computed from the start plus a length states it in the wrong line's
         * frame -- 0a.10's rule, and the reason D22 was a defect. */
        int label_line = start_line;
        int label_column = start_column + (int)(label_open - offset);
        markdown_core_inline_parser_set_offset(inline_parser, (int)(label_start + label_len + 1));
        label_node = make_label_node(extension, parser->mem, chunk->data + label_start, label_len, label_line,
                                     label_column, markdown_core_inline_parser_get_column(inline_parser) - 1);
        if (!label_node) {
            markdown_core_node_free(node);
            parser->oom = true;
            return NULL;
        }
        label_node->end_line = markdown_core_inline_parser_get_line(inline_parser);
        if (directive->label) {
            markdown_core_node_free(label_node);
            markdown_core_node_free(node);
            parser->oom = true;
            return NULL;
        }
        directive->label = label_node;
    }

    markdown_core_inline_parser_set_offset(inline_parser, (int)pos);
    node->end_line = markdown_core_inline_parser_get_line(inline_parser);
    node->end_column = markdown_core_inline_parser_get_column(inline_parser) - 1;

    /* The label brackets belong to the enclosing paragraph's claim run, not
     * to the detached label content buffer. Parser phases discover the field
     * from its live owner after this paragraph finishes. */
    if (label_node) {
        /* The label's scope spans its brackets, so the brackets are the
         * label's markers (requirement 11b). They are claimed from HERE, in
         * the enclosing paragraph's claim run, because they are not part of
         * the label's own content buffer -- the label was made from what is
         * between them. */
    }
    return node;
}

/* ONE BYTE, not two. `]` was claimed because a label's closer had to be
 * recognised as a delimiter to pair with the opener; the label is scanned at
 * the colon now, so the bracket is nobody's business but the core's -- which
 * is what makes `[a](b)` inside a label work like any other link. */
static markdown_core_node *match(const markdown_core_extension *extension, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_parser *inline_parser) {
    markdown_core_chunk *chunk = markdown_core_inline_parser_get_chunk(inline_parser);
    bufsize_t offset = (bufsize_t)markdown_core_inline_parser_get_offset(inline_parser);

    if (character == ':') {
        return match_colon_directive(extension, parser, parent, inline_parser, chunk, offset);
    }

    return NULL;
}

static bufsize_t count_colons(const unsigned char *data, bufsize_t len, bufsize_t pos) {
    bufsize_t count = 0;
    while (pos + count < len && data[pos + count] == ':') {
        count++;
    }
    return count;
}

static markdown_core_node *open_directive_block(const markdown_core_extension *extension, int indented,
                                                markdown_core_parser *parser, markdown_core_node *parent_container,
                                                unsigned char *input, int len) {
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count;
    parsed_directive parsed;
    markdown_core_node *node;
    node_directive *directive;

    if (indented) {
        return NULL;
    }

    colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
    if (colon_count < 2) {
        return NULL;
    }

    if (!parse_directive_suffix(parser->mem, input, (bufsize_t)len, first_nonspace + colon_count, &parsed)) {
        if (parsed.oom) {
            parser->oom = true;
        }
        return NULL;
    }

    if (!has_only_spaces_until_line_end(input, (bufsize_t)len, parsed.end)) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    node = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
                                          (int)first_nonspace + 1);
    if (!node) {
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    markdown_core_node_set_extension(node, extension);
    node->opaque = parser->mem->calloc(1, sizeof(node_directive));
    if (!node->opaque) {
        parser->oom = true;
        markdown_core_node_free(node);
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    if (!apply_parsed_directive(extension, node, input, &parsed, markdown_core_parser_get_line_number(parser),
                                (int)first_nonspace)) {
        /* The suffix already validated; failure here is allocation loss. */
        parser->oom = true;
        markdown_core_node_free(node);
        free_parsed_directive(parser->mem, &parsed);
        return NULL;
    }

    directive = get_directive(node);
    directive->fence_length = (int)colon_count;
    directive->closed = (colon_count == 2);
    directive->consume_line = 1;

    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);

    free_parsed_directive(parser->mem, &parsed);
    return node;
}

static int directive_block_matches(const markdown_core_extension *extension, markdown_core_parser *parser,
                                   unsigned char *input, int len, markdown_core_node *container) {
    node_directive *directive = get_directive(container);
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count;

    if (!directive) {
        return 0;
    }

    if (directive->closed) {
        return 0;
    }

    directive->consume_line = 0;

    colon_count = count_colons(input, (bufsize_t)len, first_nonspace);
    if (markdown_core_parser_get_indent(parser) <= 3 && colon_count >= (bufsize_t)directive->fence_length &&
        has_only_spaces_until_line_end(input, (bufsize_t)len, first_nonspace + colon_count)) {
        directive->closed = 1;
        directive->consume_line = 1;
        markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser),
                                            false);
        /* Returning 1 here used to leave the container open. The fence was
         * consumed, so nothing else on the line was parsed, but the block
         * stayed open and the next non-blank line arrived as a lazy paragraph
         * continuation -- pulled inside the container and recorded on the
         * fence's line rather than its own. */
        return MARKDOWN_CORE_BLOCK_CLOSED;
    }

    return 1;
}

static const char *get_type_string(const markdown_core_extension *extension, markdown_core_node *node) {
    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE) {
        return "directive";
    }

    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return "directive_block";
    }

    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return "directive_label";
    }

    return "<unknown>";
}

static int can_contain(const markdown_core_extension *extension, markdown_core_node *node,
                       markdown_core_node_type child_type) {
    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE) {
        return 0;
    }

    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return MARKDOWN_CORE_NODE_TYPE_BLOCK_P(child_type) && child_type != MARKDOWN_CORE_NODE_LIST_ITEM &&
               child_type != MARKDOWN_CORE_NODE_DOCUMENT;
    }

    if (node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL) {
        return MARKDOWN_CORE_NODE_TYPE_INLINE_P(child_type) && child_type != MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
    }

    return 0;
}

static int contains_inlines(const markdown_core_extension *extension, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

static int accepts_lines(const markdown_core_extension *extension, markdown_core_node *node) {
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (node->kind != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return 0;
    }

    return directive->fence_length == 2 || directive->consume_line;
}

static int visit_owned_subtrees(const markdown_core_extension *extension, markdown_core_node *node,
                                markdown_core_owned_subtree_visitor visitor, void *context) {
    node_directive *directive = get_directive(node);
    (void)extension;
    if (!directive || !directive->label) {
        return 1;
    }
    return visitor(&directive->label, context);
}

/* `:` opens a directive. `]` is in the dispatch set for the `]` arbitration
 * `bracket_takes_close_bracket` performs, not because it terminates a text run --
 * `is_core_special_character` refuses it there. */
const markdown_core_extension MARKDOWN_CORE_EXTENSION_DIRECTIVE = {
    .name = "directive",
    .match_inline = match,
    .last_block_matches = directive_block_matches,
    .try_opening_block = open_directive_block,
    .get_type_string_func = get_type_string,
    .can_contain_func = can_contain,
    .contains_inlines_func = contains_inlines,
    .accepts_lines_func = accepts_lines,
    .opaque_alloc_func = directive_opaque_alloc,
    .opaque_free_func = directive_opaque_free,
    .visit_owned_subtrees_func = visit_owned_subtrees,
    .terminates_text = ":",
    .dispatch = ":",
};
