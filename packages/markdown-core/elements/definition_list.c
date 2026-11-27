#include "definition_list.h"
#include "link.h"
#include "block_internal.h"
#include "directive.h"

/* The elements whose syntax this element asks, as `self->peers` holds
 * them. */
enum { DEFINITION_LINK, DEFINITION_DIRECTIVE };
static const markdown_core_element *const DEFINITION_PEERS[] = {
    [DEFINITION_LINK] = &MARKDOWN_CORE_ELEMENT_LINK, [DEFINITION_DIRECTIVE] = &MARKDOWN_CORE_ELEMENT_DIRECTIVE, NULL};

static inline bool markdown_core_block_definition_marker(markdown_core_reader *reader);
static markdown_core_node *markdown_core_block_open_definition(markdown_core_definition_list_work *counts,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_reader *reader, bool compact);
static bool markdown_core_definition_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               block_start_context *context, block_start *start);
/* THE CANDIDATES of the element (blocks.c, THE CANDIDATES), each record
 * naming its call: the term's, on a line that would open a paragraph, and a
 * body's on a blank line, which keeps the body's continuation. */
enum { DEFINITION_TERM, DEFINITION_BLANK };
typedef struct {
    int call, continuation;
    /* What the call read: the body's next line, and whether the term's
     * list is compact. */
    int line;
    bool compact;
} definition_candidate;

/* THE BODY'S READING of a blank line in `parent`: whether the next line that
 * is not blank is indented to `continuation`, which line that is in
 * `*line`. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool definition_reads(const markdown_core_element_instance *self,
                                                                             markdown_core_parser *parser,
                                                                             markdown_core_node *container,
                                                                             markdown_core_reader *reader,
                                                                             void *record);
static bool definition_body_continues(markdown_core_parser *parser, markdown_core_node *parent, int continuation,
                                      int *line) {
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_PARAGRAPH, &lookahead)) {
        return false;
    }
    int blanks;
    markdown_core_reader *next = markdown_core_parser_lookahead_next(&lookahead, &blanks);
    const bool continues = next && next->indent >= continuation;
    *line = lookahead.line - 1;
    markdown_core_parser_lookahead_end(&lookahead);
    return continues;
}

/* A BODY TAKES A BLANK LINE its reading admits (markdown_core_parser_call). */
static bool accepts_blank(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_node *body, markdown_core_reader *reader) {
    if (body->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        return true;
    }
    ((markdown_core_definition_list_work *)self->state)->work++;
    if (body->as.definition_body->continuation_line > reader->number) {
        return true;
    }
    markdown_core_node *parent = markdown_core_parser_parent(parser, body);
    definition_candidate call = {DEFINITION_BLANK, body->as.definition_body->continuation, 0, false};
    if (markdown_core_parser_call(parser, self, definition_reads, parent, reader, MARKDOWN_CORE_NODE_PARAGRAPH, &call,
                                  false) != MARKDOWN_CORE_CANDIDATE_ACCEPT) {
        return false;
    }
    body->as.definition_body->continuation_line = call.line;
    return true;
}

/* Whether the reader's line holds a definition marker at its first
 * non-space byte. */
static inline bool markdown_core_block_definition_marker(markdown_core_reader *reader) {
    const bufsize_t at = reader->nonspace;
    return reader->indent < 4 && !markdown_core_reader_ends(reader, at + 1) &&
           markdown_core_reader_peek(reader, at) == ':' &&
           markdown_core_is_whitespace(markdown_core_reader_peek(reader, at + 1));
}

/* A definition marker is ':' followed by a space, a tab or the line
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
 * element declares (its lines' `prefix_bytes`, projected to one table in
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
    markdown_core_parser *parser, markdown_core_reader *reader, const markdown_core_node *parent) {
    size_t cursor = markdown_core_reader_after(reader, parser);
    for (int line = 0; line < 2; line++) {
        /* The text's end it met: what follows the text may hold a marker
         * (blocks.c, THE REACH: past the text's end); the reader notes the
         * read of it. */
        size_t at = cursor;
        int c = markdown_core_reader_byte(reader, parser, at);
        const markdown_core_node *container = parent;
        while (c >= 0 && parser->dialect->container_prefix[c]) {
            /* A declared prefix byte that is also a marker byte -- a
             * container whose continuation strips ':' -- cannot be
             * told from the marker here; only the transaction can, so the
             * key admits. No element declares one today; the rule is what
             * lets one do so without this key silently refusing the
             * definitions inside it. */
            if (c == ':') {
                return true;
            }
            if (!markdown_core_is_space_or_tab((unsigned char)c)) {
                container = markdown_core_parser_parent(parser, container);
                if (!container) {
                    return false;
                }
            }
            c = markdown_core_reader_byte(reader, parser, ++at);
        }
        if (c < 0) {
            return false;
        }
        if (!markdown_core_is_line_end((unsigned char)c)) {
            if (c != ':') {
                return false;
            }
            const int next = markdown_core_reader_byte(reader, parser, at + 1);
            return next < 0 || markdown_core_is_whitespace((unsigned char)next);
        }
        /* Blank once stripped: the transaction skips one such line, whose
         * CR is read with the byte after it. */
        cursor = at + 1;
        if (c == '\r') {
            const int next = markdown_core_reader_byte(reader, parser, cursor);
            if (next < 0 || next != '\n') {
                continue;
            }
            cursor++;
        }
    }
    return false;
}

/* THE TERM'S ADMISSION of the reader's line in `parent`, which reads the
 * line and the first bytes of the two lines after it, and so can read the
 * end: whether a term can start there at all. The reading
 * (definition_term_reads) is begun only on a line it admits. The next lines
 * refuse almost every line, and cost the container depth where the
 * reading's reference re-parse costs the line; both are conjuncts of one
 * decision, so the order changes nothing else. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool definition_term_admits(
    const markdown_core_element_instance *self, markdown_core_parser *parser, const markdown_core_node *parent,
    markdown_core_reader *reader) {
    ((markdown_core_definition_list_work *)self->state)->work++;
    if (reader->blank || reader->indent >= 4 || markdown_core_block_definition_marker(reader)) {
        return false;
    }
    /* What the key reads of the lines after decides the line: the reader
     * notes it (THE REACH). */
    return definition_next_lines_admit(parser, reader, parent);
}

/* THE TERM'S READING of the reader's line in `parent` its admission admits:
 * whether a definition marker line follows it, after one blank line at
 * most. */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool definition_term_reads(const markdown_core_element_instance *self,
                                                                      markdown_core_parser *parser,
                                                                      markdown_core_node *parent,
                                                                      markdown_core_reader *reader, bool *compact) {
    markdown_core_definition_list_work *counts = self->state;
    markdown_core_chunk term = {(unsigned char *)markdown_core_reader_rest(reader, reader->nonspace),
                                markdown_core_reader_length(reader) - reader->nonspace, 0};
    if (term.data[0] == '[' && self->peers[DEFINITION_LINK]) {
        counts->work += term.len;
        bool reference = markdown_core_reference_definition_length(self->peers[DEFINITION_LINK], parser, &term) != 0;
        if (reference || parser->error) {
            return false;
        }
    }
    markdown_core_block_lookahead lookahead;
    if (!markdown_core_parser_lookahead_begin(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, &lookahead)) {
        return false;
    }
    int blanks;
    markdown_core_reader *next = markdown_core_parser_lookahead_next(&lookahead, &blanks);
    bool matched = next && blanks <= 1 && markdown_core_block_definition_marker(next);
    if (matched) {
        *compact = blanks == 0;
    }
    markdown_core_parser_lookahead_end(&lookahead);
    return matched;
}

static markdown_core_node *markdown_core_block_open_definition(markdown_core_definition_list_work *counts,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_reader *reader, bool compact) {
    /* A new term requires the separating blank run. If the preceding body's
     * prefix declined it, append at the existing list's definition boundary. */
    if (parent->kind == MARKDOWN_CORE_NODE_DEFINITION) {
        parent = markdown_core_block_close(parser, parent);
    }
    if (parent->kind != MARKDOWN_CORE_NODE_DEFINITION_LIST) {
        parent =
            markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION_LIST, reader->nonspace + 1);
        if (!parent) {
            return NULL;
        }
    }
    markdown_core_node *definition =
        markdown_core_parser_add_child(parser, parent, MARKDOWN_CORE_NODE_DEFINITION, reader->nonspace + 1);
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
    int begin = reader->nonspace, end = markdown_core_reader_length(reader);
    while (end > begin && markdown_core_is_whitespace(markdown_core_reader_peek(reader, end - 1))) {
        end--;
    }
    counts->work += end - begin;
    term->place = markdown_core_reader_span(reader, parser, begin, end);
    markdown_core_strbuf_put(&term->content, markdown_core_reader_bytes(reader, begin), end - begin);
    if (term->content.oom || !markdown_core_reader_marks(reader, parser, term, begin, term->content.size, 0)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_block_settle_field(parser, term, definition);
    markdown_core_reader_end(reader);
    return definition;
}

static bool markdown_core_definition_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node **container, markdown_core_reader *reader,
                                               block_start *start) {
    (void)self;

    int continuation = reader->indent + markdown_core_block_consume_item_marker(reader, 1);
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_DEFINITION_BODY, reader->nonspace + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.definition_body->continuation = continuation;
    const bufsize_t length = markdown_core_reader_length(reader);
    (*container)->internal_offset = (int)markdown_core_reader_span(reader, parser, length - 1, length - 1).end;
    return true;
}

/* What the open reads past the marker (block_start.reads): its padding and
 * where the line ends, which the body's internal offset is. */
static void S_body_reads(const markdown_core_element_instance *self, markdown_core_reader *reader,
                         const block_start *start) {
    (void)self;
    (void)start;
    (void)markdown_core_block_consume_item_marker(reader, 1);
    (void)markdown_core_reader_length(reader);
}

static bool markdown_core_definition_list_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(context->container->kind == MARKDOWN_CORE_NODE_DEFINITION && markdown_core_block_definition_marker(reader))) {
        return false;
    }
    if (parser->sink && self->peers[DEFINITION_DIRECTIVE] && markdown_core_reader_peek(reader, first) == ':' &&
        markdown_core_is_line_end(markdown_core_reader_peek(reader, first + 1))) {
        markdown_core_directive_report_line(parser, first);
    }
    start->kind = MARKDOWN_CORE_NODE_DEFINITION_BODY;
    start->open = markdown_core_definition_list_open;
    start->reads = S_body_reads;
    return true;
}

bool markdown_core_definition_list_continue(markdown_core_parser *parser, markdown_core_node *container,
                                            markdown_core_reader *reader) {
    return markdown_core_reader_indented(reader, container->as.definition_body->continuation, true);
}

/* THE TERM, the paragraph opener's call (markdown_core_parser_call): a
 * line its reading admits opens a definition, with the line as its term. */
static markdown_core_node *try_paragraph(const markdown_core_element_instance *self, int indented,
                                         markdown_core_parser *parser, markdown_core_node *parent,
                                         markdown_core_reader *reader) {
    (void)indented;
    definition_candidate call = {DEFINITION_TERM, 0, 0, false};
    if (markdown_core_parser_call(parser, self, definition_reads, parent, reader, MARKDOWN_CORE_NODE_DEFINITION_LIST,
                                  &call, false) != MARKDOWN_CORE_CANDIDATE_ACCEPT) {
        return NULL;
    }
    return markdown_core_block_open_definition(self->state, parser, parent, reader, call.compact);
}

/* THE FEED of the element's candidates (element.h). The term's reading is
 * decided by the first line after one blank line at most, which the text
 * has finished, or by a second blank line; a body's, by the first line
 * after the blank lines, which tells its indentation as soon as it is
 * read. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, void *state,
                                                   const markdown_core_candidate_line *line, bool *ends) {
    (void)self;
    (void)parser;
    const definition_candidate *candidate = state;
    if (candidate->call == DEFINITION_TERM && line->blanks > 1) {
        return MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    if (!line->reader) {
        *ends = false;
        return MARKDOWN_CORE_CANDIDATE_CONTINUES;
    }
    return candidate->call == DEFINITION_TERM && line->unfinished ? MARKDOWN_CORE_CANDIDATE_WAITS
                                                                  : MARKDOWN_CORE_CANDIDATE_ENDS;
}

/* THE CALL'S READING (element.h, `reads`): the term's of the reader's
 * line, and a body's of the next line that is not blank, from the
 * continuation it had, which nothing after the call writes. */
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline)) bool definition_reads(const markdown_core_element_instance *self,
                                                                             markdown_core_parser *parser,
                                                                             markdown_core_node *container,
                                                                             markdown_core_reader *reader,
                                                                             void *record) {
    definition_candidate *call = record;
    if (call->call == DEFINITION_TERM) {
        return definition_term_admits(self, parser, container, reader) &&
               definition_term_reads(self, parser, container, reader, &call->compact);
    }
    return definition_body_continues(parser, container, call->continuation, &call->line);
}

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return node->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY ||
           markdown_core_definition_list_continue(parser, node, reader);
}
/* A definition list, a definition and a body end where their last child
 * ends, and a body without children ends with its marker's line: taken as
 * each closes, once its children have settled. */
static bool close_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                        markdown_core_node *node, markdown_core_node *parent, markdown_core_reader *reader) {
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

static const markdown_core_node_type DEFINITION_LIST_KINDS[] = {MARKDOWN_CORE_NODE_DEFINITION_LIST,
                                                                MARKDOWN_CORE_NODE_NONE};

/* The reading takes a definition's old bodies back only after a body that
 * holds a node: an empty body stands where the one before it ends (ast.c,
 * publish_body), so the bodies taken back are measured from where the last
 * node before them ends either way. */
static bool resumes(const markdown_core_element_instance *self, const markdown_core_parser *parser,
                    const markdown_core_node *node, const markdown_core_node *next) {
    (void)self;
    (void)parser;
    (void)next;
    const markdown_core_node *last = node->child_count ? node->children[node->child_count - 1] : NULL;
    return !last || last->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY || last->child_count;
}

/* A body reopened at the start of a child's line: the line a blank line's
 * reading found the body goes on to is no later than that line, as a parse
 * of the text before it found, which a line past it no longer reads. */
static void reopen_block(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_node *node, const markdown_core_node *next, bool blank) {
    (void)self;
    (void)parser;
    (void)next;
    (void)blank;
    if (node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        node->as.definition_body->continuation_line = 0;
    }
}

/* A definition holding no open block closes as the next one's term opens,
 * whatever its prefixes carry (markdown_core_block_open_definition). */
static bool yields(const markdown_core_element_instance *self, markdown_core_parser *parser,
                   const markdown_core_node *open, markdown_core_node_type kind) {
    (void)self;
    (void)parser;
    return open->kind == MARKDOWN_CORE_NODE_DEFINITION && kind == MARKDOWN_CORE_NODE_DEFINITION;
}

/* A definition's bodies each start with their own marker. */
static const markdown_core_node_type DEFINITION_KINDS[] = {MARKDOWN_CORE_NODE_DEFINITION, MARKDOWN_CORE_NODE_NONE};

static const markdown_core_edit_ops DEFINITION_LIST_EDIT = {
    .reopens = true,
    /* A term after blank lines joins the list before it. */
    .awaits = DEFINITION_LIST_KINDS,
    .abuts = DEFINITION_KINDS,
    .reopen_block = reopen_block,
    .resumes = resumes,
    .yields = yields,
};

static const markdown_core_candidate_ops DEFINITION_LIST_CANDIDATE = {
    .feed = feed_candidate,
    .reads = definition_reads,
    .size = sizeof(definition_candidate),
};

static const markdown_core_open_ops DEFINITION_LIST_OPENS = {
    .indent = 3,
    .scan = markdown_core_definition_list_scan,
    .scan_needs = {.bytes = ":"},
    .last = try_paragraph,
};

static const markdown_core_line_ops DEFINITION_LIST_LINES = {
    .close = close_block,
    .blank = accepts_blank,
    .prefix = continue_container,
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST = {
    .peers = DEFINITION_PEERS,
    .edit = &DEFINITION_LIST_EDIT,
    .state_size = sizeof(markdown_core_definition_list_work),
    .name = "definition_list",
    .candidate = &DEFINITION_LIST_CANDIDATE,
    .opens = &DEFINITION_LIST_OPENS,
    .lines = &DEFINITION_LIST_LINES,
};

/* A definition's record (markdown_core_record_ops): its term, a field root. */
static int visit_definition(markdown_core_node *node, markdown_core_owned_subtree_visitor visitor, void *context) {
    return !node->as.definition->term || visitor(&node->as.definition->term, context);
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_DEFINITION = {
    .size = sizeof(markdown_core_definition),
    .visit = visit_definition,
};

/* A definition body's record, which owns nothing. */
const markdown_core_record_ops MARKDOWN_CORE_RECORD_DEFINITION_BODY = {
    .size = sizeof(markdown_core_definition_body_value),
};
