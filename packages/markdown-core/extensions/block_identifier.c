#include "block_identifier.h"
#include "block_internal.h"
typedef struct {
    markdown_core_chunk identifier;
    bufsize_t content_end;
    bool own_line;
} block_identifier;
static bool S_scan_block_identifier(markdown_core_parser *parser, const unsigned char *data, bufsize_t length,
                                    block_identifier *candidate) {
    bufsize_t end = length;
    while (end && markdown_core_is_line_end(data[end - 1])) {
        parser->block_identifier_work++;
        end--;
    }
    while (end && markdown_core_block_is_space_or_tab(data[end - 1])) {
        parser->block_identifier_work++;
        end--;
    }
    parser->block_identifier_work++;
    if (end < 3 || data[end - 1] != '#') {
        return false;
    }
    bufsize_t start = end - 1;
    while (start) {
        unsigned char c = data[start - 1];
        parser->block_identifier_work++;
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
    while (cut && markdown_core_block_is_space_or_tab(data[cut - 1])) {
        parser->block_identifier_work++;
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
    if (!markdown_core_chunk_to_cstr(parser->mem, &identifier)) {
        parser->oom = true;
        return false;
    }
    markdown_core_chunk_free(parser->mem, &owner->attributes.anchor);
    owner->attributes.anchor = identifier;
    return true;
}

void markdown_core_block_attach_paragraph_identifier(markdown_core_parser *parser, markdown_core_node *paragraph) {
    block_identifier candidate;
    if (!S_scan_block_identifier(parser, paragraph->content.ptr, paragraph->content.size, &candidate)) {
        return;
    }
    markdown_core_node *owner = paragraph;
    markdown_core_node *parent = paragraph->parent;
    int line, column;
    if (parent && markdown_core_block_type(parent) == MARKDOWN_CORE_NODE_LIST_ITEM &&
        parent->first_child == paragraph &&
        markdown_core_parser_content_place(
            parser, paragraph, (bufsize_t)(candidate.identifier.data - paragraph->content.ptr), &line, &column) &&
        line == parent->start_line) {
        owner = parent;
    }
    bufsize_t at = (bufsize_t)(candidate.identifier.data - paragraph->content.ptr) + paragraph->content_mark_offset;
    int indent = paragraph->content_mark_count
                     ? parser->line_marks[markdown_core_block_content_mark_at(parser, paragraph, at)].indent
                     : 0;
    if (candidate.own_line && ((indent >= CODE_INDENT) || (!candidate.content_end && owner == paragraph))) {
        return;
    }
    if (S_attach_block_identifier(parser, owner, &candidate)) {
        markdown_core_strbuf_truncate(&paragraph->content, candidate.content_end);
    }
}

bool markdown_core_block_attach_identifier_line(markdown_core_parser *parser, markdown_core_node *parent,
                                                markdown_core_chunk *input) {
    markdown_core_node *owner = parent->last_child;
    block_identifier candidate;
    if (parser->indent >= CODE_INDENT || input->data[parser->first_nonspace] != '#' || !owner ||
        owner->attributes.anchor.len ||
        (markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_LIST &&
         markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_CALLOUT &&
         markdown_core_block_type(owner) != MARKDOWN_CORE_NODE_TABLE) ||
        !S_scan_block_identifier(parser, input->data + parser->first_nonspace, input->len - parser->first_nonspace,
                                 &candidate) ||
        !candidate.own_line || candidate.content_end || !markdown_core_block_ends_with_blank_line(owner)) {
        return false;
    }
    bool followed_by_boundary = parser->lookahead_cursor == parser->lookahead_end;
    if (!followed_by_boundary) {
        markdown_core_block_lookahead lookahead;
        markdown_core_chunk next;
        int first_nonspace, indent, blank_lines;
        if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
            return false;
        }
        markdown_core_parser_lookahead_next(&lookahead, &next, &first_nonspace, &indent, &blank_lines);
        followed_by_boundary = blank_lines > 0;
        markdown_core_parser_lookahead_end(&lookahead);
    }
    if (!followed_by_boundary || parser->oom || !S_attach_block_identifier(parser, owner, &candidate)) {
        return false;
    }
    markdown_core_block_set_end_to_current_line(parser, owner);
    return true;
}
