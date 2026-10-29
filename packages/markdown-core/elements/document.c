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
/* The headings take their anchors at each snapshot and at the flush, and
 * a snapshot's commit keeps them for the next: the flush of a parse that
 * takes edits keeps them for its edits, which reopen them, and the parse's
 * release lets them go (dispose_document); any other flush, the parse's
 * last, lets them go. */
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!parser->error && headings) {
        markdown_core_headings_finish(headings, parser);
    }
    if (headings && !parser->snapshot && !markdown_core_parser_takes_edits(parser)) {
        markdown_core_headings_dispose(headings);
    }
}
/* AN EDIT REOPENS THE DOCUMENT (blocks.c, THE REOPEN): the published tree
 * keeps the children before `child`, and the headings those that start
 * before `start`; what goes is matched against what the edit reads (ast.c,
 * A DOCUMENT REOPENED FOR AN EDIT). */
static bool reopen_document(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *root, uint32_t child, uint32_t start) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    return markdown_core_publish_reopen(parser, root, child) &&
           (!headings || markdown_core_headings_reopen(headings, parser, start));
}
/* THE EDIT TAKES THE OLD CHILDREN BACK (blocks.c, THE SPLICE): the
 * headings take theirs back first, and the publish's records let the
 * children go last, once nothing else can fail. */
static bool splice_document(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_node *root, uint32_t child, uint32_t start, uint32_t point) {
    (void)root;
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    return (!headings || markdown_core_headings_splice(headings, parser, start, point)) &&
           markdown_core_publish_splice(parser, child, point);
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
    if (markdown_core_parser_candidate_meets(parser, self, parser->root)) {
        return 0;
    }
    markdown_core_parser_clear_end_read(parser);
    size_t fence;
    const bool closed = markdown_core_properties_closed(self->state, parser, &fence);
    if (markdown_core_parser_end_read(parser)) {
        if (!parser->error) {
            const bool opened = false;
            markdown_core_parser_request_candidate(parser, self, parser->root, MARKDOWN_CORE_NODE_METADATA, &opened,
                                                   false);
        }
        return 0;
    }
    return closed ? markdown_core_properties_parse(self->state, parser, fence) : 0;
}

/* THE FEED of the prefix's candidate: the first line opens the envelope
 * when it is a fence, which the candidate's record keeps, and ends the
 * construct otherwise; after it, a fence line closes the envelope and ends
 * the construct, and any other line goes on with it, a line the text has
 * not finished once its bytes rule a fence out. An envelope its text leaves
 * open is not claimed. */
static markdown_core_candidate_step feed_candidate(const markdown_core_element_instance *self,
                                                   markdown_core_parser *parser, void *state,
                                                   const markdown_core_candidate_line *line, bool *ends) {
    (void)self;
    (void)parser;
    bool *opened = state;
    const int fence = line->content ? markdown_core_properties_fence(line->content->data) : -1;
    if (line->unfinished && fence > 0) {
        return MARKDOWN_CORE_CANDIDATE_WAITS;
    }
    if (*opened ? fence == 3 : line->blanks || fence != 3) {
        return MARKDOWN_CORE_CANDIDATE_ENDS;
    }
    *opened = true;
    *ends = false;
    return MARKDOWN_CORE_CANDIDATE_CONTINUES;
}

/* The prefix's call decided as it would read now: the envelope closes at
 * the fence that ended the construct. */
static markdown_core_candidate_decision decide_candidate(const markdown_core_element_instance *self,
                                                         markdown_core_parser *parser, const void *state) {
    (void)state;
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
    .candidate_state_size = sizeof(bool),
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .reopen_document = reopen_document,
    .splice_document = splice_document,
    .publish_document = publish_document,
    .publish_candidate = publish_candidate,
};
