#include "block_identifier.h"
#include "block_internal.h"
#include "markdown-core-elements.h"
#include "sink.h"
#include "parts.h"

typedef struct {
    markdown_core_chunk identifier;
    bufsize_t content_end;
    bool own_line;
} block_identifier;
static bool S_scan_block_identifier(markdown_core_block_identifier_work *counts, const unsigned char *data,
                                    bufsize_t length, block_identifier *candidate) {
    bufsize_t end = length;
    while (end && markdown_core_is_line_end(data[end - 1])) {
        counts->scan++;
        end--;
    }
    while (end && markdown_core_is_space_or_tab(data[end - 1])) {
        counts->scan++;
        end--;
    }
    counts->scan++;
    if (end < 3 || data[end - 1] != '#') {
        return false;
    }
    bufsize_t start = end - 1;
    while (start) {
        unsigned char c = data[start - 1];
        counts->scan++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            break;
        }
        start--;
    }
    if (!start || start == end - 1 || data[start - 1] != '#') {
        return false;
    }
    candidate->identifier = (markdown_core_chunk){(unsigned char *)data + start, end - start - 1, 0};
    bufsize_t cut = start - 1;
    while (cut && markdown_core_is_space_or_tab(data[cut - 1])) {
        counts->scan++;
        cut--;
    }
    candidate->own_line = !cut || markdown_core_is_line_end(data[cut - 1]);
    if (!candidate->own_line && cut == start - 1) {
        return false;
    }
    if (candidate->own_line && cut) {
        if (data[cut - 1] == '\n') {
            cut--;
        }
        if (cut && data[cut - 1] == '\r') {
            cut--;
        }
    }
    candidate->content_end = cut;
    return true;
}

static bool S_attach_block_identifier(markdown_core_parser *parser, markdown_core_node *owner,
                                      const block_identifier *candidate) {
    if (owner->attributes.anchor.len) {
        return false;
    }
    markdown_core_chunk identifier = candidate->identifier;
    if (!markdown_core_chunk_to_cstr(&identifier)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }
    markdown_core_chunk_free(&owner->attributes.anchor);
    owner->attributes.anchor = identifier;
    return true;
}

/* Whether the content of `paragraph`, `length` bytes, ends the text
 * before the cursor but for spaces and line endings. */
static bool S_ends_text(markdown_core_parser *parser, markdown_core_node *paragraph, bufsize_t length) {
    const markdown_core_context_sink *sink = parser->sink;
    markdown_core_place span;
    if (!length || !markdown_core_text_place(parser, &paragraph->content_map, length - 1, length, &span)) {
        return false;
    }
    size_t at = span.end;
    while (at < sink->length && (sink->source[at] == ' ' || sink->source[at] == '\t' || sink->source[at] == '\n' ||
                                 sink->source[at] == '\r')) {
        at++;
    }
    return at >= sink->length;
}

void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *paragraph,
                                                     markdown_core_node *parent, markdown_core_reader *reader) {
    block_identifier candidate;
    const markdown_core_chunk text = markdown_core_node_text(paragraph);
    if (!S_scan_block_identifier(work, text.data, text.len, &candidate)) {
        return;
    }
    /* In the text before a cursor, an identifier the cursor follows may be
     * text of a construct the line goes on with: the run reads it. */
    if (parser->sink && S_ends_text(parser, paragraph, text.len)) {
        return;
    }
    markdown_core_node *owner = paragraph;
    const bufsize_t at = (bufsize_t)(candidate.identifier.data - text.data);
    int line;
    /* The last of the parent's children is the paragraph, or the block it
     * was split from, which it settles before: no block comes before it
     * when that is the only one. */
    if (parent && markdown_core_block_type(parent) == MARKDOWN_CORE_NODE_LIST_ITEM && parent->child_count == 1 &&
        (line = markdown_core_text_line(parser, &paragraph->content_map, at)) &&
        markdown_core_reader_starts(reader, parser, parent, line)) {
        owner = parent;
    }
    const int indent = markdown_core_text_indent(parser, &paragraph->content_map, at);
    if (candidate.own_line && ((indent >= CODE_INDENT) || (!candidate.content_end && owner == paragraph))) {
        return;
    }
    /* The identifier and what follows it are cut from the back of the
     * paragraph's text. */
    if (S_attach_block_identifier(parser, owner, &candidate)) {
        paragraph->cut_back += text.len - candidate.content_end;
    }
}

/* THE BOUNDARY AFTER AN IDENTIFIER LINE, the call's reading (element.h,
 * `reads`): the identifier line on the reader names the pending block in
 * `container` (parser.h, THE BUILDERS), which nothing after the call writes,
 * when the line after it is blank or the input ends. In a commit more text
 * may follow the reading waits where the text read so far ends
 * (markdown_core_parser_lookahead_wait): reaching it with no blank line
 * read reads the end. A line the reading returns is not blank, however much
 * of it the text has finished. The text ending after the identifier line
 * is a boundary, so the call says it could claim there. */
bool markdown_core_block_identifier_reads(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                          markdown_core_node *container, markdown_core_reader *reader, void *record) {
    (void)self;
    (void)record;
    markdown_core_reader_write(reader, markdown_core_reader_last(reader, parser, container));
    if (markdown_core_parser_candidate_final(parser) &&
        markdown_core_reader_byte(reader, parser, markdown_core_reader_after(reader, parser)) < 0) {
        return true;
    }
    markdown_core_block_lookahead lookahead;
    int blank_lines;
    if (!markdown_core_parser_lookahead_begin(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    markdown_core_parser_lookahead_wait(&lookahead);
    const bool line = markdown_core_parser_lookahead_next(&lookahead, &blank_lines) != NULL;
    if (!line && !blank_lines && (lookahead.exhausted || lookahead.waited)) {
        markdown_core_parser_see_end(parser);
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return blank_lines > 0;
}

/* THE IDENTIFIER LINE, the text opener's call (paragraph.c) on a line that
 * would open a paragraph in `parent` (markdown_core_parser_call): an
 * identifier alone on the line names the block before it when the boundary
 * follows it. A call that waits takes the line as its candidate would. */
bool markdown_core_block_attach_identifier_line(const markdown_core_element_instance *self,
                                                markdown_core_parser *parser, markdown_core_node *parent,
                                                markdown_core_reader *reader) {
    markdown_core_block_identifier_work *work = self->state;
    block_identifier candidate;
    if (reader->indent >= CODE_INDENT || markdown_core_reader_peek(reader, reader->nonspace) != '#' ||
        !S_scan_block_identifier(work, markdown_core_reader_rest(reader, reader->nonspace),
                                 markdown_core_reader_length(reader) - reader->nonspace, &candidate) ||
        !candidate.own_line || candidate.content_end) {
        return false;
    }
    /* The line decides the block before it, which it names: one whose
     * element says a later line may write it (element.h,
     * `trailed_kinds`). */
    markdown_core_node *owner = markdown_core_reader_last(reader, parser, parent);
    if (!owner || owner->attributes.anchor.len ||
        !(markdown_core_parser_block_kind(parser, owner)->flags & MARKDOWN_CORE_KIND_TRAILED) ||
        !markdown_core_block_ends_with_blank_line(owner)) {
        return false;
    }
    const markdown_core_candidate_decision decision =
        markdown_core_parser_call(parser, self, parent, reader, MARKDOWN_CORE_NODE_PARAGRAPH, NULL, true);
    if (decision == MARKDOWN_CORE_CANDIDATE_WAIT) {
        return true;
    }
    if (decision != MARKDOWN_CORE_CANDIDATE_ACCEPT || !S_attach_block_identifier(parser, owner, &candidate)) {
        return false;
    }
    markdown_core_block_set_end_to_current_line(parser, owner);
    return true;
}

/* THE FEED of the identifier line's candidate (element.h): the first line
 * after it, or the blank lines before one, decides its boundary. */
markdown_core_candidate_step markdown_core_block_feed_identifier_line(const markdown_core_element_instance *self,
                                                                      markdown_core_parser *parser, void *state,
                                                                      const markdown_core_candidate_line *line,
                                                                      bool *ends) {
    (void)self;
    (void)parser;
    (void)state;
    (void)line;
    (void)ends;
    return MARKDOWN_CORE_CANDIDATE_ENDS;
}

/* Where the identifier `#anchor#` stands on a line of its own or at a
 * line's end in the text: the first such place, or the last when `last`;
 * -1 for none. */
static bufsize_t S_identifier_place(const markdown_core_parts *parts, const markdown_core_chunk *anchor, bool last) {
    const unsigned char *s = parts->text;
    const bufsize_t n = parts->length, size = anchor->len;
    bufsize_t found = -1;
    for (bufsize_t at = 0; at + size + 2 <= n; at++) {
        if (s[at] != '#' || s[at + size + 1] != '#' || memcmp(s + at + 1, anchor->data, (size_t)size) ||
            (at && !markdown_core_is_space_or_tab(s[at - 1]) && !markdown_core_is_line_end(s[at - 1]))) {
            continue;
        }
        bufsize_t end = at + size + 2;
        while (end < n && markdown_core_is_space_or_tab(s[end])) {
            end++;
        }
        if (end >= n || markdown_core_is_line_end(s[end])) {
            found = at;
            if (!last) {
                break;
            }
        }
    }
    return found;
}

/* THE PARTS OF A BLOCK THAT A BLOCK IDENTIFIER NAMES (parts.h): the
 * identifier a paragraph's text ends with, as S_scan_block_identifier
 * reads it, which is the paragraph's own or, when its list item took it,
 * lent; and a list's, a table's or a callout's on the line after it. */
void markdown_core_block_identifier_parts(markdown_core_parts *parts) {
    const markdown_core_node *node = parts->node;
    if (markdown_core_node_get_kind(node) == MARKDOWN_CORE_KIND_PARAGRAPH) {
        markdown_core_block_identifier_work work = {0};
        block_identifier candidate;
        if (node->cut_back && S_scan_block_identifier(&work, parts->text, parts->length, &candidate)) {
            const bufsize_t at = (bufsize_t)(candidate.identifier.data - parts->text);
            if (node->attributes.anchor.len) {
                markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_ID, at, at + candidate.identifier.len);
            } else {
                markdown_core_parts_lend(parts, MARKDOWN_CORE_SLOT_ID, at, at + candidate.identifier.len);
            }
        }
        return;
    }
    if (node->attributes.anchor.len) {
        const bufsize_t at = S_identifier_place(parts, &node->attributes.anchor,
                                                markdown_core_node_get_kind(node) != MARKDOWN_CORE_KIND_LIST_ITEM);
        if (at >= 0) {
            markdown_core_parts_add(parts, MARKDOWN_CORE_SLOT_ID, at + 1, at + 1 + node->attributes.anchor.len);
        }
    }
}
