#include "alloc.h"
#include "services.h"
#include "block_internal.h"
#include "directive.h"
#include "element.h"
#include "inline_internal.h"

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

/* The services this element asks, as `self->needs` holds them. */
enum { DIRECTIVE_ATTRIBUTES };
static const char *const DIRECTIVE_NEEDS[] = {[DIRECTIVE_ATTRIBUTES] = MARKDOWN_CORE_SERVICE_ATTRIBUTES, NULL};

/* The attribute grammar (services.h), NULL in a dialect without it. */
static const markdown_core_attribute_ops *S_attributes(const markdown_core_element_instance *self) {
    return self->needs[DIRECTIVE_ATTRIBUTES].ops;
}

typedef struct {
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t label_start;
    bufsize_t label_len;
    int has_label;
    /* Borrowed authored attributes: a braced envelope or one class word. */
    bufsize_t attributes_start;
    bufsize_t attributes_len;
    /* What the attribute element recognized of a braced envelope, kept until
     * it writes the value into the committed block, or the declined probe
     * lets it go. */
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

/* Where a name goes on past a cursor. */
static bufsize_t name_end(const unsigned char *data, bufsize_t length) {
    return markdown_core_scan_to_class(NAME_BYTES, NAME_END, data, 0, length);
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

/* A directive's name is made NUL-terminated (set_chunk_bytes,
 * markdown_core_elements_set_directive_name), and a copy views those bytes
 * (directive_opaque_view). */
const char *markdown_core_elements_get_directive_name(const markdown_core_node *node) {
    const node_directive *directive = node->opaque;
    return directive->name.len ? (const char *)directive->name.data : NULL;
}

int markdown_core_elements_set_directive_name(markdown_core_node *node, const char *name) {
    return markdown_core_chunk_set_cstr(&((node_directive *)node->opaque)->name, name);
}

/* The name a directive holds, its bytes as written, or NULL for none. */
static const markdown_core_chunk *S_name(const markdown_core_node *node) {
    const node_directive *directive = node->opaque;
    return directive->name.len ? &directive->name : NULL;
}

static void directive_opaque_alloc(markdown_core_node *node) {
    if (is_directive_node(node)) {
        node->opaque = markdown_core_alloc(1, sizeof(node_directive));
    }
}

static void directive_opaque_free(markdown_core_node_pool *pool, markdown_core_node *node) {
    (void)pool;
    node_directive *directive = (node_directive *)node->opaque;
    if (!directive) {
        return;
    }

    /* Owned roots are released by the shared iterative node destructor. */
    markdown_core_chunk_free(&directive->name);
    markdown_core_free(directive);
    node->opaque = NULL;
}

/* Where the attribute block at `*pos` closes is the attribute grammar's to
 * say (services.h); a dialect without it has none. What it recognized stays
 * in `parsed`, which lets it go either way. */
static int scan_directive_attributes(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     unsigned char *data, bufsize_t len, bufsize_t *pos, parsed_directive *parsed) {
    const markdown_core_attribute_ops *attributes = S_attributes(self);
    if (!attributes) {
        return 0;
    }
    const markdown_core_element_instance *provider = self->needs[DIRECTIVE_ATTRIBUTES].instance;
    attributes->release(provider, &parsed->attributes);
    attributes->begin(provider, &parsed->attributes, data, len);
    const bufsize_t end = attributes->end(provider, &parsed->attributes, *pos);
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

static int parse_directive_suffix(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                  unsigned char *data, bufsize_t len, bufsize_t pos, parsed_directive *parsed) {
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

    if (pos < len && data[pos] == '{' && !scan_directive_attributes(self, parser, data, len, &pos, parsed)) {
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
    label_node->place = (markdown_core_place){(uint32_t)start, (uint32_t)end};
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

static int apply_parsed_directive(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                  markdown_core_node *node, markdown_core_reader *reader, parsed_directive *parsed) {
    const markdown_core_element *element = self->element;
    const unsigned char *data = markdown_core_reader_bytes(reader, 0);
    node_directive *directive = get_directive(node);

    if (!directive) {
        return 0;
    }

    if (parsed->name_len && !set_chunk_bytes(&directive->name, data + parsed->name_start, parsed->name_len)) {
        return 0;
    }
    const markdown_core_field_to fields = {parser, node, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NAME, parsed->name_start,
                            parsed->name_start + parsed->name_len);
    if (parsed->attributes_len) {
        const unsigned char *source = data + parsed->attributes_start;
        if (*source == '{') {
            /* The block's value is the attribute element's to write. */
            bufsize_t end;
            const bool written =
                S_attributes(self)->parse(self->needs[DIRECTIVE_ATTRIBUTES].instance, &parsed->attributes,
                                          parsed->attributes_start, &node->attributes, &end);
            if (parsed->attributes.oom) {
                markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            }
            if (!written) {
                return 0;
            }
            S_attributes(self)->fields(self->needs[DIRECTIVE_ATTRIBUTES].instance, &parsed->attributes, &fields);
        } else {
            if (!markdown_core_attributes_single_class(&node->attributes, source, parsed->attributes_len)) {
                return 0;
            }
            markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_CLASS, parsed->attributes_start,
                                    parsed->attributes_start + parsed->attributes_len);
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
        const bufsize_t from = parsed->label_start, to = from + parsed->label_len;
        const markdown_core_place place = markdown_core_reader_span(reader, parser, from - 1, to + 1);
        if (!attach_label_node(element, parser, node, data + from, parsed->label_len, place.start, place.end)) {
            return 0;
        }
        if (!markdown_core_reader_marks(reader, parser, directive->label, from, (bufsize_t)parsed->label_len, 0)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
            return 0;
        }
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

/* The directive whose `:` is at `colon`, given up with its name, from past
 * the colon to `stop`, kept on the run's node (markdown_core_inline_open). */
static void S_report_name(markdown_core_inline_state *inline_state, bufsize_t colon, bufsize_t stop) {
    const markdown_core_field name = {MARKDOWN_CORE_SLOT_NAME, MARKDOWN_CORE_KIND_DIRECTIVE, colon + 1, stop};
    markdown_core_inline_open(inline_state, MARKDOWN_CORE_KIND_DIRECTIVE, colon, colon + 1, &name, 1);
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
static markdown_core_node *match_colon_directive(const markdown_core_element_instance *self,
                                                 markdown_core_parser *parser, markdown_core_node *parent,
                                                 markdown_core_inline_state *inline_state, markdown_core_chunk *chunk,
                                                 bufsize_t offset) {
    const markdown_core_element *element = self->element;
    bufsize_t name_start;
    bufsize_t name_len;
    bufsize_t pos;
    bufsize_t label_start = 0;
    bufsize_t label_len = 0;
    bufsize_t label_open = 0;
    int has_label = 0;
    markdown_core_node *node;
    markdown_core_node *label_node = NULL;
    node_directive *directive;

    /* A TEXT DIRECTIVE'S COLON MAY NOT SIT NEXT TO ANOTHER COLON, on either
     * side: a run of colons stays whole, so `x ::a[y]` is text rather than
     * `x :` plus a directive, and `x:::a[y]` is text rather than `x::` plus
     * one. `::name` and `:::name` at the start of a line are leaf and
     * container directives and open through the block path, not this one. */
    if (offset > 0 && chunk->data[offset - 1] == ':') {
        return NULL;
    }

    if (offset + 1 >= chunk->len || chunk->data[offset + 1] == ':') {
        if (offset + 1 >= chunk->len) {
            markdown_core_inline_state_saw_end(inline_state);
            S_report_name(inline_state, offset, offset + 1);
        }
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
    /* The name, and the label after it, may go on with the input. */
    if (pos >= chunk->len) {
        markdown_core_inline_state_saw_end(inline_state);
    }
    if (pos < chunk->len && chunk->data[pos] == '[') {
        bufsize_t label_end;
        if (scan_label(chunk->data, chunk->len, pos, &label_start, &label_len, &label_end)) {
            has_label = 1;
            label_open = pos;
            pos = label_end;
        } else {
            markdown_core_inline_state_saw_end(inline_state);
        }
    }
    if (pos >= chunk->len) {
        markdown_core_inline_state_saw_end(inline_state);
    }

    /* Without a label the attribute block anchors the directive: whether one
     * closes here is the attribute grammar's to say, and its value is that
     * element's to write, as it updates the directive (no-flush.md 10.13). */
    const markdown_core_attribute_ops *attributes = S_attributes(self);
    if (!has_label && !(attributes && pos < chunk->len && chunk->data[pos] == '{' &&
                        attributes->closes(self->needs[DIRECTIVE_ATTRIBUTES].instance, inline_state, pos,
                                           MARKDOWN_CORE_KIND_DIRECTIVE))) {
        S_report_name(inline_state, offset, name_start + name_len);
        return NULL;
    }

    node = make_directive_node(element, parser, chunk->data + name_start, name_len);
    if (!node) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return NULL;
    }
    directive = get_directive(node);

    /* The directive, from its `:` to its last byte, and its label, from the
     * `[` to the `]`, are placed like every inline scope: each endpoint is
     * where the user sees that byte. A label may span a line ending, and the
     * byte after the directive need not be the next byte in the editor: at a
     * table cell's edge it is the cell's line ending, placed at the end of
     * the row. */
    markdown_core_inline_state_place(inline_state, node, (int)offset, (int)pos - 1);
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, node);
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_NAME, name_start, name_start + name_len);

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
        markdown_core_text_share(parser, &parent->content_map, &label_node->content_map, label_start, label_len);
    }

    markdown_core_inline_state_set_offset(inline_state, (int)pos);
    markdown_core_inline_made(inline_state, node);

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
        return match_colon_directive(self, parser, parent, inline_state, chunk, offset);
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
static int parse_nameless_suffix(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                 unsigned char *data, bufsize_t len, bufsize_t pos, parsed_directive *parsed) {
    while (pos < len && markdown_core_is_space_or_tab(data[pos])) {
        pos++;
    }
    if (pos < len && data[pos] == '{') {
        if (!scan_directive_attributes(self, parser, data, len, &pos, parsed)) {
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

/* Where a nameless container's class word ends. */
static bufsize_t class_word_end(const unsigned char *data, bufsize_t length) {
    bufsize_t end = 0;
    while (end < length && !markdown_core_is_whitespace(data[end]) && data[end] != ':' && data[end] != '{' &&
           data[end] != '}') {
        end++;
    }
    return end;
}

/* A directive block's opening line, its colons from `first` to `suffix`,
 * that opens none, as the line in hand `reader` reads it: what it leaves
 * open at its end, kept for the block that takes the line's text -- a
 * member of an open attribute block, else the name the colons begin, or the
 * class word a space puts after three or more of them. */
static void S_report_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           const markdown_core_reader *reader, const unsigned char *input, bufsize_t len,
                           bufsize_t first, bufsize_t suffix) {
    while (len > suffix && markdown_core_is_line_end(input[len - 1])) {
        len--;
    }
    const markdown_core_field_to to = {parser, NULL, NULL, reader, 0, NULL};
    const markdown_core_attribute_ops *attributes = S_attributes(self);
    if (attributes && attributes->report(self->needs[DIRECTIVE_ATTRIBUTES].instance, &to,
                                         MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, input, suffix, len)) {
        return;
    }
    bufsize_t word = suffix;
    while (word < len && markdown_core_is_space_or_tab(input[word])) {
        word++;
    }
    markdown_core_field name = {MARKDOWN_CORE_SLOT_NAME, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, suffix, len};
    if (word > suffix && suffix - first >= 3 && word + class_word_end(input + word, len - word) == len) {
        name.slot = MARKDOWN_CORE_SLOT_CLASS;
        name.start = word;
    } else if (suffix + name_end(input + suffix, len - suffix) != len) {
        return;
    }
    markdown_core_field_opener(&to, MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, true, first, suffix, &name, 1);
}

static bufsize_t scan_directive_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                      const markdown_core_reader *reader, unsigned char *input, int len, int first,
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
    int matched = nameless ? parse_nameless_suffix(self, parser, input, len, suffix, parsed)
                           : parse_directive_suffix(self, parser, input, len, suffix, parsed);
    if (matched && markdown_core_is_blank_to_line_end(input, parsed->end, len)) {
        return colon_count;
    }
    S_report_block(self, parser, reader, input, len, first, suffix);
    return 0;
}

static markdown_core_node *open_directive_block(const markdown_core_element_instance *self, int indented,
                                                markdown_core_parser *parser, markdown_core_node *parent_container,
                                                markdown_core_reader *reader) {
    (void)indented;
    bufsize_t first_nonspace = (bufsize_t)reader->nonspace;
    parsed_directive parsed;
    /* An opener's colons come first: a line without two reads no further. */
    bufsize_t colon_count =
        reader->indent < 4 && markdown_core_reader_run(reader, first_nonspace, ':') >= 2
            ? scan_directive_block(self, parser, reader, (unsigned char *)markdown_core_reader_rest(reader, 0),
                                   markdown_core_reader_length(reader), first_nonspace, reader->indent, &parsed)
            : (memset(&parsed, 0, sizeof(parsed)), 0);
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
    if (!node->opaque || !apply_parsed_directive(self, parser, node, reader, &parsed)) {
        /* The suffix already validated; failure here is allocation loss.
         * The block is the parse's builder from its opening and goes with
         * the parse. */
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        node = NULL;
        goto done;
    }

    directive = get_directive(node);
    directive->fence_length = (int)colon_count;
    directive->consume_line = 1;
    if (directive->label) {
        markdown_core_block_settle_field(parser, directive->label, node);
    }
    markdown_core_reader_advance(reader, markdown_core_reader_length(reader) - reader->offset, false);

done:
    if (S_attributes(self)) {
        S_attributes(self)->release(self->needs[DIRECTIVE_ATTRIBUTES].instance, &parsed.attributes);
    }
    return node;
}

/* The one closer rule of the directives module: a bare colon run at least as
 * long as the opener, at indentation zero to three, alone on its line. Shared
 * by the matcher that closes the container and the lookahead that only asks. */
static int directive_closer_line(const node_directive *directive, markdown_core_parser *parser,
                                 markdown_core_reader *reader) {
    (void)parser;
    if (reader->indent > 3) {
        return 0;
    }
    /* The line is read only as far as it decides: the colons, and past them
     * to the first byte that is no space. */
    const bufsize_t first_nonspace = (bufsize_t)reader->nonspace;
    const bufsize_t colon_count = markdown_core_reader_run(reader, first_nonspace, ':');
    return colon_count >= (bufsize_t)directive->fence_length &&
           markdown_core_reader_blank_from(reader, first_nonspace + colon_count);
}

static int directive_block_continues(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                     markdown_core_reader *reader, markdown_core_node *container) {
    node_directive *directive = get_directive(container);

    if (!directive || directive->fence_length == 2) {
        return 0;
    }

    if (!directive_closer_line(directive, parser, reader)) {
        return 1;
    }
    return MARKDOWN_CORE_BLOCK_PENDING_CLOSE;
}

static int directive_block_matches(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_reader *reader, markdown_core_node *container) {
    node_directive *directive = get_directive(container);

    if (!directive) {
        return 0;
    }

    directive->consume_line = 0;

    return directive_block_continues(self, parser, reader, container);
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

static int visit_owned_subtrees(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    /* The element payload survives kind conversion. Its ownership slots
     * remain live even when the new kind is no longer a directive. */
    node_directive *directive = node->opaque;
    if (!directive || !directive->label) {
        return 1;
    }
    return visitor(&directive->label, context);
}

/* The opener consumes the complete token; the shared inline parser parses its
 * owned label before continuing beyond it. No close-bracket dispatch exists. */

static const markdown_core_edit_ops DIRECTIVE_EDIT = {
    .reopens = true,
};

static const markdown_core_record_ops DIRECTIVE_PAYLOAD = {
    .init = directive_opaque_alloc,
    .free = directive_opaque_free,
    .visit = visit_owned_subtrees,
};

static const markdown_core_contain_ops DIRECTIVE_CONTAIN = {
    .kinds = containment_kinds,
};

static const markdown_core_content_ops DIRECTIVE_CONTENT = {
    .holds_inlines = contains_inlines,
    .holds_lines = accepts_lines,
};

static const markdown_core_inline_ops DIRECTIVE_INLINES = {
    .match = match,
    .terminates = ":",
    .dispatch = ":",
};

static const markdown_core_open_ops DIRECTIVE_OPENS = {
    .indent = 3,
    .open = open_directive_block,
    /* `scan_directive_block` needs at least two leading colons, so a line that
     * does not start with one cannot open a directive. */
    .open_needs = {.bytes = ":"},
};

static const markdown_core_line_ops DIRECTIVE_LINES = {
    .pending = true,
    .matches = directive_block_matches,
    .continues = directive_block_continues,
};

/* Where a directive's label is (element.h, markdown_core_facet). */
static markdown_core_node **S_label(const markdown_core_node *node) {
    return &((markdown_core_directive_value *)node->opaque)->label;
}

/* An inline directive reads as its label in a name. */
static bool S_words(const markdown_core_node *node, const markdown_core_words *words) {
    const markdown_core_node *label = *S_label(node);
    return !label || markdown_core_words_children(label, words);
}

const markdown_core_facet MARKDOWN_CORE_DIRECTIVE_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_DIRECTIVE, .name = S_name, .label = S_label, .words = S_words},
    {.kind = MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, .name = S_name, .label = S_label},
    {.kind = MARKDOWN_CORE_KIND_DIRECTIVE_LABEL, .reads = MARKDOWN_CORE_READS_CHILDREN},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_DIRECTIVE = {
    .needs = DIRECTIVE_NEEDS,
    .facets = MARKDOWN_CORE_DIRECTIVE_FACETS,
    .edit = &DIRECTIVE_EDIT,
    .name = "directive",
    .payload = &DIRECTIVE_PAYLOAD,
    .contain = &DIRECTIVE_CONTAIN,
    .content = &DIRECTIVE_CONTENT,
    .inlines = &DIRECTIVE_INLINES,
    .opens = &DIRECTIVE_OPENS,
    .lines = &DIRECTIVE_LINES,
};
