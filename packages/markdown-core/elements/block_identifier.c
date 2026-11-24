#include "block_identifier.h"
#include "block_internal.h"
#include "markdown-core-elements.h"
#include <string.h>

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

void markdown_core_block_attach_paragraph_identifier(markdown_core_block_identifier_work *work,
                                                     markdown_core_parser *parser, markdown_core_node *paragraph,
                                                     markdown_core_node *parent, markdown_core_reader *reader) {
    block_identifier candidate;
    const markdown_core_chunk text = markdown_core_node_text(paragraph);
    if (!S_scan_block_identifier(work, text.data, text.len, &candidate)) {
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
        /* The paragraph's text holds it: its own field, or lent to the list
         * item it names. */
        const markdown_core_field_to own = {parser, paragraph, &paragraph->content_map, NULL, 0, NULL};
        markdown_core_field_put(&own, MARKDOWN_CORE_SLOT_ID,
                                owner == paragraph ? MARKDOWN_CORE_FIELD : MARKDOWN_CORE_FIELD_LENT, at,
                                at + candidate.identifier.len, true);
        if (owner != paragraph) {
            const markdown_core_field_to item = {parser, owner, &paragraph->content_map, NULL, 0, NULL};
            markdown_core_field_put(&item, MARKDOWN_CORE_SLOT_ID, MARKDOWN_CORE_FIELD, at,
                                    at + candidate.identifier.len, true);
        }
    }
}

/* THE BOUNDARY AFTER AN IDENTIFIER LINE: the identifier line on the reader
 * names the pending block in `container` (parser.h, THE BUILDERS), which
 * nothing after it writes, when the line after it is blank or the input
 * ends. */
static bool S_identifier_bounded(markdown_core_parser *parser, markdown_core_node *container,
                                 markdown_core_reader *reader) {
    markdown_core_reader_write(reader, markdown_core_reader_last(reader, parser, container));
    if (markdown_core_reader_byte(reader, parser, markdown_core_reader_after(reader, parser)) < 0) {
        return true;
    }
    markdown_core_block_lookahead lookahead;
    int blank_lines;
    if (!markdown_core_parser_lookahead_begin(parser, container, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    markdown_core_parser_lookahead_next(&lookahead, &blank_lines);
    markdown_core_parser_lookahead_end(&lookahead);
    return blank_lines > 0;
}

/* THE IDENTIFIER LINE, the text opener's call (paragraph.c) on a line that
 * would open a paragraph in `parent`: an identifier alone on the line names
 * the block before it when the boundary follows it. */
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
    /* The line decides the block before it, which it names: one of the
     * kinds the paragraph's follows name (paragraph.c, PARAGRAPH_FOLLOWS). */
    markdown_core_node *owner = markdown_core_reader_last(reader, parser, parent);
    if (!owner || owner->attributes.anchor.len ||
        !(markdown_core_parser_block_kind(parser, owner)->flags & MARKDOWN_CORE_KIND_TRAILED) ||
        !markdown_core_block_ends_with_blank_line(owner)) {
        return false;
    }
    if (!S_identifier_bounded(parser, parent, reader) || parser->error ||
        !S_attach_block_identifier(parser, owner, &candidate)) {
        return false;
    }
    markdown_core_block_set_end_to_current_line(parser, owner);
    const markdown_core_field_to fields = {parser, owner, NULL, reader, 0, NULL};
    const bufsize_t at = (bufsize_t)(candidate.identifier.data - markdown_core_reader_rest(reader, 0));
    markdown_core_field_put(&fields, MARKDOWN_CORE_SLOT_ID, MARKDOWN_CORE_FIELD, at, at + candidate.identifier.len,
                            true);
    return true;
}

bool markdown_core_block_identifier_writes(const markdown_core_element_instance *self, const unsigned char *line,
                                           size_t length) {
    (void)self;
    return length && memchr(line, '#', length) != NULL;
}
