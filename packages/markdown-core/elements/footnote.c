#include "alloc.h"
#include "link.h"
#include "footnote_scanners.h"
#include "map.h"
#define MAX_FOOTNOTE_DEPTH 100
#include "citation.h"
#include "footnote.h"
#include "inline_internal.h"
#include "block_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { FOOTNOTE_LINK, FOOTNOTE_CITATION };
static const markdown_core_element *const FOOTNOTE_PEERS[] = {
    [FOOTNOTE_LINK] = &MARKDOWN_CORE_ELEMENT_LINK, [FOOTNOTE_CITATION] = &MARKDOWN_CORE_ELEMENT_CITATION, NULL};

static bool markdown_core_footnote_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start);

/* The key a call's label names, when a definition declares it, or NULL. The
 * key's label IS the call's normal form, so the call takes its id from there
 * rather than normalizing the same label a second time. */
static const markdown_core_key *markdown_core_inline_footnote_definition(markdown_core_inline_state *inline_state,
                                                                         bufsize_t label_start, bufsize_t after_close) {
    if (after_close - label_start < 2) {
        return NULL;
    }
    /* A borrowed slice of the block's own content: `markdown_core_chunk_dup`
     * aliases. */
    markdown_core_chunk label =
        markdown_core_chunk_dup(&inline_state->input, label_start + 1, after_close - label_start - 2);
    return markdown_core_inline_ask(inline_state, MARKDOWN_CORE_KEY_FOOTNOTE, &label, true);
}

/* A Cite of one footnote Citation, put before `opener`'s literal; the
 * Citation's member, or NULL, with the run failed, when it could not be
 * allocated. */
static markdown_core_member *markdown_core_inline_make_footnote_cite(markdown_core_inline_state *inline_state,
                                                                     bracket *opener, bufsize_t after_close) {
    markdown_core_node *node = markdown_core_inline_new_cite(inline_state);
    markdown_core_member *cite = node ? markdown_core_inline_insert_at_opener(inline_state, opener, node) : NULL;
    markdown_core_member *citation = cite ? markdown_core_inline_new_citation(inline_state, cite) : NULL;
    if (!citation) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        return NULL;
    }
    citation->node->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    markdown_core_inline_state_place(inline_state, node, opener->position - (opener->kind == BRACKET_FOOTNOTE ? 2 : 1),
                                     after_close - 1);
    markdown_core_inline_state_place(inline_state, citation->node, opener->position, after_close - 2);
    return citation;
}

markdown_core_member *markdown_core_inline_close_inline_footnote(const markdown_core_element_instance *self,
                                                                 markdown_core_parser *parser,
                                                                 markdown_core_inline_state *inline_state,
                                                                 bracket *opener) {
    /* The consumed body is inspected once by markdown_core_inline_parse_inline, never once per
     * ancestor. Invalid openers keep their already parsed content as text. */
    if (inline_state->nonblank_end <= opener->position ||
        !markdown_core_node_can_contain_type(opener->inl_text->owner->node, MARKDOWN_CORE_NODE_CITE)) {
        markdown_core_brackets(self->peers[FOOTNOTE_LINK], inline_state)->no_link_openers =
            opener->outer_no_link_openers;
        markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
        return markdown_core_inline_state_append(
            inline_state, make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1,
                                   markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 1, 1)));
    }
    markdown_core_inline_finish_citation_tokens(self->peers[FOOTNOTE_CITATION], inline_state, &opener->citations);
    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_member *citation = markdown_core_inline_make_footnote_cite(inline_state, opener, inline_state->pos);
    markdown_core_node *footnote =
        citation ? markdown_core_inline_make_simple(inline_state, MARKDOWN_CORE_NODE_FOOTNOTE) : NULL;
    markdown_core_member *note = NULL;
    if (footnote) {
        markdown_core_inline_state_place(inline_state, footnote, opener->position - 2, inline_state->pos - 1);
        /* The note is its Citation's own field, as the affixes are. */
        citation->node->as.citation->note = footnote;
        note = markdown_core_parser_attach_field(parser, citation, footnote);
    }
    if (!note) {
        inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
        markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
        return NULL;
    }
    markdown_core_inline_take_bracket_content(self->peers[FOOTNOTE_LINK], parser, opener, note);
    markdown_core_parser_release_member(parser, opener->inl_text);
    markdown_core_brackets(self->peers[FOOTNOTE_LINK], inline_state)->no_link_openers = opener->outer_no_link_openers;
    markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
    return NULL;
}

static markdown_core_member *match(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                   markdown_core_member *parent, unsigned char character,
                                   markdown_core_inline_state *inline_state) {
    /* An inline note opens a bracket, which only a dialect with links reads. */
    if (character != '^' || markdown_core_inline_peek_char_n(inline_state, 1) != '[' || !self->peers[FOOTNOTE_LINK]) {
        return NULL;
    }
    inline_state->pos += 2;
    markdown_core_member *member = markdown_core_inline_state_append(
        inline_state, make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1,
                               markdown_core_chunk_dup(&inline_state->input, inline_state->pos - 2, 2)));
    if (member) {
        markdown_core_inline_push_bracket(self->peers[FOOTNOTE_LINK], inline_state, BRACKET_FOOTNOTE, member);
    }
    return member;
}
static bool continue_container(const markdown_core_element_instance *self, markdown_core_parser *parser,
                               markdown_core_member *node, markdown_core_chunk *input,
                               const markdown_core_member *joining, bool *taken) {
    (void)self;
    return markdown_core_footnote_continue(parser, node, input);
}
/* A footnote definition carries nothing its lines read (E3). */
static uint32_t carry_save(const markdown_core_element_instance *self, const markdown_core_member *member) {
    (void)self;
    (void)member;
    return 0;
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE = {
    .peers = FOOTNOTE_PEERS,
    .name = "footnote",
    .continue_container = continue_container,
    .carry_save = carry_save,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_footnote_scan,
    .scan_block_gate = {.bytes = "["},

    .match_inline = match,
    .terminates_text = "^",
    .dispatch = "^",
};

bool markdown_core_footnote_close_reference(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                            markdown_core_inline_state *inline_state, bracket *opener) {
    bufsize_t initial_pos = inline_state->pos;
    if (opener->inl_text->next && opener->inl_text->next->node->kind == MARKDOWN_CORE_NODE_TEXT) {

        markdown_core_chunk *literal = opener->inl_text->next->node->as.literal;

        // A footnote call opens with a caret the SOURCE spells literally.
        //
        // This used to test the decoded first byte, so `[\^a]` and `[&#94;a]`
        // opened calls too — and neither could ever resolve, because the label
        // was reconstructed from a different coordinate space than the one the
        // lookup key came from. What they produced instead was a rebuilt `[^`
        // prefix over decoded bytes: `[\^abc] x` came back as `[^^abc] x`, an
        // invented caret, and `[&#94;a]` as `[^#94;a]`.
        //
        // `opener->position` is the byte after the '[', which is where the
        // caret must be. The bounds test comes first because that is the order
        // a subscript and its guard belong in -- D4 is what happens when they
        // are the other way round. It is REDUNDANT here and the proof is worth
        // writing down rather than rediscovering: reaching this function means
        // a ']' was consumed, and that ']' is after the '[', so
        // `opener->position <= initial_pos - 2 < inline_state->input.len`. No mutant
        // kills it, measured; it is kept because a reader should not have to
        // reconstruct that argument before touching the line.
        //
        // AND THE DOCUMENT DEFINES THAT LABEL. Both halves of Step 9a's rule
        // are here now, and the second one is what makes the failure path
        // ordinary: a `[^label]` nothing defines is not a footnote call, so it
        // is an unmatched `[`, which CommonMark specifies -- remove the
        // delimiter-stack entry, emit a literal `]`, and leave the interior
        // alone. Every one of its three failure branches says NOT re-parenting
        // is what failure means, and the interior nodes exist because core
        // inline parsing built them before any footnote code ran (§5.7, Q2).
        // What used to happen instead was that the call succeeded on the caret
        // alone, and the post-pass that could not resolve it replaced the whole
        // span with one flat literal -- freeing nodes core had already built,
        // for a construct that turned out not to exist.
        //
        // The definition set is filled by the block phase, which has finished
        // by the time any inline is parsed, so "defines" is answered over the
        // WHOLE document here and not over a prefix of it.
        /* THE CARET IS THE EVIDENCE, and it is separated from the definedness
         * test so that the case where the first holds and the second does not
         * can be reported. `[^x]` is footnote syntax and nothing else; a reader
         * who writes it and gets prose has no other way to find out. */
        bool caret_written = opener->position < inline_state->input.len &&
                             inline_state->input.data[opener->position] == '^' &&
                             (literal->len > 1 || opener->inl_text->next->next);
        const markdown_core_key *definition =
            caret_written ? markdown_core_inline_footnote_definition(inline_state, opener->position, initial_pos)
                          : NULL;
        if (definition) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->owner->node, MARKDOWN_CORE_NODE_CITE)) {
                return false;
            }

            // Before we got this far, the `markdown_core_inline_handle_close_bracket` function may have
            // advanced the current state beyond our footnote's actual closing
            // bracket, ie if it went looking for a `markdown_core_inline_link_label`.
            // Let's just rewind the inline state's position:
            inline_state->pos = initial_pos;

            markdown_core_member *citation = markdown_core_inline_make_footnote_cite(inline_state, opener, initial_pos);
            if (!citation) {
                markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
                return true;
            }
            /* The call's label is its normal form: the key the lookup
             * matched, copied rather than computed again. */
            unsigned char *id = markdown_core_alloc(1, (size_t)definition->length + 1);
            if (!id) {
                inline_state->error = MARKDOWN_CORE_PARSE_ALLOCATION_FAILED;
                markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
                return true;
            }
            memcpy(id, definition->label, (size_t)definition->length + 1);
            markdown_core_chunk *value = &citation->node->as.citation->value;
            value->data = id;
            value->len = (bufsize_t)definition->length;
            value->alloc = 1;

            markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
            // sometimes, the footnote reference text gets parsed into multiple nodes
            // i.e. '[^example]' parsed into '[', '^exam', 'ple]'.
            // this happens for ex with the autolink element. when the autolinker
            // finds the 'w' character, it will split the text into multiple nodes
            // in hopes of being able to match a 'www.' substring.
            //
            // because this function is called one character at a time via the
            // `parse_inlines` function, and the current inline_state->pos is pointing at the
            // closing ] brace, and because we copy all the text between the [ ]
            // braces, we should be able to safely ignore and delete any nodes after
            // the opener->inl_text->next.
            //
            // therefore, here we walk thru the list and free them all up
            /* A valid definition label contains no ']'; a completed inline
             * footnote necessarily does. This label therefore cannot own a
             * committed Footnote from the parser collection. */
            markdown_core_member *next_node;
            markdown_core_member *current_node = opener->inl_text->next;
            while (current_node) {
                next_node = current_node->next;
                markdown_core_parser_release_member(parser, current_node);
                current_node = next_node;
            }
            markdown_core_parser_release_member(parser, opener->inl_text);
            markdown_core_inline_pop_bracket(self->peers[FOOTNOTE_LINK], inline_state);
            return true;
        }
    }
    return false;
}

static bool markdown_core_footnote_open(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        markdown_core_member **container, markdown_core_chunk *input,
                                        block_start *start) {
    bufsize_t matched = start->matched;

    markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);
    unsigned char *id;
    int lost = 0;

    while (c.data[c.len - 1] != ']') {
        --c.len;
    }
    --c.len;

    if (!markdown_core_chunk_to_cstr(&c)) {
        /* The label would keep borrowing the transient line buffer. */
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        return false;
    }

    markdown_core_block_advance_offset(parser, input, parser->first_nonspace + matched - parser->offset, false);
    /* THE ANCHOR RULE (§5.1): a definition is a block node at the byte
     * where its OPENING BRACKET was written. It used to start at the
     * byte after `[^label]:`, which is a column that need not exist --
     * `[^footnote]:` alone on a line is twelve bytes and the definition
     * began at column 13. Every other block in this engine starts at
     * its own first byte and the marker is inside it; a footnote
     * definition was the one that started after its own marker. */
    *container =
        markdown_core_parser_add_child(parser, *container, MARKDOWN_CORE_NODE_FOOTNOTE, parser->first_nonspace + 1);
    if (!*container) {
        markdown_core_chunk_free(&c);
        return false;
    }
    /* The label is under the map's own normalization and WITHOUT a caret
     * (M4): the key every call's referent names. The
     * caret that kept a footnote apart from a link definition in a
     * consumer's single map went with the association -- a
     * `Footnote` and a resolved `Link` are different values now. */
    id = normalize_map_label(&c, &lost);
    if (!id) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        markdown_core_chunk_free(&c);
        return false;
    }
    markdown_core_optional_chunk *label = &(*container)->node->as.footnote->label;
    label->has_value = true;
    label->value.data = id;
    label->value.len = (bufsize_t)strlen((const char *)id);
    label->value.alloc = 1;
    /* The document defines this label from here on: the definition holds the
     * fact (registry.h) for as long as it is in the tree. */
    markdown_core_parser_declare(parser, (*container)->node, MARKDOWN_CORE_KEY_FOOTNOTE, &label->value);
    markdown_core_chunk_free(&c);

    (*container)->node->internal_offset = matched;
    return true;
}

static bool markdown_core_footnote_scan(const markdown_core_element_instance *self, markdown_core_parser *parser,
                                        block_start_context *context, block_start *start) {
    (void)self;
    markdown_core_chunk *input = context->input;
    int first = context->first;
    if (!(context->depth < MAX_FOOTNOTE_DEPTH &&
          (start->matched = scan_footnote_definition(input->data, input->len, first)))) {
        return false;
    }
    start->kind = MARKDOWN_CORE_NODE_FOOTNOTE;
    start->open = markdown_core_footnote_open;
    return true;
}

bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_member *container,
                                     markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, 4, true);
}
