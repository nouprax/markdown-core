#include "alloc.h"
#include "attributes.h"
#include "directive.h"
#include "element.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <buffer.h>
#include <chunk.h>
#include <inlines.h>
#include <node.h>
#include <parser.h>

typedef markdown_core_directive_value node_directive;

typedef struct {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t label_start;
    bufsize_t label_len;
    int has_label;
    /* Borrowed authored attributes: a braced envelope or one class word. */
    bufsize_t attributes_start;
    bufsize_t attributes_len;
    /* One immutable source extent and its recognition memo survive until the
     * committed owner decodes the value, or a declined probe releases it. */
    markdown_core_attribute_parser attributes;
    bufsize_t end;
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

/* A NAME IS A STRING WITHOUT SPACES, as the generic-directive proposal puts
 * it. Any byte that is not whitespace (a space, tab or line ending)
 * continues it, up to the `[` or `{` that opens the label or the attributes
 * and the `:` that would make a block fence's colon count ambiguous. Nothing
 * is classified by Unicode category: `:中文[中文]` names `中文`, `:1a[x]`
 * names `1a`, and a name is found by one table test per byte. What makes
 * `12:30` text is not the `3` but the absence of a bracket part, which is
 * where the inline form is anchored (see match_colon_directive). */
enum { NAME_END = 1 };
static const uint8_t NAME_BYTES[256] = {MARKDOWN_CORE_WHITESPACE_BYTES(NAME_END), ['['] = NAME_END, ['{'] = NAME_END,
                                        [':'] = NAME_END};

static int scan_name(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *name_start,
                     bufsize_t *name_len) {
    bufsize_t start = pos;
    pos = markdown_core_scan_to_class(NAME_BYTES, NAME_END, data, pos, len);
    if (pos == start) {
        return 0;
    }
    *name_start = start;
    *name_len = pos - start;
    return 1;
}

/* The only bytes that decide where a label ends: the brackets, and the
 * backslash that escapes the byte after it. Every other byte, of any script,
 * is skipped by the class scan. */
enum { LABEL_STOP = 1 };
static const uint8_t LABEL_BYTES[256] = {['['] = LABEL_STOP, [']'] = LABEL_STOP, ['\\'] = LABEL_STOP};

static inline int scan_label(const unsigned char *data, bufsize_t len, bufsize_t pos, bufsize_t *label_start,
                             bufsize_t *label_len, bufsize_t *end) {
    int depth = 1;
    bufsize_t i;

    if (pos >= len || data[pos] != '[') {
        return 0;
    }

    for (i = pos + 1; (i = markdown_core_scan_to_class(LABEL_BYTES, LABEL_STOP, data, i, len)) < len;) {
        if (data[i] == '\\') {
            i += i + 1 < len ? 2 : 1;
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
        } else if (--depth == 0) {
            *label_start = pos + 1;
            *label_len = i - (pos + 1);
            *end = i + 1;
            return 1;
        }
        i++;
    }

    return 0;
}

static int set_chunk_bytes(markdown_core_chunk *chunk, const unsigned char *data, bufsize_t len) {
    markdown_core_chunk_free(chunk);
    chunk->data = (unsigned char *)data;
    chunk->len = len;
    chunk->alloc = 0;
    if (!markdown_core_chunk_to_cstr(chunk)) {
        /* Never keep borrowing the transient line buffer. */
        chunk->data = NULL;
        chunk->len = 0;
        return 0;
    }
    return 1;
}

const char *markdown_core_elements_get_directive_name(markdown_core_node *node) {
    node_directive *directive = node->opaque;
    return directive->name.len ? markdown_core_chunk_to_cstr(&directive->name) : NULL;
}

int markdown_core_elements_set_directive_name(markdown_core_node *node, const char *name) {
    return markdown_core_chunk_set_cstr(&((node_directive *)node->opaque)->name, name);
}

static void directive_opaque_alloc(const markdown_core_element *element, markdown_core_node *node) {
    if (is_directive_node(node)) {
        node->opaque = markdown_core_alloc(1, sizeof(node_directive));
    }
}

static void directive_opaque_free(const markdown_core_element *element, markdown_core_node *node) {
    node_directive *directive = (node_directive *)node->opaque;
    if (!directive) {
        return;
    }

    /* Owned roots are released by the shared iterative node destructor. */
    markdown_core_chunk_free(&directive->name);
    markdown_core_free(directive);
    node->opaque = NULL;
}

/* Recognize the shared attribute grammar without constructing semantic
 * strings or records. The parser owns and accounts for recognition work. */
static int scan_directive_attributes(markdown_core_parser *parser, unsigned char *data, bufsize_t len, bufsize_t *pos,
                                     parsed_directive *parsed) {
    parsed->attributes.data = data;
    parsed->attributes.length = len;
    parsed->attributes.scratch = &parser->attribute_scratch;
    bufsize_t end = markdown_core_attributes_end(&parsed->attributes, *pos);
    if (parsed->attributes.oom) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (!end) {
        return 0;
    }
    parsed->attributes_start = *pos;
    parsed->attributes_len = end - *pos;
    *pos = end;
    return 1;
}

static int parse_directive_suffix(markdown_core_parser *parser, unsigned char *data, bufsize_t len, bufsize_t pos,
                                  parsed_directive *parsed) {
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

    if (pos < len && data[pos] == '{' && !scan_directive_attributes(parser, data, len, &pos, parsed)) {
        return 0;
    }

    parsed->end = pos;
    return 1;
}

static markdown_core_node *make_label_node(const markdown_core_element *element, markdown_core_parser *parser,
                                           const unsigned char *label, bufsize_t label_len, bufsize_t start,
                                           bufsize_t end) {
    markdown_core_node *label_node =
        markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_DIRECTIVE_LABEL, element);
    if (!label_node) {
        return NULL;
    }

    markdown_core_strbuf_put(&label_node->content, label, label_len);
    if (label_node->content.oom) {
        markdown_core_parser_release_node(parser, label_node);
        return NULL;
    }
    label_node->where.place = (markdown_core_place){(uint32_t)start, (uint32_t)end};
    return label_node;
}

static int attach_label_node(const markdown_core_element *element, markdown_core_parser *parser,
                             markdown_core_node *directive_node, const unsigned char *label, bufsize_t label_len,
                             bufsize_t start, bufsize_t end) {
    markdown_core_node *label_node;

    label_node = make_label_node(element, parser, label, label_len, start, end);
    if (!label_node) {
        return 0;
    }

    node_directive *directive = get_directive(directive_node);
    if (!directive || directive->label) {
        markdown_core_parser_release_node(parser, label_node);
        return 0;
    }

    directive->label = label_node;

    return 1;
}

static int apply_parsed_directive(const markdown_core_element *element, markdown_core_parser *parser,
                                  markdown_core_node *node, const unsigned char *data, parsed_directive *parsed,
                                  int start_line) {
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (parsed->name_len && !set_chunk_bytes(&directive->name, data + parsed->name_start, parsed->name_len)) {
        return 0;
    }
    if (parsed->attributes_len) {
        const unsigned char *source = data + parsed->attributes_start;
        if (*source == '{') {
            bufsize_t end;
            int matched =
                markdown_core_attributes_parse(&parsed->attributes, parsed->attributes_start, &node->attributes, &end);
            if (parsed->attributes.oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            if (!matched) {
                return 0;
            }
        } else {
            if (!markdown_core_attributes_single_class(&node->attributes, source, parsed->attributes_len)) {
                return 0;
            }
        }
    }

    if (parsed->has_label) {
        /* THE LABEL'S SCOPE SPANS ITS BRACKETS. It used to span the content
         * only, which made `[]` a NEGATIVE range -- end one column before
         * start -- because there was no content to point at. A label always
         * has its two brackets, so the bracket-inclusive range is a place for
         * every label there is, and `:red[]:` reads `1:5..1:6`.
         *
         * A scope is where the user sees these bytes in the editor. `parsed`
         * indexes the block parser's input line, which is the whole source
         * line in the document but a cell's content in a grid or multiline
         * table; the `[` is in its column label_start, and the `]` follows
         * the label. Both are projected from that line to the source. The
         * content is the bytes between them, and its map is that slice of
         * the line's source map, as a callout title's is: in a cell a tab is
         * several bytes of the input but one byte of the source. */
        const int open_column = (int)parsed->label_start;
        const int close_column = open_column + (int)parsed->label_len + 1;
        if (!attach_label_node(element, parser, node, data + parsed->label_start, parsed->label_len,
                               markdown_core_parser_source_offset(parser, start_line, open_column),
                               markdown_core_parser_source_end(parser, start_line, close_column))) {
            return 0;
        }
        if (!markdown_core_parser_append_source_marks(parser, directive->label, start_line, open_column + 1,
                                                      (bufsize_t)parsed->label_len, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
        /* The label is a field, complete once made. */
        markdown_core_parser_complete(parser, directive->label, NULL);
    }

    return 1;
}

static markdown_core_node *make_directive_node(const markdown_core_element *element, markdown_core_parser *parser,
                                               const unsigned char *name, bufsize_t name_len) {
    markdown_core_node *node = markdown_core_parser_make_node_with_ext(parser, MARKDOWN_CORE_NODE_DIRECTIVE, element);
    node_directive *directive;

    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }

    directive = get_directive(node);
    if (!directive) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
    if (!set_chunk_bytes(&directive->name, name, name_len)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_parser_release_node(parser, node);
        return NULL;
    }
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
 * element is ever offered them. There is nothing left to protect. */
static markdown_core_node *match_colon_directive(const markdown_core_element *element, markdown_core_parser *parser,
                                                 markdown_core_node *parent, markdown_core_inline_state *inline_state,
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

    memset(&attributes, 0, sizeof(attributes));

    /* A TEXT DIRECTIVE'S COLON MAY NOT SIT NEXT TO ANOTHER COLON, on either
     * side: a run of colons stays whole, so `x ::a[y]` is text rather than
     * `x :` plus a directive, and `x:::a[y]` is text rather than `x::` plus
     * one. `::name` and `:::name` at the start of a line are leaf and
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

    /* THE INLINE FORM IS ANCHORED ON ITS BRACKET PART. A name alone is text:
     * the proposal's examples always carry a label or attributes, and it is
     * that part, not the name's first character, that tells `:badge[new]`
     * from the colon in `12:30` or `http://`. A part that fails to scan does
     * not anchor; the name commits only once one part has. */
    pos = name_start + name_len;
    if (pos < chunk->len && chunk->data[pos] == '[') {
        bufsize_t label_end;
        if (scan_label(chunk->data, chunk->len, pos, &label_start, &label_len, &label_end)) {
            has_label = 1;
            label_open = pos;
            pos = label_end;
        }
    }

    int has_attributes = 0;
    if (pos < chunk->len && chunk->data[pos] == '{') {
        has_attributes = markdown_core_inline_state_attributes(inline_state, pos, &attributes, &pos);
    }
    if (!has_label && !has_attributes) {
        markdown_core_attributes_free(&attributes);
        return NULL;
    }

    node = make_directive_node(element, parser, chunk->data + name_start, name_len);
    if (!node) {
        markdown_core_attributes_free(&attributes);
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    directive = get_directive(node);
    node->attributes = attributes;

    /* The directive, from its `:` to its last byte, and its label, from the
     * `[` to the `]`, are placed like every inline scope: each endpoint is
     * where the user sees that byte. A label may span a line ending, and the
     * byte after the directive need not be the next byte in the editor: at a
     * table cell's edge it is the cell's line ending, placed at the end of
     * the row. */
    markdown_core_inline_state_place(inline_state, node, (int)offset, (int)pos - 1);

    if (has_label) {
        label_node = make_label_node(element, parser, chunk->data + label_start, label_len, 0, 0);
        if (!label_node) {
            markdown_core_parser_release_node(parser, node);
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        markdown_core_inline_state_place(inline_state, label_node, (int)label_open, (int)(label_start + label_len));
        if (directive->label) {
            markdown_core_parser_release_node(parser, label_node);
            markdown_core_parser_release_node(parser, node);
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return NULL;
        }
        directive->label = label_node;
        /* The field is a view of these source bytes, including line breaks
         * and stripped block prefixes. Do not rebuild its map from one column. */
        markdown_core_parser_adopt_content_marks(parser, &parent->content_map, &label_node->content_map, label_start,
                                                 label_len);
    }

    markdown_core_inline_state_set_offset(inline_state, (int)pos);

    return node;
}

/* ONE BYTE, not two. `]` was claimed because a label's closer had to be
 * recognised as a delimiter to pair with the opener; the label is scanned at
 * the colon now, so the bracket is nobody's business but the core's -- which
 * is what makes `[a](b)` inside a label work like any other link. */
static markdown_core_node *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    markdown_core_chunk *chunk = markdown_core_inline_state_get_chunk(inline_state);
    bufsize_t offset = (bufsize_t)markdown_core_inline_state_get_offset(inline_state);

    if (character == ':') {
        return match_colon_directive(self->element, parser, parent, inline_state, chunk, offset);
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

/* Nameless containers share the named container's ownership and closer. Only
 * the opener's attribute spelling differs; a class word is one literal class. */
static int parse_nameless_suffix(markdown_core_parser *parser, unsigned char *data, bufsize_t len, bufsize_t pos,
                                 parsed_directive *parsed) {
    while (pos < len && markdown_core_is_space_or_tab(data[pos])) {
        pos++;
    }
    if (pos < len && data[pos] == '{') {
        if (!scan_directive_attributes(parser, data, len, &pos, parsed)) {
            return 0;
        }
    } else {
        bufsize_t start = pos;
        while (pos < len && !markdown_core_is_whitespace(data[pos]) && data[pos] != ':' && data[pos] != '{' &&
               data[pos] != '}') {
            pos++;
        }
        if (pos == start) {
            return 0;
        }
        parsed->attributes_start = start;
        parsed->attributes_len = pos - start;
    }
    while (pos < len && markdown_core_is_space_or_tab(data[pos])) {
        pos++;
    }
    pos += count_colons(data, len, pos);
    parsed->end = pos;
    return 1;
}

static bufsize_t scan_directive_block(markdown_core_parser *parser, unsigned char *input, int len, int first,
                                      int indent, parsed_directive *parsed) {
    memset(parsed, 0, sizeof(*parsed));
    if (indent >= 4) {
        return 0;
    }
    bufsize_t colon_count = count_colons(input, (bufsize_t)len, first);
    if (colon_count < 2) {
        return 0;
    }

    bufsize_t suffix = first + colon_count;
    bool nameless =
        colon_count >= 3 && suffix < len && (markdown_core_is_space_or_tab(input[suffix]) || input[suffix] == '{');
    int matched = nameless ? parse_nameless_suffix(parser, input, len, suffix, parsed)
                           : parse_directive_suffix(parser, input, len, suffix, parsed);
    return matched && markdown_core_is_blank_to_line_end(input, parsed->end, len) ? colon_count : 0;
}

static void free_parsed_directive(parsed_directive *parsed) {
    markdown_core_attribute_parser_free(&parsed->attributes);
}

static int probe_directive_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 markdown_core_chunk *input, int first, int indent,
                                 markdown_core_block_reader *reader) {
    (void)self;
    (void)reader;
    parsed_directive parsed;
    bool matched = scan_directive_block(parser, input->data, input->len, first, indent, &parsed) != 0;
    free_parsed_directive(&parsed);
    return matched;
}

static markdown_core_node *open_directive_block(const markdown_core_element_instance *self, int indented,
                                                markdown_core_parser *parser, markdown_core_node *parent_container,
                                                unsigned char *input, int len) {
    (void)indented;
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    parsed_directive parsed;
    bufsize_t colon_count =
        scan_directive_block(parser, input, len, first_nonspace, markdown_core_parser_get_indent(parser), &parsed);
    markdown_core_node *node = NULL;
    node_directive *directive;
    if (!colon_count) {
        goto done;
    }

    node = markdown_core_parser_add_child(parser, parent_container, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK,
                                          (int)first_nonspace + 1);
    if (!node) {
        goto done;
    }

    markdown_core_node_set_element(node, self->element);
    node->opaque = markdown_core_alloc(1, sizeof(node_directive));
    if (!node->opaque || !apply_parsed_directive(self->element, parser, node, input, &parsed,
                                                 markdown_core_parser_get_line_number(parser))) {
        /* The suffix already validated; failure here is allocation loss.
         * The block is in the tree, which the failed parse releases. */
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        node = NULL;
        goto done;
    }

    directive = get_directive(node);
    directive->fence_length = (int)colon_count;
    directive->consume_line = 1;
    markdown_core_parser_advance_offset(parser, (char *)input, len - markdown_core_parser_get_offset(parser), false);

done:
    free_parsed_directive(&parsed);
    return node;
}

/* The one closer rule of the directives module: a bare colon run at least as
 * long as the opener, at indentation zero to three, alone on its line. Shared
 * by the matcher that closes the container and the lookahead that only asks. */
static int directive_closer_line(const node_directive *directive, markdown_core_parser *parser,
                                 const unsigned char *input, int len) {
    bufsize_t first_nonspace = (bufsize_t)markdown_core_parser_get_first_nonspace(parser);
    bufsize_t colon_count = count_colons(input, (bufsize_t)len, first_nonspace);

    return markdown_core_parser_get_indent(parser) <= 3 && colon_count >= (bufsize_t)directive->fence_length &&
           markdown_core_is_blank_to_line_end(input, first_nonspace + colon_count, (bufsize_t)len);
}

static int directive_block_continues(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     const unsigned char *input, int len, markdown_core_node *container) {
    node_directive *directive = get_directive(container);

    if (!directive || directive->fence_length == 2) {
        return 0;
    }

    return directive_closer_line(directive, parser, input, len) ? MARKDOWN_CORE_BLOCK_PENDING_CLOSE : 1;
}

static int directive_block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   unsigned char *input, int len, markdown_core_node *container) {
    node_directive *directive = get_directive(container);

    if (!directive) {
        return 0;
    }

    directive->consume_line = 0;

    return directive_block_continues(self, parser, input, len, container);
}

static const markdown_core_node_type containment_kinds[] = {
    MARKDOWN_CORE_NODE_DIRECTIVE, MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK, MARKDOWN_CORE_NODE_DIRECTIVE_LABEL,
    MARKDOWN_CORE_NODE_NONE};

static int contains_inlines(const markdown_core_element *element, markdown_core_node *node) {
    return node->kind == MARKDOWN_CORE_NODE_DIRECTIVE_LABEL;
}

static int accepts_lines(const markdown_core_element *element, markdown_core_node *node) {
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (node->kind != MARKDOWN_CORE_NODE_DIRECTIVE_BLOCK) {
        return 0;
    }

    return directive->fence_length == 2 || directive->consume_line;
}

static int visit_owned_subtrees(const markdown_core_element *element, markdown_core_node *node,
                                markdown_core_owned_subtree_visitor visitor, void *context) {
    /* The element payload survives kind conversion. Its ownership slots
     * remain live even when the new kind is no longer a directive. */
    node_directive *directive = node->opaque;
    (void)element;
    if (!directive || !directive->label) {
        return 1;
    }
    return visitor(&directive->label, context);
}

/* The opener consumes the complete token; the shared inline parser parses its
 * owned label before continuing beyond it. No close-bracket dispatch exists. */

const markdown_core_element MARKDOWN_CORE_ELEMENT_DIRECTIVE = {
    .interrupts_paragraph = true,

    .pending_close = true,

    .name = "directive",
    .match_inline = match,
    .last_block_matches = directive_block_matches,
    .continues_block = directive_block_continues,
    .maximum_block_indent = 3,
    .try_opening_block = open_directive_block,
    /* `scan_directive_block` needs at least two leading colons, so a line that
     * does not start with one cannot open a directive. */
    .open_block_gate = {.bytes = ":"},
    .probe_block = probe_directive_block,
    .containment_kinds = containment_kinds,
    .contains_inlines_func = contains_inlines,
    .accepts_lines_func = accepts_lines,
    .opaque_alloc_func = directive_opaque_alloc,
    .opaque_free_func = directive_opaque_free,
    .visit_owned_subtrees_func = visit_owned_subtrees,
    .terminates_text = ":",
    .dispatch = ":",
};
