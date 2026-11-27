#include "definition_list.h"
#include "services.h"
#include "block_internal.h"

/* The services this element asks, as `self->needs` holds them. */
enum { DEFINITION_LABELS };
static const char *const DEFINITION_NEEDS[] = {[DEFINITION_LABELS] = MARKDOWN_CORE_SERVICE_LABELS, NULL};

static inline bool markdown_core_block_definition_marker(markdown_core_reader *reader);
static markdown_core_node *markdown_core_block_open_definition(markdown_core_definition_list_work *counts,
                                                               markdown_core_parser *parser, markdown_core_node *parent,
                                                               markdown_core_reader *reader, bool compact);
/* Whether the reader's line holds a definition marker at its first
 * non-space byte. */
static inline bool markdown_core_block_definition_marker(markdown_core_reader *reader) {
    const bufsize_t at = reader->nonspace;
    return reader->indent < 4 && !markdown_core_reader_ends(reader, at + 1) &&
           (markdown_core_reader_peek(reader, at) == ':' || markdown_core_reader_peek(reader, at) == '~') &&
           markdown_core_is_whitespace(markdown_core_reader_peek(reader, at + 1));
}

/* A definition marker is ':' or '~' followed by a space, a tab or the line
 * end. Asking whether the NEXT line carries one used to mean opening a full
 * lookahead transaction -- chain walk, reserve, mark, then a line pulled
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
             * container whose continuation strips ':' or '~' -- cannot be
             * told from the marker here; only the transaction can, so the
             * key admits. No element declares one today; the rule is what
             * lets one do so without this key silently refusing the
             * definitions inside it. */
            if (c == ':' || c == '~') {
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
            if (c != ':' && c != '~') {
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

/* THE TERM'S READING of the reader's line in `parent` its admission admits
 * begins: no reference definition. Its look (S_term) reads whether a
 * definition marker line follows it, after one blank line at most
 * (S_term_line). */
static MARKDOWN_CORE_ATTRIBUTE((noinline)) bool definition_term_reads(const markdown_core_element_instance *self,
                                                                      markdown_core_parser *parser,
                                                                      markdown_core_node *parent,
                                                                      markdown_core_reader *reader) {
    markdown_core_definition_list_work *counts = self->state;
    markdown_core_chunk term = {(unsigned char *)markdown_core_reader_rest(reader, reader->nonspace),
                                markdown_core_reader_length(reader) - reader->nonspace, 0};
    const markdown_core_need labels = self->needs[DEFINITION_LABELS];
    if (term.data[0] == '[' && labels.ops) {
        counts->work += term.len;
        bool reference = ((const markdown_core_label_ops *)labels.ops)->definition(labels.instance, parser, &term) != 0;
        if (reference || parser->error) {
            return false;
        }
    }
    return true;
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

/* What a body's start found (WHAT THE START FOUND, block_internal.h):
 * where its marker stands, the column the start began at and the columns
 * its marker and padding take past it, the body's continuation. */
typedef struct {
    bufsize_t marker;
    uint32_t from, past, continuation;
} body_found;
MARKDOWN_CORE_BLOCK_FOUND_FITS(body_found, body_found_fits);

static bool markdown_core_definition_list_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                               markdown_core_node **container, markdown_core_reader *reader,
                                               block_start *start) {
    (void)self;
    const body_found *found = MARKDOWN_CORE_BLOCK_FOUND(start, body_found);
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_DEFINITION_BODY, found->marker + 1);
    if (!*container) {
        return false;
    }
    (*container)->as.definition_body->continuation = (int)found->continuation;
    const bufsize_t length = markdown_core_reader_length(reader);
    (*container)->internal_offset = (int)markdown_core_reader_span(reader, parser, length - 1, length - 1).end;
    return true;
}

enum { DEFINITION_LIST_BODY, DEFINITION_LIST_TERM, DEFINITION_LIST_SHUT, DEFINITION_LIST_BLOCKS };

static void S_definition_list_body(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_definition_list_open(markdown_core_compile_self(compiler), turn->parser,
                                                      turn->container, turn->context->reader, turn->start);
}

/* THE MARKER (code by code, as a list item's): up to three spaces, `:` or
 * `~` in a definition, whitespace after it, and the item's padding. */
static inline body_found *S_body_found(const markdown_core_tokenizer *tokenizer) {
    return MARKDOWN_CORE_BLOCK_FOUND(markdown_core_block_turn_of(tokenizer)->start, body_found);
}

/* The continuation is the marker's columns and the padding's past them,
 * one when the padding is refused. */
static markdown_core_state S_body_at(markdown_core_tokenizer *tokenizer, uint32_t space) {
    body_found *found = S_body_found(tokenizer);
    found->continuation = found->past - found->from + space;
    return markdown_core_block_found(tokenizer, DEFINITION_LIST_BODY);
}

static markdown_core_state S_body_item(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_body_at(tokenizer, markdown_core_column(tokenizer) - S_body_found(tokenizer)->past);
}

static markdown_core_state S_body_one(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_body_at(tokenizer, 1);
}

static markdown_core_state S_body_padded(markdown_core_tokenizer *tokenizer, int code) {
    if (markdown_core_code_space(code)) {
        markdown_core_consume(tokenizer);
        return markdown_core_state_of(S_body_one);
    }
    return S_body_one(tokenizer, code);
}

static markdown_core_state S_body_marked(markdown_core_tokenizer *tokenizer, int code) {
    ((markdown_core_definition_list_work *)((const markdown_core_element_instance *)markdown_core_self(tokenizer))
         ->state)
        ->work++;
    if (!markdown_core_code_space(code) && !markdown_core_code_ending(code)) {
        return markdown_core_nok(tokenizer);
    }
    S_body_found(tokenizer)->past = markdown_core_column(tokenizer);
    return markdown_core_attempt(tokenizer, &markdown_core_item_padding, markdown_core_state_of(S_body_item),
                                 markdown_core_state_of(S_body_padded), code);
}

static markdown_core_state S_body_marker(markdown_core_tokenizer *tokenizer, int code) {
    if (code != ':' && code != '~') {
        return markdown_core_nok(tokenizer);
    }
    S_body_found(tokenizer)->marker = (bufsize_t)(tokenizer->position - tokenizer->base);
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_body_marked);
}

static markdown_core_state markdown_core_definition_list_scan(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->context->container->kind != MARKDOWN_CORE_NODE_DEFINITION) {
        return markdown_core_nok(tokenizer);
    }
    turn->start->kind = MARKDOWN_CORE_NODE_DEFINITION_BODY;
    *S_body_found(tokenizer) = (body_found){.from = markdown_core_column(tokenizer)};
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_body_marker), MARKDOWN_CORE_TOKEN_SPACE, 3, code);
}

/* THE TERM, the paragraph opener's last call: a line its reading admits
 * opens a definition, with the line as its term. The reading is a check
 * of the document's reading: the next line under the same prefixes, one
 * blank line before it at most, holds a definition marker. */
static markdown_core_state S_term_none(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

/* The look's line holds a definition marker: up to three spaces, `:` or
 * `~`, whitespace. */
static markdown_core_state S_term_spaced(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_code_space(code) || markdown_core_code_ending(code) ? markdown_core_ok(tokenizer)
                                                                             : markdown_core_nok(tokenizer);
}

static markdown_core_state S_term_marker(markdown_core_tokenizer *tokenizer, int code) {
    if (code != ':' && code != '~') {
        return markdown_core_nok(tokenizer);
    }
    markdown_core_consume(tokenizer);
    return markdown_core_state_of(S_term_spaced);
}

static markdown_core_state S_term_line(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_parser *parser = turn->parser;
    if (parser->look->blanks > 1) {
        return markdown_core_nok(tokenizer);
    }
    *MARKDOWN_CORE_BLOCK_FOUND(turn->start, bool) = parser->look->blanks == 0;
    return markdown_core_spaces(tokenizer, markdown_core_state_of(S_term_marker), 0, 3, code);
}

static markdown_core_state S_term_next(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_look_next(tokenizer, markdown_core_state_of(S_term_line), markdown_core_state_of(S_term_none),
                                   code);
}

static const markdown_core_construct TERM_LOOK = {.start = S_term_next};

static markdown_core_state S_term_read(markdown_core_tokenizer *tokenizer, bool matched) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (!matched || turn->parser->error) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_block_found(tokenizer, DEFINITION_LIST_TERM);
}

static markdown_core_state S_term_matched(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_term_read(tokenizer, true);
}

static markdown_core_state S_term_unmatched(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return S_term_read(tokenizer, false);
}

static markdown_core_state S_term(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *parent = turn->context->container;
    markdown_core_reader *reader = turn->context->reader;
    *MARKDOWN_CORE_BLOCK_FOUND(turn->start, bool) = false;
    if (!definition_term_admits(self, parser, parent, reader) || !definition_term_reads(self, parser, parent, reader) ||
        parser->error) {
        return markdown_core_nok(tokenizer);
    }
    const markdown_core_look_ask ask = {
        .container = parent, .kind = MARKDOWN_CORE_NODE_DEFINITION_LIST, .construct = &TERM_LOOK};
    return markdown_core_look(tokenizer, &ask, markdown_core_state_of(S_term_matched),
                              markdown_core_state_of(S_term_unmatched), code);
}

static void S_definition_list_term(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    markdown_core_node *opened =
        markdown_core_block_open_definition(self->state, turn->parser, *turn->container, turn->context->reader,
                                            *MARKDOWN_CORE_BLOCK_FOUND(turn->start, bool));
    if (opened) {
        *turn->container = opened;
        turn->opened = true;
    }
}

/* A definition list, a definition and a body end where their last child
 * ends, and a body without children ends with its marker's line: taken as
 * each closes, once its children have settled. */
static void close_block(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    const markdown_core_element_instance *self = compiler->current->context;
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *node = turn->node;
    markdown_core_node *parent = *turn->container;
    markdown_core_reader *reader = turn->reader;
    (void)self;
    (void)parser;
    (void)node;
    (void)parent;
    (void)reader;
    if (node->child_count) {
        node->place.end = node->children[node->child_count - 1]->place.end;
    } else if (node->kind == MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        node->place.end = (uint32_t)node->internal_offset;
    }
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

static const markdown_core_block_construct DEFINITION_LIST_STARTS[] = {
    {.bytes = ":~", .indent = 3, .construct = {.start = markdown_core_definition_list_scan}},
    {.indent = INT_MAX, .precedence = 2, .last = true, .construct = {.start = S_term}},
};

static const markdown_core_compile_ops DEFINITION_LIST_TOKENS[DEFINITION_LIST_BLOCKS] = {
    [DEFINITION_LIST_BODY] = {.enter = S_definition_list_body},
    [DEFINITION_LIST_TERM] = {.enter = S_definition_list_term},
    [DEFINITION_LIST_SHUT] = {.exit = close_block},
};

static const markdown_core_open_ops DEFINITION_LIST_OPENS = {
    .starts = DEFINITION_LIST_STARTS,
    .start_count = 2,
    .tokens = DEFINITION_LIST_TOKENS,
    .token_count = DEFINITION_LIST_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`): a
 * body goes on past its continuation's columns, or on a blank line; a list
 * and a definition go on. */
static markdown_core_state S_lines(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_node *node = markdown_core_block_turn_of(tokenizer)->node;
    if (node->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        return markdown_core_ok(tokenizer);
    }
    return markdown_core_block_indented(tokenizer, (uint32_t)node->as.definition_body->continuation, true, code);
}

/* A BODY TAKES A BLANK LINE its reading admits: a check of the document's
 * reading, whether the next line that is not blank is indented to the
 * body's continuation. */
static markdown_core_state S_body_none(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_nok(tokenizer);
}

static markdown_core_state S_body_indented(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    return markdown_core_ok(tokenizer);
}

static markdown_core_state S_body_line(markdown_core_tokenizer *tokenizer, int code) {
    const markdown_core_definition_list_work *work =
        ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    return markdown_core_indent(tokenizer, markdown_core_state_of(S_body_indented), markdown_core_state_of(S_body_none),
                                (uint32_t)work->body->as.definition_body->continuation, code);
}

static markdown_core_state S_body_next(markdown_core_tokenizer *tokenizer, int code) {
    return markdown_core_look_next(tokenizer, markdown_core_state_of(S_body_line), markdown_core_state_of(S_body_none),
                                   code);
}

static const markdown_core_construct BODY_LOOK = {.start = S_body_next};

static markdown_core_state S_body_read(markdown_core_tokenizer *tokenizer, bool continues, int code) {
    markdown_core_definition_list_work *work =
        ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_parser *parser = turn->parser;
    markdown_core_node *body = work->body;
    work->body = NULL;
    turn->node = body;
    turn->joining = NULL;
    if (!continues || parser->error) {
        return markdown_core_nok(tokenizer);
    }
    body->as.definition_body->continuation_line = parser->look->line - 1;
    return S_lines(tokenizer, code);
}

static markdown_core_state S_body_continues(markdown_core_tokenizer *tokenizer, int code) {
    return S_body_read(tokenizer, true, code);
}

static markdown_core_state S_body_stops(markdown_core_tokenizer *tokenizer, int code) {
    return S_body_read(tokenizer, false, code);
}

/* A blank line in a body, past the line its run was found to go on to:
 * the look decides. */
static markdown_core_state S_body_blank(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    markdown_core_node *body = turn->node;
    markdown_core_definition_list_work *work =
        ((const markdown_core_element_instance *)markdown_core_self(tokenizer))->state;
    work->work++;
    if (body->as.definition_body->continuation_line > turn->reader->number) {
        return S_lines(tokenizer, code);
    }
    markdown_core_parser *parser = turn->parser;
    work->body = body;
    const markdown_core_look_ask ask = {.container = markdown_core_parser_parent(parser, body),
                                        .kind = MARKDOWN_CORE_NODE_PARAGRAPH,
                                        .construct = &BODY_LOOK};
    return markdown_core_look(tokenizer, &ask, markdown_core_state_of(S_body_continues),
                              markdown_core_state_of(S_body_stops), code);
}

static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    if (turn->look || turn->node->kind != MARKDOWN_CORE_NODE_DEFINITION_BODY) {
        return S_lines(tokenizer, code);
    }
    return markdown_core_check(tokenizer, &markdown_core_blank_line, markdown_core_state_of(S_body_blank),
                               markdown_core_state_of(S_lines), code);
}

static const markdown_core_line_ops DEFINITION_LIST_LINES = {
    .shut = &DEFINITION_LIST_TOKENS[DEFINITION_LIST_SHUT],
    .continuation = {.start = S_continue},
};

const markdown_core_element MARKDOWN_CORE_ELEMENT_DEFINITION_LIST = {
    .needs = DEFINITION_NEEDS,
    .edit = &DEFINITION_LIST_EDIT,
    .state_size = sizeof(markdown_core_definition_list_work),
    .name = "definition_list",
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
