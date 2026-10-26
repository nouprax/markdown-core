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
    int line;
    bufsize_t end = 0;
    if (!length || !markdown_core_parser_content_end_place(parser, &paragraph->content_map, length - 1, &line, &end)) {
        return false;
    }
    size_t at = (size_t)end;
    while (at < sink->length && (sink->source[at] == ' ' || sink->source[at] == '\t' || sink->source[at] == '\n' ||
                                 sink->source[at] == '\r')) {
        at++;
    }
    return at >= sink->length;
}

void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *paragraph,
                                                     markdown_core_node *parent) {
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
    int line;
    bufsize_t source;
    /* The last of the parent's children is the paragraph, or the block it
     * was split from, which it settles before: no block comes before it
     * when that is the only one. */
    if (parent && markdown_core_block_type(parent) == MARKDOWN_CORE_NODE_LIST_ITEM && parent->child_count == 1 &&
        markdown_core_parser_content_place(parser, &paragraph->content_map,
                                           (bufsize_t)(candidate.identifier.data - text.data), &line, &source) &&
        markdown_core_parser_starts_on_line(parser, parent, line)) {
        owner = parent;
    }
    bufsize_t at = (bufsize_t)(candidate.identifier.data - text.data) + paragraph->content_map.offset;
    int indent =
        paragraph->content_map.count
            ? parser->line_marks.values[markdown_core_block_content_mark_at(parser, &paragraph->content_map, at)].indent
            : 0;
    if (candidate.own_line && ((indent >= CODE_INDENT) || (!candidate.content_end && owner == paragraph))) {
        return;
    }
    /* The identifier and what follows it are cut from the back of the
     * paragraph's text. */
    if (S_attach_block_identifier(parser, owner, &candidate)) {
        paragraph->cut_back += text.len - candidate.content_end;
    }
}

/* THE BOUNDARY AFTER AN IDENTIFIER LINE, read from the line after it in
 * `parent`: a blank line, or the end of the input. In a commit more text
 * may follow the reading waits where the text read so far ends
 * (markdown_core_parser_lookahead_wait): reaching it with no blank line
 * read leaves the reading `undecided`, and the call asks to be a candidate
 * (blocks.c, THE CANDIDATES). A line the reading returns is not blank,
 * however much of it the text has finished. */
static bool S_followed_by_boundary(markdown_core_parser *parser, markdown_core_node *parent, bool *undecided) {
    const bool waits = !markdown_core_parser_candidate_final(parser);
    *undecided = false;
    if (!waits && markdown_core_parser_input_ends(parser, parser->at.lookahead_cursor)) {
        return true;
    }
    markdown_core_block_lookahead lookahead;
    markdown_core_chunk next;
    int first_nonspace, indent, blank_lines;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    if (waits) {
        markdown_core_parser_lookahead_wait(&lookahead);
    }
    const bool line = markdown_core_parser_lookahead_next(&lookahead, &next, &first_nonspace, &indent, &blank_lines);
    *undecided = !line && !blank_lines && (lookahead.exhausted || lookahead.waited);
    markdown_core_parser_lookahead_end(&lookahead);
    return blank_lines > 0;
}

/* THE IDENTIFIER LINE, the text opener's call (paragraph.c) on a line that
 * would open a paragraph in `parent`: an identifier alone on the line names
 * the block before it, the pending block (parser.h, THE BUILDERS), when the
 * boundary follows it. A call whose boundary reading decides nothing asks
 * to be a candidate, which takes the line as the call would; a call that
 * meets its candidate declines. */
bool markdown_core_block_attach_identifier_line(const markdown_core_element_instance *self,
                                                markdown_core_parser *parser, markdown_core_node *parent,
                                                markdown_core_chunk *input) {
    markdown_core_block_identifier_work *work = self->state;
    markdown_core_node *owner = markdown_core_parser_last_child(parent);
    block_identifier candidate;
    if (parser->at.indent >= CODE_INDENT || input->data[parser->at.first_nonspace] != '#' || !owner ||
        owner->attributes.anchor.len ||
        (markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_LIST &&
         markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_CALLOUT &&
         markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_TABLE) ||
        !S_scan_block_identifier(work, input->data + parser->at.first_nonspace, input->len - parser->at.first_nonspace,
                                 &candidate) ||
        !candidate.own_line || candidate.content_end || !markdown_core_block_ends_with_blank_line(owner) ||
        markdown_core_parser_candidate_meets(parser, self, parent)) {
        return false;
    }
    bool undecided;
    const bool boundary = S_followed_by_boundary(parser, parent, &undecided);
    if (parser->error) {
        return false;
    }
    if (undecided) {
        markdown_core_parser_request_candidate(parser, self, parent, MARKDOWN_CORE_NODE_PARAGRAPH, NULL, true);
        return true;
    }
    if (!boundary || !S_attach_block_identifier(parser, owner, &candidate)) {
        return false;
    }
    markdown_core_block_set_end_to_current_line(parser, owner);
    return true;
}

/* THE FEED of the identifier line's candidate (element.h): the first line
 * after it, or the blank lines before one, decides its boundary. The text
 * ending after the identifier line is a boundary, so the request says the
 * call could claim there. */
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

/* The call decided as it would read now (element.h): its line and the
 * pending block it names, which nothing after the call writes, held when
 * it asked, so what decides is the boundary after its line. */
markdown_core_candidate_decision markdown_core_block_decide_identifier_line(const markdown_core_element_instance *self,
                                                                            markdown_core_parser *parser,
                                                                            const void *state) {
    (void)self;
    (void)state;
    markdown_core_chunk line;
    markdown_core_node *container;
    if (!markdown_core_parser_candidate_start(parser, &line, &container)) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    bool undecided;
    const bool boundary = S_followed_by_boundary(parser, container, &undecided);
    markdown_core_parser_candidate_end(parser);
    return undecided  ? MARKDOWN_CORE_CANDIDATE_WAIT
           : boundary ? MARKDOWN_CORE_CANDIDATE_ACCEPT
                      : MARKDOWN_CORE_CANDIDATE_REJECT;
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
