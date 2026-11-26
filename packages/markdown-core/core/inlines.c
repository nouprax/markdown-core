#include "alloc.h"
#include "inline_internal.h"
#include "attention.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "markdown_core_ctype.h"
#include "config.h"
#include "node.h"
#include "parser.h"
#include "node_type.h"
#include "buffer.h"
#include "utf8.h"
#include "delimiter.h"
#include "inlines.h"
#include "label.h"
#include "element.h"

/* The element API's placement: the inline fast path (inline_internal.h),
 * behind a call, for the makers outside this translation unit. */
void markdown_core_inline_state_place(markdown_core_inline_state *inline_state, markdown_core_node *node, int from,
                                      int to) {
    markdown_core_inline_place(inline_state, node, from, to);
}

/* A placement that leaves the frame -- the next line's first token, an
 * emphasis closed across lines, a link placed back at its opener, an end
 * the caller left unresolved -- is answered by the span, which probes from
 * the cursor and moves it to where the node ends; the frame is then read
 * from the run the cursor moved to. The fallback of the old arithmetic is
 * gone: every content-bearing block has a map by the time its inlines are
 * parsed (`markdown_core_inline_start_inlines` gives one to any block whose
 * content was SET rather than fed), and a state with no map places nothing
 * (`mapped`, tested by the fast path before it comes here). */
void markdown_core_inline_place_outside_frame(markdown_core_inline_state *inline_state, markdown_core_node *node,
                                              int from, int to) {
    markdown_core_content_span span;
    markdown_core_parser_content_span(inline_state->owner_parser,
                                      inline_state->owner ? &inline_state->owner->content_map : NULL, from, to, &span,
                                      &inline_state->mark_cursor);
    markdown_core_inline_seat_cursor(inline_state);
    if (span.has_start) {
        node->place.start = (uint32_t)span.start;
    }
    if (span.has_end) {
        node->place.end = (uint32_t)span.end;
    }
    if (node->kind == inline_state->dialect->plain && node->as.literal->len > 0) {
        /* A Text placed with an end unresolved keeps no map at all: the
         * writes stay inside the verbatim gate, and a partial span names no
         * slice to take. */
        if (span.has_start && span.has_end) {
            markdown_core_inline_map_text(inline_state, node, from, to, span.first, span.last);
        } else {
            const markdown_core_chunk *literal = node->as.literal;
            if (!(literal->len == to - from + 1 &&
                  (literal->data == inline_state->input.data + from ||
                   memcmp(literal->data, inline_state->input.data + from, (size_t)literal->len) == 0))) {
                node->content_map.count = 0;
                node->content_map.offset = 0;
                markdown_core_parser_append_content_mark(inline_state->owner_parser, node, 0, inline_state->mark_line,
                                                         node->place.start, (int)(node->place.end - node->place.start),
                                                         0);
            }
        }
    }
}

// Create an inline with a literal string value. Inlined into the delimiter
// match, whose every run makes one; the extern form serves the elements.
static inline MARKDOWN_CORE_ATTRIBUTE((always_inline))
    markdown_core_node *S_make_literal(markdown_core_inline_state *inline_state, markdown_core_node_type t,
                                       int start_column, int end_column, markdown_core_chunk s) {
    markdown_core_node *e = markdown_core_parser_make_node(inline_state->owner_parser, t);
    if (!e) {
        /* Frees an owned literal; borrowed chunks only reset fields. */
        markdown_core_chunk_free(&s);
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    *e->as.literal = s;
    markdown_core_inline_place(inline_state, e, start_column, end_column);
    return e;
}

markdown_core_node *markdown_core_inline_make_literal(markdown_core_inline_state *inline_state,
                                                      markdown_core_node_type t, int start_column, int end_column,
                                                      markdown_core_chunk s) {
    return S_make_literal(inline_state, t, start_column, end_column, s);
}

/* Records the kind it creates. Every parse-time caller reaches the parser
 * through its inline state; the mem-only form above stays for callers that
 * have no parse at all. */
markdown_core_node *markdown_core_inline_make_simple(markdown_core_inline_state *inline_state,
                                                     markdown_core_node_type t) {
    return markdown_core_parser_make_node(inline_state->owner_parser, t);
}

/* markdown_core_inline_make_simple with the inline state's loss flag for handlers that consume input
 * before creating the node. */
markdown_core_node *markdown_core_inline_make_simple_with_state(markdown_core_inline_state *inline_state,
                                                                markdown_core_node_type t) {
    markdown_core_node *e = markdown_core_inline_make_simple(inline_state, t);
    if (!e) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    return e;
}

/* A run's records, one block of the dialect's run-state size: a
 * block a finished run gave back, or a new one, zeroed either way. A dialect
 * whose elements own no inline state gives a run no block. */
typedef struct markdown_core_inline_record {
    struct markdown_core_inline_record *next;
} markdown_core_inline_record;

static unsigned char *S_take_inline_records(markdown_core_parser *parser, size_t size) {
    markdown_core_inline_record *record = parser->free_inline_records;
    if (!record) {
        return markdown_core_alloc(1, size);
    }
    parser->free_inline_records = record->next;
    memset(record, 0, size);
    return (unsigned char *)record;
}

static void S_give_inline_records(markdown_core_parser *parser, unsigned char *records) {
    markdown_core_inline_record *record = (markdown_core_inline_record *)(void *)records;
    record->next = parser->free_inline_records;
    parser->free_inline_records = record;
}

void markdown_core_inline_release_records(markdown_core_parser *parser) {
    while (parser->free_inline_records) {
        markdown_core_inline_record *record = parser->free_inline_records;
        parser->free_inline_records = record->next;
        markdown_core_free(record);
    }
}

/* Whether the run began: a run whose records could not be allocated never
 * calls a lifecycle hook, since every one of them reads its records. */
static bool S_inline_run_began(const markdown_core_inline_state *inline_state) {
    return inline_state->run_state || !inline_state->dialect->run_state_size;
}

void markdown_core_inline_state_from_buf(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                         markdown_core_chunk *chunk) {
    memset(inline_state, 0, sizeof(*inline_state));
    inline_state->input = *chunk;
    inline_state->cut = -1;
    inline_state->owner_parser = parser;
    /* No link opens before a label does (label.h). */
    inline_state->no_link_openers = true;
    /* A run that lists no settle starts holds everything it reads
     * undecided (`undecided` 0): nothing asks it where its end reads
     * began. */
    if (parser) {
        inline_state->dialect = parser->dialect;
        size_t records = parser->dialect->run_state_size;
        if (records) {
            inline_state->run_state = S_take_inline_records(parser, records);
            if (!inline_state->run_state) {
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                return;
            }
        }
        const markdown_core_element_instance *const *owners =
            parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_INIT];
        size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_INIT];
        for (size_t i = 0; i < count; i++) {
            parser->inline_hook_work++;
            owners[i]->element->inlines->init(owners[i], inline_state);
        }
    }
}

/* The byte `n` past the cursor, or 0 past the end. A NUL of the text is a
 * byte like any other, so 0 says no more than the byte: whether the input
 * ended is the cursor's (markdown_core_inline_is_eof). */
unsigned char markdown_core_inline_peek_char_n(markdown_core_inline_state *inline_state, bufsize_t n) {
    if (inline_state->pos + n < inline_state->input.len) {
        return inline_state->input.data[inline_state->pos + n];
    }
    markdown_core_inline_saw_end(inline_state);
    return 0;
}

unsigned char markdown_core_inline_peek_char(markdown_core_inline_state *inline_state) {
    return markdown_core_inline_peek_char_n(inline_state, 0);
}

unsigned char markdown_core_inline_peek_at(markdown_core_inline_state *inline_state, bufsize_t pos) {
    return inline_state->input.data[pos];
}

// Return true if there are more characters in the inline state.
int markdown_core_inline_is_eof(markdown_core_inline_state *inline_state) {
    if (inline_state->pos < inline_state->input.len) {
        return 0;
    }
    markdown_core_inline_saw_end(inline_state);
    return 1;
}

// Advance the inline state.  Doesn't check for eof.
#define advance(inline_state) (inline_state)->pos += 1

bool markdown_core_inline_skip_spaces(markdown_core_inline_state *inline_state) {
    bool skipped = false;
    while (markdown_core_is_space_or_tab(markdown_core_inline_peek_char(inline_state))) {
        advance(inline_state);
        skipped = true;
    }
    return skipped;
}

bool markdown_core_inline_skip_line_end(markdown_core_inline_state *inline_state) {
    bool seen_line_end_char = false;
    if (markdown_core_inline_peek_char(inline_state) == '\r') {
        advance(inline_state);
        seen_line_end_char = true;
    }
    if (markdown_core_inline_peek_char(inline_state) == '\n') {
        advance(inline_state);
        seen_line_end_char = true;
    }
    return seen_line_end_char || markdown_core_inline_is_eof(inline_state);
}

/* A token crossed into the owner's cut (element.h, markdown_core_follow,
 * `yields`): the follower gives it back. */
static void S_cut_crossed(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    const markdown_core_kind_record *kind = markdown_core_parser_block_kind(parser, inline_state->owner);
    inline_state->cut = -1;
    kind->follow[MARKDOWN_CORE_FOLLOW_TAIL]->undo(kind->follower[MARKDOWN_CORE_FOLLOW_TAIL], parser,
                                                  inline_state->owner);
}

/* Parse an inline, advancing inline state, and add it as a child of the
 * state's OWNER. The owner used to be passed in alongside the state, and both
 * callers passed the same node on every iteration of their loop -- so the
 * owner's structural element, a pure function of its kind, was re-derived once
 * per inline token. It is resolved once now, where the owner is set. */
void markdown_core_inline_start_inlines(markdown_core_parser *parser, markdown_core_node *parent, bool root,
                                        markdown_core_inline_state *inline_state) {
    markdown_core_chunk content = markdown_core_node_text(parent);
    /* EVERY content-bearing block has a map by the time its inlines are parsed.
     * One the parser fed line by line already does; one whose content was SET
     * -- a table cell, a directive's label -- gets one mark here, derived from
     * where the block says it starts. That derivation IS the arithmetic this
     * replaces: `start + internal_offset` was the block offset every inline
     * position used to be measured from, and stating it once as a mark is
     * what lets the term itself go. */
    if (parent->content_map.count == 0) {
        markdown_core_parser_mark_content(parser, parent, 0, parent->place.start + parent->internal_offset);
    }
    /* A cut its inlines may give back (element.h, markdown_core_follow,
     * `yields`) is read with the rest of the text. */
    const markdown_core_follow *tail =
        root && parent->cut_back ? markdown_core_parser_block_kind(parser, parent)->follow[MARKDOWN_CORE_FOLLOW_TAIL]
                                 : NULL;
    const bool held = tail && tail->yields;
    bufsize_t text = content.len;
    if (held) {
        content.len += parent->cut_back;
    }
    markdown_core_inline_state_from_buf(parser, inline_state, &content);
    /* Block buffers include their terminating line ending. An inline field
     * ends at its owner's delimiter: its trailing spaces are body content. */
    if (!MARKDOWN_CORE_NODE_TYPE_INLINE_P(parent->kind)) {
        markdown_core_chunk_rtrim(&inline_state->input);
    }
    if (held) {
        markdown_core_chunk front = {content.data, text, 0};
        markdown_core_chunk_rtrim(&front);
        text = front.len;
    }
    /* A root's nodes are placed in its content: the bytes this parse reads
     * keep their map to the source as the root's runs. A field's nodes are
     * placed in the content of the root it was cut from, which its map is a
     * slice of. */
    if (root) {
        markdown_core_parser_read_content(parser, parent, inline_state->input.len);
    }
    inline_state->owner = parent;
    inline_state->owner_structure = markdown_core_parser_structure(parser, parent);
    inline_state->mark_cursor = parent->content_map.first;
    markdown_core_inline_seat_cursor(inline_state);

    const markdown_core_element_instance *structure = inline_state->owner_structure;
    if (structure && structure->element->inlines && structure->element->inlines->enter &&
        S_inline_run_began(inline_state)) {
        const bufsize_t len = inline_state->input.len;
        if (held) {
            inline_state->input.len = text;
        }
        structure->element->inlines->enter(structure, parser, inline_state, parent);
        if (held) {
            text = inline_state->input.len;
            inline_state->input.len = len;
        }
    }
    if (held) {
        inline_state->cut = text;
    }
}

static void S_fields_drop(markdown_core_inline_state *inline_state, size_t from);
void markdown_core_inline_clear_inlines(markdown_core_inline_state *inline_state) {
    markdown_core_parser *parser = inline_state->owner_parser;
    if (S_inline_run_began(inline_state)) {
        const markdown_core_element_instance *const *owners =
            parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
        size_t dispose_count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
        for (size_t i = 0; i < dispose_count; i++) {
            parser->inline_hook_work++;
            owners[i]->element->inlines->dispose(owners[i], inline_state);
        }
    }
    if (inline_state->run_state) {
        S_give_inline_records(parser, inline_state->run_state);
        inline_state->run_state = NULL;
    }
    if (inline_state->reads) {
        S_fields_drop(inline_state, 0);
        markdown_core_free(inline_state->reads);
        inline_state->reads = NULL;
        inline_state->read_room = 0;
    }
    if (inline_state->error) {
        markdown_core_parser_fail(parser, inline_state->error);
    }
}

/* The token start the run reads next listed as one a settle point may be. */
static void S_settle_start(markdown_core_inline_state *inline_state) {
    markdown_core_inline_settle *settle = inline_state->settle;
    bufsize_t *starts = markdown_core_reserve(settle->starts, &settle->capacity, settle->count + 1, sizeof(*starts));
    if (!starts) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    settle->starts = starts;
    starts[settle->count++] = inline_state->pos;
}

void markdown_core_inline_settle_note(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                      markdown_core_node *note) {
    markdown_core_parser_settle_field(parser, note);
    markdown_core_inline_settle *settle = inline_state->settle;
    if (!settle) {
        return;
    }
    markdown_core_node **notes =
        markdown_core_reserve(settle->notes, &settle->note_capacity, settle->note_count + 1, sizeof(*notes));
    if (!notes) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return;
    }
    settle->notes = notes;
    notes[settle->note_count++] = note;
}

/* THE OPEN CONSTRUCT a run gave up (node.h, OPENER and OPEN): its opener
 * and the fields it wrote, kept on the root whose run reads it, placed in
 * its content as the run's own nodes are, which publishing places in the
 * source (ast.c, publish_fields). */
static void S_inline_open(markdown_core_inline_state *inline_state, markdown_core_node_kind opens, bufsize_t from,
                          bufsize_t to, const markdown_core_field *fields, size_t count, bool line) {
    const markdown_core_field_to target = markdown_core_inline_fields(inline_state, inline_state->owner);
    markdown_core_field_opener(&target, opens, line, from, to, fields, count);
}

void markdown_core_inline_open(markdown_core_inline_state *inline_state, markdown_core_node_kind opens, bufsize_t from,
                               bufsize_t to, const markdown_core_field *fields, size_t count) {
    S_inline_open(inline_state, opens, from, to, fields, count, false);
}

static bool S_read(markdown_core_parser *parser, markdown_core_inline_state *inline_state, bool halts);

/* What a run does once its last token is read: when it lists settle
 * starts, the earliest undecided start is its settle's; then it is let go. */
static void S_finish_run(markdown_core_inline_state *inline_state, bool settle) {
    if (settle) {
        inline_state->settle->undecided = inline_state->undecided;
    }
    markdown_core_inline_clear_inlines(inline_state);
}

/* THE READING OF A RUN (plan symbols-cleanroom.md 23): the run's content
 * is tokenized into events, which the constructs' resolvers rewrite, and
 * compile makes the run's nodes of them, in order. A PASS is the
 * tokenizer and the compiler of one reading; a reading inside a reading
 * (a field's) takes its own. */
markdown_core_pass *markdown_core_pass_take(markdown_core_parser *parser) {
    markdown_core_pass *pass = parser->free_passes;
    if (pass) {
        parser->free_passes = pass->next;
        return pass;
    }
    pass = markdown_core_alloc(1, sizeof(*pass));
    if (pass) {
        markdown_core_tokenizer_init(&pass->tokenizer);
        markdown_core_compiler_init(&pass->compiler);
    }
    return pass;
}

void markdown_core_pass_back(markdown_core_parser *parser, markdown_core_pass *pass) {
    pass->next = parser->free_passes;
    parser->free_passes = pass;
}

void markdown_core_inline_release_passes(markdown_core_parser *parser) {
    while (parser->free_passes) {
        markdown_core_pass *pass = parser->free_passes;
        parser->free_passes = pass->next;
        markdown_core_tokenizer_free(&pass->tokenizer);
        markdown_core_compiler_free(&pass->compiler);
        markdown_core_free(pass);
    }
}

/* THE SEAMS OF A READING (plan 23.6): at a start of the text at which
 * nothing is held, the reading meets the nearest old seam when the start
 * is the seam's and the last token before it is not text and ends where
 * the old child before the seam did. Otherwise the start is listed as one
 * a settle point may be. */
static bool S_start(markdown_core_tokenizer *tokenizer, void *context) {
    markdown_core_inline_state *inline_state = context;
    const bufsize_t pos = (bufsize_t)tokenizer->position;
    markdown_core_seam_stops *stops = inline_state->stops;
    if (stops) {
        while (stops->count && stops->end - stops->items[stops->count - 1].offset < pos) {
            stops->count--;
        }
        const markdown_core_events *events = &tokenizer->events;
        if (stops->count && stops->end - stops->items[stops->count - 1].offset == pos && events->count) {
            const markdown_core_token *last = markdown_core_event_token(events, events->count - 1);
            const markdown_core_compile_ops *ops =
                last->kind < inline_state->dialect->token_kinds ? inline_state->dialect->compiles[last->kind] : NULL;
            if (ops && !ops->plain && (bufsize_t)last->end + (bufsize_t)stops->items[stops->count - 1].lead == pos) {
                stops->met = true;
                return false;
            }
        }
    }
    inline_state->pos = pos;
    S_settle_start(inline_state);
    return !inline_state->error;
}

/* What the reading's constructs gave up, kept on the run's node in the
 * order they gave it up (node.h, OPENER). */
static void S_give_ups(markdown_core_inline_state *inline_state, const markdown_core_tokenizer *tokenizer) {
    for (size_t at = 0; at < tokenizer->note_count; at++) {
        const markdown_core_note *note = &tokenizer->notes[at];
        if (note->gone) {
            continue;
        }
        markdown_core_field local[MARKDOWN_CORE_GIVE_UP_FIELDS];
        const uint32_t *words = tokenizer->words + note->first;
        const size_t count = note->count / 4;
        markdown_core_field *fields =
            count <= MARKDOWN_CORE_GIVE_UP_FIELDS ? local : markdown_core_alloc(count, sizeof(*fields));
        if (!fields) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
            return;
        }
        for (size_t i = 0; i < count; i++) {
            fields[i] =
                (markdown_core_field){(markdown_core_slot_kind)words[4 * i], (markdown_core_node_kind)words[4 * i + 1],
                                      (bufsize_t)words[4 * i + 2], (bufsize_t)words[4 * i + 3]};
        }
        markdown_core_inline_open(inline_state, (markdown_core_node_kind)note->token.kind, (bufsize_t)note->token.start,
                                  (bufsize_t)note->token.end, fields, count);
        if (fields != local) {
            markdown_core_free(fields);
        }
    }
}

/* A FIELD'S READING (markdown_core_text_field): the pass that read it,
 * the root it reads, and its run. */
struct markdown_core_field_read {
    markdown_core_pass *pass;
    markdown_core_node *root;
    markdown_core_inline_state state;
};

/* The fields the run read from `from` on, compile having taken none of
 * them, let go. */
static void S_fields_drop(markdown_core_inline_state *inline_state, size_t from) {
    markdown_core_parser *parser = inline_state->owner_parser;
    for (size_t i = from; i < inline_state->read_count; i++) {
        markdown_core_field_read *read = inline_state->reads[i];
        if (read) {
            markdown_core_pass_back(parser, read->pass);
            markdown_core_inline_clear_inlines(&read->state);
            markdown_core_parser_release_node(parser, read->root);
            markdown_core_free(read);
        }
    }
    inline_state->read_count = from;
}

/* The elements' run records let go and zeroed, as a run's begin. */
static void S_records_clear(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    if (!inline_state->run_state) {
        return;
    }
    const markdown_core_element_instance *const *owners =
        parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
    const size_t count = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_DISPOSE];
    for (size_t i = 0; i < count; i++) {
        owners[i]->element->inlines->dispose(owners[i], inline_state);
    }
    memset(inline_state->run_state, 0, parser->dialect->run_state_size);
    owners = parser->dialect->inline_hooks[MARKDOWN_CORE_INLINE_HOOK_INIT];
    const size_t inits = parser->dialect->inline_hook_counts[MARKDOWN_CORE_INLINE_HOOK_INIT];
    for (size_t i = 0; i < inits; i++) {
        owners[i]->element->inlines->init(owners[i], inline_state);
    }
}

/* Reads the run from its cursor to its end, or to the seam it meets; a
 * reading that `halts` and does leaves the run as it was and answers
 * false (markdown_core_inline_settles). */
static bool S_read(markdown_core_parser *parser, markdown_core_inline_state *inline_state, bool halts) {
    markdown_core_pass *pass = markdown_core_pass_take(parser);
    if (!pass) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return true;
    }
    /* What the reading changes of the run before it is compiled. */
    const markdown_core_inline_state saved = *inline_state;
    const markdown_core_read read = {.data = inline_state->input.data,
                                     .size = (uint32_t)inline_state->input.len,
                                     .from = (uint32_t)inline_state->pos,
                                     .cut = inline_state->cut >= 0 ? (uint32_t)inline_state->cut : UINT32_MAX,
                                     .text = &inline_state->dialect->text,
                                     .reader = inline_state,
                                     .records = inline_state->run_state,
                                     .start = inline_state->settle ? S_start : NULL,
                                     .context = inline_state,
                                     .halts = halts};
    markdown_core_tokenizer *tokenizer = &pass->tokenizer;
    const bool read_ok = markdown_core_tokenize(tokenizer, &read);
    parser->rewound_work += tokenizer->rewound;
    tokenizer->rewound = 0;
    if (!read_ok) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    } else if (tokenizer->halted) {
        /* The fields it read are let go, the list itself kept, and the
         * elements' records are as the run began. */
        S_fields_drop(inline_state, saved.read_count);
        S_records_clear(parser, inline_state);
        markdown_core_inline_state run = saved;
        run.reads = inline_state->reads;
        run.read_room = inline_state->read_room;
        *inline_state = run;
        markdown_core_pass_back(parser, pass);
        return false;
    }
    if (tokenizer->undecided != UINT32_MAX && (bufsize_t)tokenizer->undecided < inline_state->undecided) {
        inline_state->undecided = (bufsize_t)tokenizer->undecided;
    }
    const bufsize_t end = tokenizer->stopped != UINT32_MAX ? (bufsize_t)tokenizer->stopped : inline_state->input.len;
    if (!inline_state->error) {
        S_give_ups(inline_state, tokenizer);
    }
    if (!inline_state->error &&
        !markdown_core_compile(&pass->compiler, &tokenizer->events, inline_state->input.data,
                               inline_state->dialect->compiles, inline_state->dialect->token_kinds, inline_state)) {
        if (!inline_state->error) {
            inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    inline_state->pos = end;
    if (tokenizer->crossed && inline_state->cut >= 0 && !inline_state->error) {
        S_cut_crossed(parser, inline_state);
    }
    markdown_core_pass_back(parser, pass);
    return true;
}

bool markdown_core_inline_settles(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    return parser->error || inline_state->error || S_read(parser, inline_state, true);
}

uint32_t markdown_core_text_field(markdown_core_tokenizer *tokenizer, markdown_core_node *root, bool *spaced) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    markdown_core_parser *parser = inline_state->owner_parser;
    *spaced = false;
    /* A field is read once the document is known, as its token is. */
    if (tokenizer->halts) {
        markdown_core_halt(tokenizer);
        return UINT32_MAX;
    }
    markdown_core_field_read **reads = markdown_core_reserve(inline_state->reads, &inline_state->read_room,
                                                             inline_state->read_count + 1, sizeof(*reads));
    if (reads) {
        /* Kept at once: a reserve that moved them freed where they were. */
        inline_state->reads = reads;
    }
    markdown_core_field_read *read = reads ? markdown_core_alloc(1, sizeof(*read)) : NULL;
    markdown_core_pass *pass = read ? markdown_core_pass_take(parser) : NULL;
    if (!pass) {
        markdown_core_free(read);
        tokenizer->events.failed = true;
        return UINT32_MAX;
    }
    read->pass = pass;
    markdown_core_inline_start_inlines(parser, root, false, &read->state);
    read->root = root;
    markdown_core_inline_state *state = &read->state;
    reads[inline_state->read_count] = read;
    if (state->error) {
        /* Its records were not had: the reading ends. */
        tokenizer->events.failed = true;
        return (uint32_t)inline_state->read_count++;
    }
    const markdown_core_read text = {.data = state->input.data,
                                     .size = (uint32_t)state->input.len,
                                     .from = (uint32_t)state->pos,
                                     .cut = UINT32_MAX,
                                     .text = &state->dialect->text,
                                     .reader = state,
                                     .records = state->run_state,
                                     .context = state};
    if (!markdown_core_tokenize(&pass->tokenizer, &text)) {
        state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
    }
    parser->rewound_work += pass->tokenizer.rewound;
    pass->tokenizer.rewound = 0;
    *spaced = markdown_core_attention_spaced(&pass->tokenizer);
    return (uint32_t)inline_state->read_count++;
}

markdown_core_node *markdown_core_compile_field(markdown_core_compiler *compiler, uint32_t index) {
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_field_read *read = index < inline_state->read_count ? inline_state->reads[index] : NULL;
    if (!read) {
        return NULL;
    }
    inline_state->reads[index] = NULL;
    markdown_core_parser *parser = inline_state->owner_parser;
    markdown_core_inline_state *state = &read->state;
    markdown_core_tokenizer *tokenizer = &read->pass->tokenizer;
    if (!state->error) {
        S_give_ups(state, tokenizer);
    }
    if (!state->error && !markdown_core_compile(&read->pass->compiler, &tokenizer->events, state->input.data,
                                                state->dialect->compiles, state->dialect->token_kinds, state)) {
        if (!state->error) {
            state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        }
    }
    state->pos = state->input.len;
    markdown_core_pass_back(parser, read->pass);
    S_finish_run(state, false);
    markdown_core_node *root = read->root;
    markdown_core_free(read);
    return root;
}

bool markdown_core_text_give_up(markdown_core_tokenizer *tokenizer, markdown_core_node_kind opens, bufsize_t from,
                                bufsize_t to, const markdown_core_field *fields, size_t count) {
    uint32_t local[4 * MARKDOWN_CORE_GIVE_UP_FIELDS] = {0};
    uint32_t *words = count <= MARKDOWN_CORE_GIVE_UP_FIELDS ? local : markdown_core_alloc(4 * count, sizeof(*words));
    if (!words) {
        tokenizer->events.failed = true;
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        words[4 * i] = (uint32_t)fields[i].slot;
        words[4 * i + 1] = (uint32_t)fields[i].owner;
        words[4 * i + 2] = (uint32_t)fields[i].start;
        words[4 * i + 3] = (uint32_t)fields[i].stop;
    }
    const bool kept =
        markdown_core_note_push(tokenizer, (uint16_t)opens, (uint32_t)from, (uint32_t)to, 0, words, 4 * count);
    if (words != local) {
        markdown_core_free(words);
    }
    return kept;
}

void markdown_core_text_made(markdown_core_tokenizer *tokenizer, markdown_core_node_type kind) {
    markdown_core_inline_state *inline_state = tokenizer->reader;
    inline_state->made_kind = kind;
    inline_state->made_end = (bufsize_t)tokenizer->position;
    if (inline_state->made_end >= (bufsize_t)tokenizer->size && tokenizer->cut == UINT32_MAX &&
        markdown_core_dialect_kind(inline_state->dialect, kind)->follow[MARKDOWN_CORE_FOLLOW_TOKEN]) {
        markdown_core_met_end(tokenizer);
    }
}

bool markdown_core_compile_append(markdown_core_compiler *compiler, markdown_core_node *node) {
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_node *parent = markdown_core_compile_parent(compiler);
    if (!node) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        compiler->failed = true;
        return false;
    }
    /* Fixed inline owners admit every token their grammar constructs. Only
     * a dynamic policy needs a decision here. */
    const bool asked = compiler->current && compiler->current->asked;
    if (!asked && markdown_core_node_allows(parent) &&
        !markdown_core_node_can_contain_type(parent, (markdown_core_node_type)node->kind)) {
        markdown_core_parser_release_node(inline_state->owner_parser, node);
        if (!inline_state->error) {
            inline_state->error = MARKDOWN_CORE_PARSE_CONTAINMENT_REJECTED;
        }
        compiler->failed = true;
        return false;
    }
    markdown_core_scaffold_append(parent, node);
    return true;
}

bool markdown_core_compile_open(markdown_core_compiler *compiler, markdown_core_node *node) {
    return markdown_core_compile_append(compiler, node) && markdown_core_compile_push(compiler, node);
}

markdown_core_node *markdown_core_compile_text(markdown_core_compiler *compiler, bufsize_t from, bufsize_t to) {
    markdown_core_inline_state *inline_state = markdown_core_compile_state(compiler);
    markdown_core_node *node =
        make_str(inline_state, from, to - 1, markdown_core_chunk_dup(&inline_state->input, from, to - from));
    return markdown_core_compile_append(compiler, node) ? node : NULL;
}

void markdown_core_inline_finish_inlines(markdown_core_parser *parser, markdown_core_inline_state *inline_state) {
    if (!parser->error && !inline_state->error) {
        S_read(parser, inline_state, false);
    }
    S_finish_run(inline_state, false);
}

/* A run that reads its root's content from `from` on gives up again what
 * the root's earlier runs gave up from there: the constructs they kept
 * (node.h, OPENER) whose opener starts where content byte `from` was
 * written or past it, all of them from the content's start, and the fields
 * after each go with it, a published one from where `source` says.
 * Those a line of the block phase kept stay. */
static void S_opens_drop(markdown_core_node *root, bufsize_t from, uint32_t source) {
    markdown_core_node_drop_opens(root, false, (uint32_t)from, source);
}

void markdown_core_parse_inlines(markdown_core_parser *parser, markdown_core_node *parent, bool root) {
    if (root && parent->fields) {
        S_opens_drop(parent, 0, 0);
    }
    markdown_core_inline_state inline_state;
    markdown_core_inline_start_inlines(parser, parent, root, &inline_state);
    markdown_core_inline_finish_inlines(parser, &inline_state);
}

void markdown_core_parse_inlines_seam(markdown_core_parser *parser, markdown_core_node *parent,
                                      markdown_core_inline_settle *settle, markdown_core_seam_stops *stops) {
    markdown_core_inline_state inline_state;
    if (parent->fields) {
        S_opens_drop(parent, settle->from, settle->source);
    }
    markdown_core_inline_start_inlines(parser, parent, true, &inline_state);
    inline_state.stops = stops;
    settle->count = 0;
    settle->note_count = 0;
    inline_state.settle = settle;
    inline_state.undecided = INT32_MAX;
    inline_state.pos = settle->from;
    S_read(parser, &inline_state, false);
    S_finish_run(&inline_state, true);
}

uint32_t markdown_core_inline_state_root_offset(markdown_core_inline_state *inline_state, int offset) {
    const markdown_core_content_map *map = inline_state->owner ? &inline_state->owner->content_map : NULL;
    if (!map || !inline_state->owner_parser || map->first == MARKDOWN_CORE_IDENTITY_MARK || map->count <= 0) {
        return offset > 0 ? (uint32_t)offset : 0;
    }
    int line;
    bufsize_t at = 0;
    if (offset < inline_state->input.len) {
        markdown_core_parser_content_place(inline_state->owner_parser, map, offset > 0 ? offset : 0, &line, &at);
    } else if (inline_state->input.len > 0) {
        markdown_core_parser_content_end_place(inline_state->owner_parser, map, inline_state->input.len - 1, &line,
                                               &at);
    }
    return (uint32_t)at;
}

markdown_core_chunk *markdown_core_inline_state_get_chunk(markdown_core_inline_state *inline_state) {
    return &inline_state->input;
}
