#include <string.h>
#include "alloc.h"
#include "footnote_scanners.h"
#define MAX_FOOTNOTE_DEPTH 100
#include "footnote.h"
#include "attention.h"
#include "label.h"
#include "inline_internal.h"
#include "block_internal.h"

/* An inline note's label, opened by a `^[`: a body of its own
 * (label.h). */
static const markdown_core_label_shape MARKDOWN_CORE_FOOTNOTE_BRACKET = {
    .width = 2, .mark = -1, .own = true, .closer = ']', .kind = MARKDOWN_CORE_KIND_FOOTNOTE};

const markdown_core_space MARKDOWN_CORE_FOOTNOTE_SPACE = {.name = "footnote", .folds = true};
/* The spaces this element files labels in, as `self->spaces` numbers them. */
enum { FOOTNOTE_LABELS };
static const markdown_core_space *const FOOTNOTE_SPACES[] = {[FOOTNOTE_LABELS] = &MARKDOWN_CORE_FOOTNOTE_SPACE, NULL};

static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_node *node, markdown_core_reader *reader,
                               const markdown_core_node *joining, bool *taken) {
    (void)self;
    return markdown_core_footnote_continue(parser, node, reader);
}
static const markdown_core_edit_ops FOOTNOTE_EDIT = {
    .reopens = true,
};

/* THE FOOTNOTE'S TEXT (plan symbols-cleanroom.md 23.12): a `^[` opens a
 * label (label.h) where the dialect closes one; its own rule makes an
 * inline note of it, a NOTE group over its body, and the shared call rule
 * makes a `[^label]` the document defines a call, a CALL group of what
 * the call read (S_call), whole. */
enum { FOOTNOTE_NOTE, FOOTNOTE_CALL, FOOTNOTE_KINDS };

/* WHAT THE CALL RULE READ: where the cite starts and its label's caret
 * is, where the label's field ends, and the definition's name, kept at
 * `name`, `name_size` bytes. */
typedef struct S_call {
    uint32_t start, position, end;
    uint32_t name, name_size;
} S_call;

static markdown_core_state S_start(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    const markdown_core_element_instance *self = markdown_core_self(tokenizer);
    if (markdown_core_code_at(tokenizer, 1) != '[' ||
        !markdown_core_label_closes(self, ((const markdown_core_inline_state *)tokenizer->reader)->dialect)) {
        return markdown_core_nok(tokenizer);
    }
    return markdown_core_label_open(tokenizer, &MARKDOWN_CORE_FOOTNOTE_BRACKET);
}

static markdown_core_bracket_match S_end_note(const markdown_core_element_instance *self,
                                              markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                              markdown_core_label_made *made) {
    const unsigned char *data = tokenizer->data;
    const uint32_t close = made->past - 1;
    /* A body of nothing but spaces and tabs is no note, and its label is
     * text. */
    uint32_t at = close;
    while (at > label->position && markdown_core_is_space_or_tab(data[at - 1])) {
        at--;
    }
    made->links = -1;
    if (at == label->position ||
        !markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE, MARKDOWN_CORE_NODE_CITE)) {
        made->kind = MARKDOWN_CORE_TOKEN_NONE;
        return BRACKET_MATCHED;
    }
    made->kind = (uint16_t)(self->token_base + FOOTNOTE_NOTE);
    made->holder = MARKDOWN_CORE_NODE_NONE;
    return BRACKET_MATCHED;
}

static markdown_core_bracket_match S_end_call(const markdown_core_element_instance *self,
                                              markdown_core_tokenizer *tokenizer, markdown_core_opener *label,
                                              markdown_core_label_made *made) {
    const markdown_core_opener opener = *label;
    markdown_core_inline_state *inline_state = tokenizer->reader;
    const markdown_core_events *events = &tokenizer->events;
    const markdown_core_dialect *dialect = inline_state->dialect;
    const unsigned char *data = tokenizer->data;
    const uint32_t after = made->past;
    const size_t first = (size_t)opener.enter + 2;
    if (first >= opener.close) {
        return BRACKET_UNMATCHED;
    }
    /* The label's first token is text: what compile makes of it is a
     * Text node. */
    const markdown_core_token *token = markdown_core_event_token(events, first);
    const markdown_core_compile_ops *ops = token->kind < dialect->token_kinds ? dialect->compiles[token->kind] : NULL;
    if (!ops || !ops->plain) {
        return BRACKET_UNMATCHED;
    }
    /* THE CARET IS THE EVIDENCE: the source spells it. */
    const bool caret = opener.position < tokenizer->size && data[opener.position] == '^' &&
                       (token->end - token->start > 1 || first + 2 < opener.close);
    if (!caret) {
        return BRACKET_UNMATCHED;
    }
    const markdown_core_mention *definition = NULL;
    if (after - opener.position >= 2) {
        markdown_core_chunk text = {(unsigned char *)data + opener.position + 1,
                                    (bufsize_t)(after - opener.position - 2), 0};
        bool failed = false;
        definition = markdown_core_parser_find_definition(
            inline_state->owner_parser, self->spaces[FOOTNOTE_LABELS], &text,
            markdown_core_inline_state_root_offset(inline_state, (int)opener.position), &failed);
        if (failed) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    if (!definition) {
        /* A call whose label names no definition, which a cursor in it may
         * name: no later rule reads its label. */
        const markdown_core_field call = {MARKDOWN_CORE_SLOT_FOOTNOTE, MARKDOWN_CORE_KIND_CITATION,
                                          (bufsize_t)opener.position + 1, (bufsize_t)after - 1};
        markdown_core_text_give_up(tokenizer, MARKDOWN_CORE_KIND_CITE, (bufsize_t)opener.start,
                                   (bufsize_t)opener.position + 1, &call, 1);
        return BRACKET_UNMATCHED;
    }
    if (!markdown_core_attention_allows(tokenizer, MARKDOWN_CORE_NODE_NONE, MARKDOWN_CORE_NODE_CITE)) {
        return BRACKET_UNMATCHED;
    }
    /* The call's label, after its caret, without the whitespace before
     * its `]`. */
    uint32_t end = after - 1;
    while (end > opener.position + 1 && markdown_core_is_whitespace(data[end - 1])) {
        end--;
    }
    const markdown_core_chunk name = markdown_core_mention_name(definition);
    S_call call = {.start = opener.start, .position = opener.position, .end = end, .name_size = (uint32_t)name.len};
    call.name = markdown_core_events_keep(&tokenizer->events, name.data, (size_t)name.len + 1);
    const uint32_t kept = markdown_core_events_keep(&tokenizer->events, &call, sizeof(call));
    if (call.name == UINT32_MAX || kept == UINT32_MAX) {
        return BRACKET_REJECTED;
    }
    made->kind = (uint16_t)(self->token_base + FOOTNOTE_CALL);
    made->data = kept;
    made->from = opener.start + opener.shape->kept;
    made->holder = MARKDOWN_CORE_NODE_NONE;
    return BRACKET_MATCHED;
}

/* A call is a Cite of one Citation whose referent is a footnote, placed
 * from the cite's start `start` and the label's caret `position` up to
 * `after`, past the `]`. */
static markdown_core_node *S_cite(markdown_core_inline_state *inline_state, bufsize_t start, bufsize_t position,
                                  bufsize_t after) {
    markdown_core_node *cite = markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITE);
    markdown_core_node *citation =
        cite ? markdown_core_inline_make_simple_with_state(inline_state, MARKDOWN_CORE_NODE_CITATION) : NULL;
    if (!citation) {
        if (cite) {
            markdown_core_parser_release_node(inline_state->owner_parser, cite);
        }
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    markdown_core_scaffold_append(cite, citation);
    citation->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    markdown_core_inline_state_place(inline_state, cite, start, after - 1);
    markdown_core_inline_state_place(inline_state, citation, position, after - 2);
    return cite;
}

/* COMPILE: an inline note is a call whose citation's note holds the body;
 * a call is its cite alone. */
static void S_note_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_parser *parser = inline_state->owner_parser;
    const bufsize_t start = (bufsize_t)token->start, after = (bufsize_t)token->end;
    markdown_core_node *cite = S_cite(inline_state, start, start + 2, after);
    markdown_core_node *footnote =
        cite ? markdown_core_inline_make_simple(inline_state, MARKDOWN_CORE_NODE_FOOTNOTE) : NULL;
    if (!footnote) {
        if (cite) {
            markdown_core_parser_release_node(parser, cite);
        }
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return;
    }
    markdown_core_inline_state_place(inline_state, footnote, start, after - 1);
    /* The note is its Citation's own field, as the affixes are. */
    markdown_core_scaffold_first(cite)->as.citation->note = footnote;
    if (markdown_core_compile_append(compiler, cite)) {
        markdown_core_compile_push(compiler, footnote);
    }
}

static void S_note_close(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_parser *parser = inline_state->owner_parser;
    const markdown_core_element_instance *self = markdown_core_compile_self(compiler);
    markdown_core_node *footnote = markdown_core_compile_pop(compiler);
    markdown_core_inline_settle_note(parser, inline_state, footnote);
    /* The note is a footnote of no label, which the document lists from
     * where it starts: its place is in its root's content. */
    const markdown_core_chunk none = {0};
    if (!parser->error) {
        markdown_core_parser_declare(parser, self->spaces[FOOTNOTE_LABELS], &none,
                                     markdown_core_parser_source_at(parser, footnote->place.start), NULL, footnote);
    }
}

static void S_call_made(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)index;
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    const S_call *call = markdown_core_events_kept(compiler->events, token->data);
    markdown_core_node *cite =
        S_cite(inline_state, (bufsize_t)call->start, (bufsize_t)call->position, (bufsize_t)token->end);
    if (!cite) {
        compiler->failed = true;
        return;
    }
    markdown_core_node *citation = markdown_core_scaffold_first(cite);
    const markdown_core_field_to fields = markdown_core_inline_fields(inline_state, citation);
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_FOOTNOTE, (bufsize_t)call->position + 1, (bufsize_t)call->end);
    /* The call's label is its normal form: the key the lookup matched. */
    unsigned char *id = markdown_core_alloc(1, (size_t)call->name_size + 1);
    if (!id) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        markdown_core_parser_release_node(inline_state->owner_parser, cite);
        compiler->failed = true;
        return;
    }
    memcpy(id, markdown_core_events_kept(compiler->events, call->name), (size_t)call->name_size);
    id[call->name_size] = '\0';
    citation->as.citation->value = (markdown_core_chunk){id, (bufsize_t)call->name_size, 1};
    markdown_core_compile_append(compiler, cite);
}

static const markdown_core_text_construct FOOTNOTE_CONSTRUCTS[] = {{"^", MARKDOWN_CORE_LABEL_CONSTRUCT(S_start)}};
static const markdown_core_compile_ops FOOTNOTE_TOKENS[FOOTNOTE_KINDS] = {
    [FOOTNOTE_NOTE] = {.enter = S_note_open, .exit = S_note_close, .asked = true},
    [FOOTNOTE_CALL] = {.enter = S_call_made, .opaque = true, .asked = true}};

static const markdown_core_close_rule FOOTNOTE_CLOSES[] = {
    {.precedence = 0, .mine = true, .end = S_end_note},
    {.precedence = 20, .end = S_end_call},
};

static const markdown_core_inline_ops FOOTNOTE_INLINES = {
    .constructs = FOOTNOTE_CONSTRUCTS,
    .construct_count = sizeof(FOOTNOTE_CONSTRUCTS) / sizeof(FOOTNOTE_CONSTRUCTS[0]),
    .tokens = FOOTNOTE_TOKENS,
    .token_count = FOOTNOTE_KINDS,
    .closes = FOOTNOTE_CLOSES,
    .close_count = sizeof(FOOTNOTE_CLOSES) / sizeof(FOOTNOTE_CLOSES[0]),
    .closer = ']',
};

static bool markdown_core_footnote_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        markdown_core_node **container, markdown_core_reader *reader,
                                        block_start *start) {
    bufsize_t matched = start->matched;

    markdown_core_chunk c = {(unsigned char *)markdown_core_reader_bytes(reader, reader->nonspace + 2), matched - 2, 0};

    while (c.data[c.len - 1] != ']') {
        --c.len;
    }
    --c.len;

    if (!markdown_core_chunk_to_cstr(&c)) {
        /* The label would keep borrowing the transient line buffer. */
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }

    const bufsize_t label = reader->nonspace + 2;
    markdown_core_reader_advance(reader, reader->nonspace + matched - reader->offset, false);
    /* THE ANCHOR RULE (§5.1): a definition is a block node at the byte
     * where its OPENING BRACKET was written. It used to start at the
     * byte after `[^label]:`, which is a column that need not exist --
     * `[^footnote]:` alone on a line is twelve bytes and the definition
     * began at column 13. Every other block in this engine starts at
     * its own first byte and the marker is inside it; a footnote
     * definition was the one that started after its own marker. */
    *container = markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE, reader->nonspace + 1);
    if (!*container) {
        markdown_core_chunk_free(&c);
        return false;
    }
    /* The label is in the reference-label normal form and WITHOUT a caret
     * (M4): the key every call's referent names. The
     * caret that kept a footnote apart from a link definition in a
     * consumer's single map went with the association -- a
     * `Footnote` and a resolved `Link` are different values now. */
    markdown_core_node *footnote = *container;
    const markdown_core_field_to fields = {parser, footnote, NULL, reader, 0, NULL};
    markdown_core_field_add(&fields, MARKDOWN_CORE_SLOT_FOOTNOTE, label, label + c.len);
    markdown_core_chunk normal = {0};
    /* The document defines this label from here on.
     *
     * Registered where the label is READ, which is here. Whether it is
     * registered at open or at close is NOT observable and that was
     * measured, not assumed: moving this call into `markdown_core_block_close` leaves
     * every suite and every oracle green. It used to matter, and the
     * reason it stopped is the shape rather than the timing -- the map
     * this replaced held a NODE per entry and used registration order
     * as the tie-break for a repeated label, so on EXIT a definition
     * nested inside another closed first, won the label, and the outer
     * one was freed with everything written in it (D11). A set of
     * labels owns no node and picks no winner, so order decides
     * nothing left to get wrong. */
    const bool declared = markdown_core_parser_declare(parser, self->spaces[FOOTNOTE_LABELS], &c, footnote->place.start,
                                                       &normal, footnote);
    if (normal.len) {
        footnote->as.footnote->label = markdown_core_optional_chunk_present(normal);
    }
    if (!declared || !normal.len) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_chunk_free(&c);
        return false;
    }
    markdown_core_chunk_free(&c);

    (*container)->internal_offset = matched;
    return true;
}

enum { FOOTNOTE_OPEN, FOOTNOTE_BLOCKS };

static void S_footnote_open(markdown_core_compiler *compiler, const markdown_core_token *token, uint32_t index) {
    (void)token;
    (void)index;
    markdown_core_block_turn *turn = markdown_core_block_turned(compiler);
    turn->opened = markdown_core_footnote_open(markdown_core_compile_self(compiler), turn->parser, turn->container,
                                               turn->context->reader, turn->start);
}

static markdown_core_state markdown_core_footnote_scan(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    block_start_context *context = markdown_core_block_turn_of(tokenizer)->context;
    block_start *start = markdown_core_block_turn_of(tokenizer)->start;
    markdown_core_reader *reader = context->reader;
    int first = reader->nonspace;
    if (!(context->depth < MAX_FOOTNOTE_DEPTH &&
          (start->matched = scan_footnote_definition(markdown_core_reader_rest(reader, 0),
                                                     markdown_core_reader_length(reader), first)))) {
        return markdown_core_nok(tokenizer);
    }
    start->kind = MARKDOWN_CORE_NODE_FOOTNOTE;
    return markdown_core_block_found(tokenizer, FOOTNOTE_OPEN);
}

static const markdown_core_block_construct FOOTNOTE_STARTS[] = {
    {.bytes = "[", .indent = 3, .construct = {.start = markdown_core_footnote_scan}},
};

static const markdown_core_compile_ops FOOTNOTE_BLOCK_TOKENS[FOOTNOTE_BLOCKS] = {
    [FOOTNOTE_OPEN] = {.enter = S_footnote_open},
};

static const markdown_core_open_ops FOOTNOTE_OPENS = {
    .starts = FOOTNOTE_STARTS,
    .start_count = 1,
    .tokens = FOOTNOTE_BLOCK_TOKENS,
    .token_count = FOOTNOTE_BLOCKS,
};

/* THE CONTINUATION (element.h, markdown_core_line_ops `continuation`). */
static markdown_core_state S_continue(markdown_core_tokenizer *tokenizer, int code) {
    (void)code;
    markdown_core_block_turn *turn = markdown_core_block_turn_of(tokenizer);
    bool taken = false;
    if (!continue_container(markdown_core_self(tokenizer), turn->parser, turn->node, turn->reader, turn->joining,
                            &taken)) {
        return markdown_core_nok(tokenizer);
    }
    return taken ? markdown_core_block_continued(tokenizer, MARKDOWN_CORE_TOKEN_TAKE) : markdown_core_ok(tokenizer);
}

static const markdown_core_line_ops FOOTNOTE_LINES = {
    .continuation = {.start = S_continue},

};

const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE = {
    .edit = &FOOTNOTE_EDIT,
    .spaces = FOOTNOTE_SPACES,
    .name = "footnote",
    .inlines = &FOOTNOTE_INLINES,
    .opens = &FOOTNOTE_OPENS,
    .lines = &FOOTNOTE_LINES,
};

bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_reader *reader) {
    return markdown_core_reader_indented(reader, 4, true);
}

/* A footnote definition's record (markdown_core_record_ops): the label it
 * declares, in the pool's storage, which a view copies. */
static void free_footnote(markdown_core_node_pool *pool, markdown_core_node *node) {
    if (node->as.footnote->label.has_value) {
        markdown_core_node_pool_bytes_free(pool, node->as.footnote->label.value.data);
    }
}

const markdown_core_record_ops MARKDOWN_CORE_RECORD_FOOTNOTE = {
    .size = sizeof(markdown_core_footnote_value),
    .free = free_footnote,
};
