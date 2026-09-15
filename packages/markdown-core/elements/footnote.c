#include "link.h"
#include "footnote_scanners.h"
#define MAX_FOOTNOTE_DEPTH 100
#include "citation.h"
#include "footnote.h"
#include "inline_internal.h"
#include "block_internal.h"

static bool markdown_core_inline_footnote_label_is_defined(markdown_core_parser *parser,
                                                           markdown_core_inline_state *inline_state,
                                                           bufsize_t label_start, bufsize_t after_close);
static markdown_core_node *markdown_core_inline_make_footnote_cite(markdown_core_inline_state *inline_state,
                                                                   bracket *opener, bufsize_t after_close);
static bool markdown_core_footnote_scan(markdown_core_parser *parser, block_start_context *context, block_start *start);
void markdown_core_block_finalize_footnotes(markdown_core_parser *parser) {
    markdown_core_definition_collection *collection = &parser->footnotes;
    markdown_core_key_index ids = {0};
    size_t index, ordinal = 0;
    if (!collection->count) {
        goto done;
    }
    if (!markdown_core_block_order_definitions(parser->mem, collection) ||
        !markdown_core_key_index_init(&ids, parser->mem, collection->count)) {
        goto failed;
    }
    for (index = 0; index < collection->count; index++) {
        markdown_core_node *footnote = collection->values[index].definition;
        markdown_core_chunk *id = &footnote->as.footnote->id;
        if (id->data && !markdown_core_key_index_insert(&ids, id->data, id->len, footnote, 0, NULL)) {
            goto failed;
        }
    }
    for (index = 0; index < collection->count; index++) {
        markdown_core_node *footnote = collection->values[index].definition;
        markdown_core_chunk *id = &footnote->as.footnote->id;
        if (!id->data) {
            /* Each decimal size_t takes at most 3 * sizeof(size_t) bytes. */
            char candidate[sizeof("inline--") + 6 * sizeof(size_t)];
            size_t suffix = 0;
            markdown_core_node *citation = collection->values[index].citation;
            assert(citation && citation->kind == MARKDOWN_CORE_NODE_CITATION);
            ordinal++;
            snprintf(candidate, sizeof(candidate), "inline-%zu", ordinal);
            while (
                markdown_core_key_index_lookup(&ids, (const unsigned char *)candidate, (bufsize_t)strlen(candidate))) {
                snprintf(candidate, sizeof(candidate), "inline-%zu-%zu", ordinal, ++suffix);
            }
            if (!markdown_core_chunk_set_cstr(parser->mem, id, candidate) ||
                !markdown_core_chunk_set_cstr(parser->mem, &citation->as.citation->value, candidate) ||
                !markdown_core_key_index_insert(&ids, id->data, id->len, footnote, 0, NULL)) {
                goto failed;
            }
        }
    }
    /* No allocation or fallible work remains once ownership starts moving. */
    markdown_core_block_own_definitions(collection, &parser->root->as.document->footnotes);
    goto done;
failed:
    parser->oom = true;
done:
    markdown_core_key_index_free(&ids);
    parser->mem->free(collection->values);
    memset(collection, 0, sizeof(*collection));
}

static bool markdown_core_inline_footnote_label_is_defined(markdown_core_parser *parser,
                                                           markdown_core_inline_state *inline_state,
                                                           bufsize_t label_start, bufsize_t after_close) {
    markdown_core_chunk label;
    bool defined;

    if (after_close - label_start < 2) {
        return false;
    }
    /* A borrowed slice of the block's own content: `markdown_core_chunk_dup`
     * aliases, so the only allocation in here is the map's own normalization,
     * and that one reports itself through the map's sticky flag. */
    label = markdown_core_chunk_dup(&inline_state->input, label_start + 1, after_close - label_start - 2);
    defined = markdown_core_map_lookup(parser->footnote_defs, &label) != NULL;
    return defined;
}

static markdown_core_node *markdown_core_inline_make_footnote_cite(markdown_core_inline_state *inline_state,
                                                                   bracket *opener, bufsize_t after_close) {
    markdown_core_node *cite = markdown_core_inline_new_cite(inline_state);
    markdown_core_node *citation = cite ? markdown_core_inline_new_citation(inline_state, cite, NULL) : NULL;
    if (!citation) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        inline_state->oom = 1;
        return NULL;
    }
    cite->as.cite->citations = citation;
    citation->as.citation->referent = MARKDOWN_CORE_NODE_REFERENT_FOOTNOTE;
    markdown_core_inline_state_place(inline_state, cite, opener->position - (opener->kind == BRACKET_FOOTNOTE ? 2 : 1),
                                     after_close - 1);
    markdown_core_inline_state_place(inline_state, citation, opener->position, after_close - 2);
    return cite;
}

markdown_core_node *markdown_core_inline_close_inline_footnote(markdown_core_parser *parser,
                                                               markdown_core_inline_state *inline_state,
                                                               bracket *opener) {
    markdown_core_node *cite, *footnote;
    /* The consumed body is inspected once by markdown_core_inline_parse_inline, never once per
     * ancestor. Invalid openers keep their already parsed content as text. */
    if (inline_state->nonblank_end <= opener->position ||
        !markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
        inline_state->no_link_openers = opener->outer_no_link_openers;
        markdown_core_inline_pop_bracket(inline_state);
        return make_str(inline_state, inline_state->pos - 1, inline_state->pos - 1, markdown_core_chunk_literal("]"));
    }
    cite = markdown_core_inline_make_footnote_cite(inline_state, opener, inline_state->pos);
    footnote = cite ? markdown_core_inline_make_simple(inline_state->mem, MARKDOWN_CORE_NODE_FOOTNOTE) : NULL;
    if (!footnote) {
        if (cite) {
            markdown_core_node_free(cite);
        }
        inline_state->oom = 1;
        markdown_core_inline_pop_bracket(inline_state);
        return NULL;
    }
    markdown_core_inline_state_place(inline_state, footnote, opener->position - 2, inline_state->pos - 1);
    markdown_core_inline_finish_citation_tokens(inline_state, &opener->citations);
    markdown_core_inline_process_delimiters(parser, inline_state, opener->position, opener->delim_end);
    markdown_core_inline_take_bracket_content(parser, opener, footnote);
    markdown_core_node_attach_owned(opener->inl_text->parent, cite, opener->inl_text);
    if (!markdown_core_parser_register_definition(parser, &parser->footnotes, footnote, cite->as.cite->citations,
                                                  &parser->root->as.document->footnotes)) {
        markdown_core_node_free(footnote);
        inline_state->oom = 1;
    }
    markdown_core_node_free(opener->inl_text);
    inline_state->no_link_openers = opener->outer_no_link_openers;
    markdown_core_inline_pop_bracket(inline_state);
    return NULL;
}

static markdown_core_node *match(const markdown_core_element *self, markdown_core_parser *parser,
                                 markdown_core_node *parent, unsigned char character,
                                 markdown_core_inline_state *inline_state) {
    if (character != '^' || markdown_core_inline_peek_char_n(inline_state, 1) != '[') {
        return NULL;
    }
    inline_state->pos += 2;
    markdown_core_node *node =
        make_str(inline_state, inline_state->pos - 2, inline_state->pos - 1, markdown_core_chunk_literal("^["));
    if (node) {
        markdown_core_inline_push_bracket(inline_state, BRACKET_FOOTNOTE, node);
    }
    return node;
}
static bool continue_container(markdown_core_parser *parser, markdown_core_node *node, markdown_core_chunk *input,
                               const markdown_core_node *joining, bool *taken) {
    return markdown_core_footnote_continue(parser, node, input);
}
const markdown_core_element MARKDOWN_CORE_ELEMENT_FOOTNOTE = {
    .name = "footnote",
    .continue_container = continue_container,
    .maximum_block_indent = 3,
    .scan_block_start = markdown_core_footnote_scan,

    .match_inline = match,
    .terminates_text = "^",
    .dispatch = "^",
};

bool markdown_core_footnote_close_reference(markdown_core_parser *parser, markdown_core_inline_state *inline_state,
                                            bracket *opener) {
    bufsize_t initial_pos = inline_state->pos;
    if (opener->inl_text->next && opener->inl_text->next->kind == MARKDOWN_CORE_NODE_TEXT) {

        markdown_core_chunk *literal = opener->inl_text->next->as.literal;

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
        if (caret_written &&
            markdown_core_inline_footnote_label_is_defined(parser, inline_state, opener->position, initial_pos)) {
            if (!markdown_core_node_can_contain_type(opener->inl_text->parent, MARKDOWN_CORE_NODE_CITE)) {
                return false;
            }

            // Before we got this far, the `markdown_core_inline_handle_close_bracket` function may have
            // advanced the current state beyond our footnote's actual closing
            // bracket, ie if it went looking for a `markdown_core_inline_link_label`.
            // Let's just rewind the inline state's position:
            inline_state->pos = initial_pos;

            markdown_core_node *fnref = markdown_core_inline_make_footnote_cite(inline_state, opener, initial_pos);
            if (!fnref) {
                markdown_core_inline_pop_bracket(inline_state);
                return true;
            }
            markdown_core_chunk label =
                markdown_core_chunk_dup(&inline_state->input, opener->position + 1, initial_pos - opener->position - 2);
            int lost = 0;
            unsigned char *id = normalize_map_label(inline_state->mem, &label, &lost);
            if (!id) {
                inline_state->oom = 1;
                markdown_core_node_free(fnref);
                markdown_core_inline_pop_bracket(inline_state);
                return true;
            }
            markdown_core_chunk *value = &fnref->as.cite->citations->as.citation->value;
            value->data = id;
            value->len = (bufsize_t)strlen((const char *)id);
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
            markdown_core_node *next_node;
            markdown_core_node *current_node = opener->inl_text->next;
            while (current_node) {
                next_node = current_node->next;
                markdown_core_node_free(current_node);
                current_node = next_node;
            }

            markdown_core_inline_replace_bracket_opener(inline_state, opener, fnref);
            markdown_core_inline_pop_bracket(inline_state);
            return true;
        }
    }
    return false;
}

static bool markdown_core_footnote_open(markdown_core_parser *parser, markdown_core_node **container,
                                        markdown_core_chunk *input, block_start *start) {
    bufsize_t matched = start->matched;

    markdown_core_chunk c = markdown_core_chunk_dup(input, parser->first_nonspace + 2, matched - 2);
    unsigned char *id;
    int lost = 0;

    while (c.data[c.len - 1] != ']') {
        --c.len;
    }
    --c.len;

    if (!markdown_core_chunk_to_cstr(parser->mem, &c)) {
        /* The label would keep borrowing the transient line buffer. */
        parser->oom = true;
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
        markdown_core_chunk_free(parser->mem, &c);
        return false;
    }
    /* The id is the label under the map's own normalization and
     * WITHOUT a caret (M4): the key every call's referent names. The
     * caret that kept a footnote apart from a link definition in a
     * consumer's single map went with the association -- a
     * `Footnote` and a resolved `Link` are different values now. */
    id = normalize_map_label(parser->mem, &c, &lost);
    if (!id) {
        parser->oom = true;
        markdown_core_chunk_free(parser->mem, &c);
        return false;
    }
    (*container)->as.footnote->id.data = id;
    (*container)->as.footnote->id.len = (bufsize_t)strlen((const char *)id);
    (*container)->as.footnote->id.alloc = 1;
    if (!markdown_core_parser_register_definition(parser, &parser->footnotes, *container, NULL, NULL)) {
        markdown_core_chunk_free(parser->mem, &c);
        return false;
    }

    /* The document defines this label from here on.
     *
     * Registered where the label is READ, which is here. Whether it is
     * registered at open or at close is NOT observable and that was
     * measured, not assumed: moving this call into `markdown_core_block_finalize` leaves
     * every suite and every oracle green. It used to matter, and the
     * reason it stopped is the shape rather than the timing -- the map
     * this replaced held a NODE per entry and used registration order
     * as the tie-break for a repeated label, so on EXIT a definition
     * nested inside another closed first, won the label, and the outer
     * one was freed with everything written in it (D11). A set of
     * labels owns no node and picks no winner, so order decides
     * nothing left to get wrong. */
    markdown_core_footnote_definition_create(parser->footnote_defs, &c);
    markdown_core_chunk_free(parser->mem, &c);

    (*container)->internal_offset = matched;
    return true;
}

static bool markdown_core_footnote_scan(markdown_core_parser *parser, block_start_context *context,
                                        block_start *start) {
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

bool markdown_core_footnote_continue(markdown_core_parser *parser, markdown_core_node *container,
                                     markdown_core_chunk *input) {
    return markdown_core_block_continue_indented(parser, input, 4, true);
}
