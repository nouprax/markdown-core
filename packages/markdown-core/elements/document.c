#include "alloc.h"
#include "document.h"
#include "block_internal.h"
#include "properties.h"
#include "heading.h"
#include "ast_internal.h"

/* The elements whose state this element reads, as `self->peers` holds them. */
enum { DOCUMENT_HEADING };
static const markdown_core_element *const DOCUMENT_PEERS[] = {[DOCUMENT_HEADING] = &MARKDOWN_CORE_ELEMENT_HEADING,
                                                              NULL};

/* THE DOCUMENT LIFECYCLE. The document element drives it and owns only the
 * properties grammar's work, its parse record; the definitions the parse
 * declares are the parse's registry (registry.h), and the headings it
 * finalizes are the heading element's state, its peer, which it asks through
 * their lifecycle calls. An element the dialect does not hold has nothing to
 * finalize. */
static void init_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    (void)parser;
}
static void dispose_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)parser;
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
}
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_prepare(headings, parser);
    }
}
/* The headings take their anchors at each snapshot and at the finalize, and
 * a snapshot's commit keeps them for the next: only the finalize, the parse's
 * last, lets them go. */
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!parser->error && headings) {
        markdown_core_headings_finish(headings, parser);
    }
    if (headings && !parser->snapshot) {
        markdown_core_headings_dispose(headings);
    }
}
static void start_snapshot(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_start_snapshot(headings, parser);
    }
}
static void end_snapshot(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_end_snapshot(headings, parser);
    }
}
static void mark_candidate(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_mark_candidate(headings, parser);
    }
}
static void undo_candidate(const markdown_core_element_instance *self, markdown_core_parser *parser, size_t index) {
    (void)parser;
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_undo_candidate(headings, index);
    }
}
static void drop_candidate(const markdown_core_element_instance *self, markdown_core_parser *parser, size_t index) {
    (void)parser;
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_drop_candidate(headings, index);
    }
}
static void publish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    if (!markdown_core_publish_tree(parser)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
static void publish_candidate(const markdown_core_element_instance *self, markdown_core_parser *parser,
                              markdown_core_node *candidate, markdown_core_node *root, uint32_t holder_start) {
    (void)self;
    if (!markdown_core_publish_candidate(parser, candidate, root, holder_start)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* THE PREFIX, the call before line 1: an envelope that closes is read as
 * the document's properties. The call asks whether its reading read the end
 * (parser.h, markdown_core_parser_end_read): one that read it where more
 * text may follow decides nothing, and asks to be a candidate (blocks.c, THE
 * CANDIDATES); a call that meets its candidate declines. */
static size_t read_document_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    if (parser->stream && markdown_core_parser_candidate_meets(parser, self, parser->root)) {
        return 0;
    }
    markdown_core_parser_clear_end_read(parser);
    size_t fence;
    const bool closed = markdown_core_properties_closed(self->state, parser, &fence);
    if (markdown_core_parser_end_read(parser)) {
        if (!parser->error) {
            const int words[MARKDOWN_CORE_CANDIDATE_WORDS] = {0};
            markdown_core_parser_request_candidate(parser, self, parser->root, MARKDOWN_CORE_NODE_METADATA, words,
                                                   false);
        }
        return 0;
    }
    return closed ? markdown_core_properties_parse(self->state, parser, fence) : 0;
}

/* THE FEED of the prefix's candidate: the first line opens the envelope
 * when it is a fence, which the first of the words keeps, and ends the
 * construct otherwise; after it, a fence line closes the envelope and ends
 * the construct, and any other line goes on with it, a line the text has
 * not finished once its bytes rule a fence out. An envelope its text leaves
 * open is not claimed. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, int *words,
                                                   const markdown_core_chunk *input, int first, int indent, int blanks,
                                                   bool unfinished, bool *ends) {
    (void)self;
    (void)parser;
    (void)first;
    (void)indent;
    const bool opened = words[0];
    const int fence = input ? markdown_core_properties_fence(input->data) : -1;
    if (unfinished && fence > 0) {
        return MARKDOWN_CORE_CANDIDATE_WAITS;
    }
    if (opened ? fence == 3 : blanks || fence != 3) {
        return MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    words[0] = 1;
    *ends = false;
    return MARKDOWN_CORE_CANDIDATE_CONTINUES;
}

/* The prefix's call decided as it would read now: the envelope closes at
 * the fence that ended the construct. */
static markdown_core_candidate_decision decide_candidate(const markdown_core_element_instance *self,
                                                         markdown_core_parser *parser, const int *words) {
    (void)words;
    size_t fence;
    return markdown_core_properties_closed(self->state, parser, &fence) ? MARKDOWN_CORE_CANDIDATE_ACCEPT
                                                                        : MARKDOWN_CORE_CANDIDATE_REJECT;
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DOCUMENT = {
    .peers = DOCUMENT_PEERS,
    .state_size = sizeof(markdown_core_properties_work),
    .name = "document",
    .init_document = init_document,
    .dispose_document = dispose_document,
    .read_document_prefix = read_document_prefix,
    .feed_candidate = feed_candidate,
    .decide_candidate = decide_candidate,
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .start_snapshot = start_snapshot,
    .end_snapshot = end_snapshot,
    .mark_candidate = mark_candidate,
    .undo_candidate = undo_candidate,
    .drop_candidate = drop_candidate,
    .publish_document = publish_document,
    .publish_candidate = publish_candidate,
};
