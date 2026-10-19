#include "definition_list.h"
#include "attributes.h"
#include "link.h"
#include "table.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { DEFINITION_LIST_TABLE };
static const markdown_core_element *const DEFINITION_LIST_PEERS[] = {
    [DEFINITION_LIST_TABLE] = &MARKDOWN_CORE_ELEMENT_TABLE, NULL};

static bool markdown_core_block_definition_marker(markdown_core_chunk *input, int at, int indent);
static markdown_core_node *markdown_core_block_open_definition(markdown_core_definition_list_work *counts,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_chunk *input, bool compact);
static bool markdown_core_definition_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               block_start_context *context, block_start *start);
/* THE CANDIDATES of the element (blocks.c, THE CANDIDATES), the first of
 * their words naming the call: the term's, on a line that would open a
 * paragraph, and a body's on a blank line, whose second word is the body's
 * continuation. */
enum { DEFINITION_TERM, DEFINITION_BLANK };

/* THE BODY'S READING of a blank line in `parent`: whether the next line that
 * is not blank is indented to `continuation`, which line that is in
 * `*line`. */
static bool definition_body_continues(markdown_core_parser *parser, markdown_core_node *parent, int continuation,
                                      int *line) {
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    const bool continues =
        markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) && indent >= continuation;
    *line = lookahead.line - 1;
    markdown_core_parser_lookahead_end(&lookahead);
    return continues;
}

/* A BODY TAKES A BLANK LINE its reading admits. A call in a commit more text
 * may follow sets the end it reads aside: one that reads it decides nothing,
 * and asks to be a candidate; while it waits, the body declines the line. */
static bool accepts_blank(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *body) {
    if (body->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        return true;
    }
    ((markdown_core_definition_list_work *)self->state)->work++;
    if (body->as.definition_body->continuation_line > parser->line_number) {
        return true;
    }
    markdown_core_node *parent = markdown_core_parser_parent(parser, body);
    if (parser->stream && markdown_core_parser_candidate_live(parser, self, parent)) {
        return false;
    }
    const int continuation = body->as.definition_body->continuation;
    const bool aside = !markdown_core_parser_candidate_final(parser);
    const markdown_core_end_aside end =
        aside ? markdown_core_parser_set_end_aside(parser) : (markdown_core_end_aside){0};
    int line;
    const bool continues = definition_body_continues(parser, parent, continuation, &line);
    if (aside && markdown_core_parser_put_end_back(parser, end)) {
        if (!parser->error) {
            const int words[MARKDOWN_CORE_CANDIDATE_WORDS] = {DEFINITION_BLANK, continuation};
            markdown_core_parser_request_candidate(parser, self, parent, MARKDOWN_CORE_NODE_PARAGRAPH, words, false);
        }
        return false;
    }
    if (continues) {
        body->as.definition_body->continuation_line = line;
    }
    return continues;
}

static bool markdown_core_block_definition_marker(markdown_core_chunk *input, int at, int indent) {
    return indent < 4 && at + 1 < input->len && (input->data[at] == ':' || input->data[at] == '~') &&
           markdown_core_is_whitespace(input->data[at + 1]);
}

/* A definition marker is ':' or '~' followed by a space, a tab or the line
 * end. Asking whether the NEXT line carries one used to mean opening a full
 * lookahead transaction -- chain walk, reserve, snapshot, then a line pulled
 * at 548 Ir -- on every line of every document, because a definition TERM is
 * arbitrary prose and nothing about the term's own line can rule the grammar
 * out. This is that question answered from raw source first, as the
 * grammar's own necessary condition.
 *
 * The transaction accepts only when the next line -- one blank line skipped
 * at most -- begins, once its container prefix is stripped, with a marker.
 * A container continuation strips nothing but indentation and the bytes its
 * element declares (`container_prefix_bytes`, projected to one table in
 * the dialect's `container_prefix`: quote markers, today), so when a marker is
 * there the raw bytes before it are all in that table: walking over them
 * lands on the byte the stripped line would show first, and a raw line made
 * of nothing else is blank once stripped (or a bare quote opener, which the
 * transaction refuses; yielding to the line after it only over-admits).
 * Each container the term is in strips at most one declared byte, so the
 * walk crosses no more of them than there are containers from `parent` up:
 * one more is the stripped line's first byte, a block's opener and no
 * marker. So the key reads a prefix, never a line: its cost is the
 * container depth, whatever the prose's length, where the `memchr` search
 * it replaces read both lines end to end (97% of this hook's own cost on
 * long prose) and admitted any line with ': ' in it. Every answer of false
 * is a line the transaction would refuse too. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool definition_next_lines_admit(
    markdown_core_parser *parser, const markdown_core_node *parent) {
    const unsigned char *cursor = parser->lookahead_cursor;
    for (int line = 0; line < 2 && cursor && !markdown_core_parser_input_ends(parser, cursor); line++) {
        const unsigned char *at = cursor;
        const markdown_core_node *container = parent;
        while (!markdown_core_parser_input_ends(parser, at) && parser->dialect->container_prefix[*at]) {
            /* A declared prefix byte that is also a marker byte -- a
             * container whose continuation strips ':' or '~' -- cannot be
             * told from the marker here; only the transaction can, so the
             * key admits. No element declares one today; the rule is what
             * lets one do so without this key silently refusing the
             * definitions inside it. */
            if (*at == ':' || *at == '~') {
                return true;
            }
            if (!markdown_core_is_space_or_tab(*at)) {
                container = markdown_core_parser_parent(parser, container);
                if (!container) {
                    return false;
                }
            }
            at++;
        }
        if (!markdown_core_parser_input_ends(parser, at) && !markdown_core_is_line_end(*at)) {
            return (*at == ':' || *at == '~') &&
                   (markdown_core_parser_input_ends(parser, at + 1) || markdown_core_is_whitespace(at[1]));
        }
        /* Blank once stripped: the transaction skips one such line. */
        cursor = at;
        if (!markdown_core_parser_input_ends(parser, cursor) && *cursor == '\r') {
            cursor++;
        }
        if (!markdown_core_parser_input_ends(parser, cursor) && *cursor == '\n') {
            cursor++;
        }
    }
    return false;
}

/* THE TERM'S ADMISSION of the line `input` in `parent`, which reads the
 * line and the first bytes of the two lines after it, and so can read the
 * end: whether a term can start there at all. The reading
 * (definition_term_reads) is begun only on a line it admits. The next lines
 * refuse almost every line, and cost the container depth where the
 * reading's reference re-parse costs the line; both are conjuncts of one
 * decision, so the order changes nothing else. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool definition_term_admits(
    const markdown_core_element_instance *self, markdown_core_parser *parser, const markdown_core_node *parent,
    markdown_core_chunk *input) {
    ((markdown_core_definition_list_work *)self->state)->work++;
    return !parser->blank && parser->indent < 4 &&
           !markdown_core_block_definition_marker(input, parser->first_nonspace, parser->indent) &&
           definition_next_lines_admit(parser, parent);
}

/* THE TERM'S READING of a line `input` in `parent` its admission admits:
 * whether a definition marker line follows it, after one blank line at
 * most, that is no caption of a table. A candidate's decision, `decides`,
 * writes nothing. */
static bool definition_term_reads(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                  markdown_core_node *parent, markdown_core_chunk *input, bool *compact, bool decides) {
    markdown_core_definition_list_work *counts = self->state;
    markdown_core_chunk term = {input->data + parser->first_nonspace, input->len - parser->first_nonspace, 0};
    if (term.data[0] == '[') {
        counts->work += term.len;
        markdown_core_attribute_parser attributes = {
            .data = term.data, .length = term.len, .scratch = &parser->attribute_scratch};
        bool reference = markdown_core_reference_definition_length(&term, &attributes) != 0;
        if (attributes.oom) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
        markdown_core_attribute_parser_free(&attributes);
        if (reference || parser->error) {
            return false;
        }
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
        return false;
    }
    markdown_core_chunk next;
    int first, indent, blanks;
    bool matched = markdown_core_parser_lookahead_next(&lookahead, &next, &first, &indent, &blanks) && blanks <= 1 &&
                   markdown_core_block_definition_marker(&next, first, indent);
    if (matched && next.data[first] == ':') {
        /* A dialect without tables has no caption to yield to. */
        const markdown_core_element_instance *table = self->peers[DEFINITION_LIST_TABLE];
        matched = !table || !markdown_core_table_caption_probe(table, &lookahead, &next, first, indent, decides);
    }
    if (matched) {
        *compact = blanks == 0;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return matched;
}

static markdown_core_node *markdown_core_block_open_definition(markdown_core_definition_list_work *counts,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_chunk *input, bool compact) {
    /* A new term requires the separating blank run. If the preceding body's
     * prefix declined it, append at the existing list's definition boundary. */
    if (parent->kind == MARKDOWN_CORE_NODE_DEFINITION) {
        parent = markdown_core_block_close(parser, parent);
    }
    if (parent->kind != MARKDOWN_CORE_NODE_DEFINITION_LIST) {
        parent = markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST,
                                                parser->first_nonspace + 1);
        if (!parent) {
            return NULL;
        }
    }
    markdown_core_node *definition =
        markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION, parser->first_nonspace + 1);
    if (!definition) {
        return NULL;
    }
    definition->as.definition->compact = compact;
    markdown_core_node *term = markdown_core_parser_make_node(parser, MARKDOWN_CORE_NODE_PARAGRAPH);
    if (!term) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return definition;
    }
    definition->as.definition->term = term;
    int begin = parser->first_nonspace, end = input->len;
    while (end > begin && markdown_core_is_whitespace(input->data[end - 1])) {
        end--;
    }
    counts->work += end - begin;
    term->place =
        (markdown_core_place){(uint32_t)markdown_core_parser_source_offset(parser, parser->line_number, begin + 1),
                              (uint32_t)markdown_core_parser_source_end(parser, parser->line_number, end)};
    markdown_core_strbuf_put(&term->content, input->data + begin, end - begin);
    if (term->content.oom || !markdown_core_parser_append_source_marks(parser, term, parser->line_number, begin + 1,
                                                                       term->content.size, 0)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_block_settle_field(parser, term, definition);
    markdown_core_block_advance_offset(parser, input, input->len - 1 - parser->offset, false);
    return definition;
}

static bool markdown_core_definition_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node **container, markdown_core_chunk *input,
                                               block_start *start) {
    (void)self;

    int continuation = parser->indent + markdown_core_block_consume_item_marker(parser, input, 1);
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_DEFINITION_BODY,
                                                parser->first_nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.definition_body->continuation = continuation;
    (*container)->internal_offset =
        (int)markdown_core_parser_source_end(parser, parser->line_number, (int)input->len - 1);
    return true;
}

static bool markdown_core_definition_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(context->container->kind == MARKDOWN_CORE_NODE_DEFINITION &&
          markdown_core_block_definition_marker(input, first, context->indent))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_DEFINITION_BODY;
    start->open = markdown_core_definition_list_open;
    return true;
}

bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, container->as.definition_body->continuation, true);
}

/* THE TERM, the paragraph opener's call: a line its reading admits opens a
 * definition, with the line as its term. A call in a commit more text may
 * follow sets the end it reads aside: one that reads it decides nothing,
 * and asks to be a candidate; while it waits, it declines. */
static markdown_core_node *try_paragraph(const markdown_core_element_instance *self, int indented,
                                         markdown_core_parser *parser, markdown_core_node *parent, unsigned char *data,
                                         int length) {
    (void)indented;
    if (parser->stream && markdown_core_parser_candidate_live(parser, self, parent)) {
        return NULL;
    }
    markdown_core_chunk input = {data, length, 0};
    bool compact = false;
    const bool aside = !markdown_core_parser_candidate_final(parser);
    const markdown_core_end_aside end =
        aside ? markdown_core_parser_set_end_aside(parser) : (markdown_core_end_aside){0};
    const bool term = definition_term_admits(self, parser, parent, &input) &&
                      definition_term_reads(self, parser, parent, &input, &compact, false);
    if (aside && markdown_core_parser_put_end_back(parser, end)) {
        if (!parser->error) {
            const int words[MARKDOWN_CORE_CANDIDATE_WORDS] = {DEFINITION_TERM};
            markdown_core_parser_request_candidate(parser, self, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, words,
                                                   false);
        }
        return NULL;
    }
    if (!term) {
        return NULL;
    }
    return markdown_core_block_open_definition(self->state, parser, parent, &input, compact);
}

/* THE FEED of the element's candidates (element.h). The term's reading is
 * decided by the first line after one blank line at most, which the text
 * has finished, or by a second blank line; a body's, by the first line
 * after the blank lines, which tells its indentation as soon as it is
 * read. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, int *words,
                                                   const markdown_core_chunk *input, int first, int indent, int blanks,
                                                   bool unfinished, bool *ends) {
    (void)self;
    (void)parser;
    (void)first;
    (void)indent;
    if (words[0] == DEFINITION_TERM && blanks > 1) {
        return MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    if (!input) {
        *ends = false;
        return MARKDOWN_CORE_CANDIDATE_CONTINUES;
    }
    return words[0] == DEFINITION_TERM && unfinished ? MARKDOWN_CORE_CANDIDATE_WAITS : MARKDOWN_CORE_CANDIDATE_ENDS;
}

/* The call decided as it would read now (element.h): a body's from the
 * continuation it had, which nothing after the call writes. */
static markdown_core_candidate_decision decide_candidate(const markdown_core_element_instance *self,
                                                         markdown_core_parser *parser, const int *words) {
    markdown_core_chunk line;
    markdown_core_node *container;
    if (!markdown_core_parser_candidate_start(parser, &line, &container)) {
        return MARKDOWN_CORE_CANDIDATE_REJECT;
    }
    bool claims;
    if (words[0] == DEFINITION_TERM) {
        bool compact = false;
        claims = definition_term_admits(self, parser, container, &line) &&
                 definition_term_reads(self, parser, container, &line, &compact, true);
    } else {
        int next;
        claims = definition_body_continues(parser, container, words[1], &next);
    }
    markdown_core_parser_candidate_end(parser);
    return claims ? MARKDOWN_CORE_CANDIDATE_ACCEPT : MARKDOWN_CORE_CANDIDATE_REJECT;
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_chunk *input, const markdown_core_node *joining,
                               bool *taken) {
    (void)self;
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY ||
           markdown_core_definition_list_continue(parser, node, input);
}
/* A definition list, a definition and a body end where their last child
 * ends, and a body without children ends with its marker's line: taken as
 * each closes, once its children have settled. */
static bool close_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node *node, markdown_core_node *parent) {
    (void)self;
    (void)parser;
    (void)parent;
    if (node->child_count) {
        node->place.end = node->children[node->child_count - 1]->place.end;
    } else if (node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        node->place.end = (uint32_t)node->internal_offset;
    }
    return true;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST = {
    .peers = DEFINITION_LIST_PEERS,
    .state_size = sizeof(markdown_core_definition_list_work),
    .close_block = close_block,

    .accepts_blank = accepts_blank,

    .name = "definition_list",
    .continue_container = continue_container,
    .continue_reach = markdown_core_block_indent_reach,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_definition_list_scan,
    .scan_block_gate = {.bytes = ":~"},
    .try_opening_paragraph = try_paragraph,
    .feed_candidate = feed_candidate,
    .decide_candidate = decide_candidate,
};
