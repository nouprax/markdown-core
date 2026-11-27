#include "heading_scanners.h"
#include "heading.h"
#include "inline_internal.h"
#include "block_internal.h"
#include "topic.h"
#include "parts.h"
#include "attributes.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { HEADING_TOPIC, HEADING_ATTRIBUTES };
static const markdown_core_element *const HEADING_PEERS[] = {
    [HEADING_TOPIC] = &MARKDOWN_CORE_ELEMENT_TOPIC, [HEADING_ATTRIBUTES] = &MARKDOWN_CORE_ELEMENT_ATTRIBUTES, NULL};

/* A heading closes as a paragraph does. */
static bool close_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *node, markdown_core_node *parent, markdown_core_reader *reader) {
    (void)parent;
    (void)reader;
    /* The end of its text is offered (element.h, markdown_core_follow):
     * what is taken is cut from it. */
    const markdown_core_chunk text = markdown_core_node_text(node);
    (void)self;
    node->cut_back += markdown_core_parser_offer(parser, node, text.data, 0, text.len, false);
    return true;
}

/* A heading's inline content parses as a paragraph's does, from its seams
 * when an edit's reading leaps in it (blocks.c, THE SEAMS). */
static bool heading_seams(const markdown_core_element_instance *self, const markdown_core_node *node) {
    (void)self;
    (void)node;
    return true;
}

void markdown_core_heading_begin_inlines(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                         markdown_core_inline_state *inline_state, markdown_core_node *parent) {
    (void)self;
    (void)parser;
    /* An ATX heading's closing sequence ends its text: what may follow it
     * on the line another grammar has taken from the text already
     * (no-flush.md 10.13). */
    if (parent->kind == MARKDOWN_CORE_NODE_HEADING && !parent->as.heading->setext) {
        bufsize_t hashes = inline_state->input.len;
        while (hashes > 0 && inline_state->input.data[hashes - 1] == '#') {
            hashes--;
        }
        if (hashes < inline_state->input.len &&
            (hashes == 0 || markdown_core_is_space_or_tab(inline_state->input.data[hashes - 1]))) {
            inline_state->input.len = hashes;
            markdown_core_chunk_rtrim(&inline_state->input);
        }
    }
}

/* THE PARTS OF A HEADING (parts.h): the attribute tail that ends its last
 * content line, as markdown_core_heading_begin_inlines finds it, unless a
 * node of its content holds the brace. A setext heading's last content line
 * is the one before its underline. */
void markdown_core_heading_parts(markdown_core_parts *parts) {
    const unsigned char *s = parts->text;
    bufsize_t end = parts->length;
    while (end > 0 && markdown_core_is_line_end(s[end - 1])) {
        end--;
    }
    if (parts->node->as.heading->setext) {
        while (end > 0 && !markdown_core_is_line_end(s[end - 1])) {
            end--;
        }
        while (end > 0 && markdown_core_is_line_end(s[end - 1])) {
            end--;
        }
    }
    bufsize_t line = end;
    while (line > 0 && !markdown_core_is_line_end(s[line - 1])) {
        line--;
    }
    markdown_core_attributes_tail_parts(parts, line, end);
}

#define peek_at(input, at) ((input)->data[(at)])
static bool open_atx(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;

    bufsize_t hashpos;
    int level = 0;
    bufsize_t heading_startpos = reader->nonspace;

    markdown_core_reader_advance(reader, reader->nonspace + matched - reader->offset, false);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_HEADING, heading_startpos + 1);
    if (!*container) {
        return false;
    }

    markdown_core_chunk line = {(unsigned char *)markdown_core_reader_rest(reader, 0),
                                markdown_core_reader_length(reader), 0};
    hashpos = markdown_core_chunk_strchr(&line, '#', reader->nonspace);

    while (markdown_core_reader_peek(reader, hashpos) == '#') {
        level++;
        hashpos++;
    }

    (*container)->as.heading->level = level;
    (*container)->as.heading->setext = false;
    (*container)->internal_offset = matched;

    return true;
}
static bool open_setext(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node **container, markdown_core_reader *reader, block_start *start) {
    (void)self;
    bufsize_t matched = start->matched;
    /* The change of kind cuts the items the paragraph's text opens with
     * first (blocks.c, THE FRONT CUT): what is left becomes the heading. */
    markdown_core_node *parent = markdown_core_parser_parent(parser, *container);
    markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, *container, parent, MARKDOWN_CORE_NODE_HEADING);
    if (result == MARKDOWN_CORE_NODE_SET_KIND_CUT) {
        return true;
    }
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        if (result == MARKDOWN_CORE_NODE_SET_KIND_ALLOCATION_FAILED) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        return false;
    }
    (*container)->as.heading->level = matched;
    (*container)->as.heading->setext = true;
    markdown_core_reader_end(reader);
    return true;
}
static bool scan_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         block_start_context *context, block_start *start) {
    (void)self;
    /* The line is read past its first byte only when that byte may begin
     * the heading's line: a `#`, or an underline's `=` or `-`. */
    const unsigned char first = markdown_core_reader_peek(context->reader, context->reader->nonspace);
    if (first == '#' && (start->matched = scan_atx_heading_start(markdown_core_reader_rest(context->reader, 0),
                                                                 markdown_core_reader_length(context->reader),
                                                                 context->reader->nonspace))) {
        start->open = open_atx;
        if (parser->sink && self->peers[HEADING_TOPIC]) {
            const unsigned char *data = markdown_core_reader_rest(context->reader, 0);
            bufsize_t end = context->reader->nonspace, last = markdown_core_reader_length(context->reader);
            while (end < last && data[end] == '#') {
                end++;
            }
            while (last > end && (data[last - 1] == '\n' || data[last - 1] == '\r')) {
                last--;
            }
            if (end == last) {
                markdown_core_topic_report_line(parser, context->reader->nonspace, end, true);
            } else if (data[last - 1] == '#') {
                /* A closing sequence the input's end cuts short. */
                bufsize_t run = last;
                while (data[run - 1] == '#') {
                    run--;
                }
                if (markdown_core_is_space_or_tab(data[run - 1])) {
                    markdown_core_topic_report_line(parser, run, last, false);
                }
            }
        }
    } else if (context->paragraph && (first == '=' || first == '-') &&
               markdown_core_reader_blank_from(
                   context->reader, context->reader->nonspace +
                                        markdown_core_reader_run(context->reader, context->reader->nonspace, first))) {
        /* An underline: a run of one of them, and only spaces after it. */
        start->matched = first == '=' ? 1 : 2;
        start->open = open_setext;
    } else {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_HEADING;
    return true;
}
static int continue_heading(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_reader *reader, markdown_core_node *container) {
    return 0;
}

/* A setext heading read its lines as a paragraph, which its underline made
 * a heading (THE FORMER KIND). */
static markdown_core_node_type heading_former(const markdown_core_element_instance *self,
                                              const markdown_core_node *node) {
    (void)self;
    return node->kind == MARKDOWN_CORE_NODE_HEADING && node->as.heading->setext ? MARKDOWN_CORE_NODE_PARAGRAPH
                                                                                : MARKDOWN_CORE_NODE_NONE;
}

/* The paragraph it was, its level kept for its underline's redo, without
 * the attributes its inline content gave it, as a new paragraph is. */
static bool heading_revert(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_node *node, markdown_core_node *parent, uint32_t *kept) {
    (void)self;
    markdown_core_heading *value = node->as.heading;
    markdown_core_attributes_free(&node->attributes);
    memset(&node->attributes, 0, sizeof(node->attributes));
    /* What its attributes took from its text is text again. */
    node->cut_back = 0;
    const uint32_t level = (uint32_t)value->level;
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_PARAGRAPH);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    *kept = level;
    return true;
}

/* The heading its old underline made it, as open_setext did. */
static bool heading_redo(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, markdown_core_node *parent, uint32_t kept) {
    (void)self;
    const markdown_core_node_set_kind_result result =
        markdown_core_parser_set_node_kind(parser, node, parent, MARKDOWN_CORE_NODE_HEADING);
    assert(result != MARKDOWN_CORE_NODE_SET_KIND_REJECTED);
    if (result != MARKDOWN_CORE_NODE_SET_KIND_OK) {
        return false;
    }
    node->as.heading->level = (int)kept;
    node->as.heading->setext = true;
    return true;
}

/* A SETEXT UNDERLINE FOLLOWS A PARAGRAPH (element.h, markdown_core_follow):
 * the line makes it a heading (open_setext), which an edit undoes and does
 * again. */
static const markdown_core_node_type HEADING_LEADERS[] = {MARKDOWN_CORE_NODE_PARAGRAPH, MARKDOWN_CORE_NODE_NONE};
static const markdown_core_follow HEADING_FOLLOWS[] = {
    {.at = MARKDOWN_CORE_FOLLOW_LINE,
     .kinds = HEADING_LEADERS,
     .makes = MARKDOWN_CORE_NODE_HEADING,
     .former = heading_former,
     .revert = heading_revert,
     .redo = heading_redo},
    {0},
};

static const markdown_core_finish_ops HEADING_FINISH = {
    .seams = heading_seams,
};

static const markdown_core_content_ops HEADING_CONTENT = {
    .mode = MARKDOWN_CORE_CONTENT_PROSE,
    .inlines = true,
};

static const markdown_core_inline_ops HEADING_INLINES = {
    .enter = markdown_core_heading_begin_inlines,
};

static const markdown_core_open_ops HEADING_OPENS = {
    .indent = 3,
    .scan = scan_heading,
    /* ATX `#`; a setext underline of `=` or `-`. */
    .scan_needs = {.bytes = "#=-"},
};

static const markdown_core_line_ops HEADING_LINES = {
    .matches = continue_heading,
    .close = close_heading,
    .blank_opaque = true,
};

const markdown_core_facet MARKDOWN_CORE_HEADING_FACETS[] = {
    {.kind = MARKDOWN_CORE_KIND_HEADING, .parts = markdown_core_heading_parts},
    {.kind = MARKDOWN_CORE_KIND_NONE},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_HEADING = {
    .facets = MARKDOWN_CORE_HEADING_FACETS,
    .peers = HEADING_PEERS,
    .name = "heading",
    .finish = &HEADING_FINISH,
    .content = &HEADING_CONTENT,
    .inlines = &HEADING_INLINES,
    .opens = &HEADING_OPENS,
    .lines = &HEADING_LINES,
    .follows = HEADING_FOLLOWS,
};

/* A heading's record (markdown_core_record_ops): level 1 until its marker
 * says otherwise. */
static void init_heading(markdown_core_node *node) { node->as.heading->level = 1; }

const markdown_core_record_ops MARKDOWN_CORE_RECORD_HEADING = {
    .size = sizeof(markdown_core_heading),
    .init = init_heading,
};
