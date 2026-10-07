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

/* THE DOCUMENT'S PARSE RECORD: the properties grammar's work, and what the
 * parse publishes as its nodes complete. */
typedef struct {
    markdown_core_properties_work properties;
    markdown_core_publication publication;
} document_state;

/* THE DOCUMENT LIFECYCLE. The document element drives it and owns only its
 * parse record; the headings it finishes are the state of their own element,
 * a peer, which it asks through its lifecycle calls. An element the dialect
 * does not hold has nothing to finish. */
static void init_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    (void)self;
    (void)parser;
}
static void dispose_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
    markdown_core_publication_dispose(&((document_state *)self->state)->publication, parser->pool);
}
/* RESOLUTION OF LOOKUPS (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.7). Every definition is declared once the blocks are complete and the
 * headings the parse made have declared their labels, and the questions
 * asked of the registry then that an earlier parse asked are those of the
 * roots the parse took. A key whose answer, whether it is defined, is not
 * the one it gave the old document has each such root read again, from
 * where it began in the old source. Each parse of the edit finds the same
 * keys and roots, and the first finds them before it moves any fact. */
static void reread_lookups(markdown_core_parser *parser) {
    const markdown_core_registry *registry = parser->registry;
    for (const markdown_core_key *key = registry->marked; key && !parser->error; key = key->marked_next) {
        if (key->group == MARKDOWN_CORE_KEY_FAMILY || key->was == markdown_core_key_defined(key)) {
            continue;
        }
        for (const markdown_core_fact *lookup = key->lookups; lookup && !parser->error; lookup = lookup->next) {
            if (lookup->edit != registry->edit) {
                markdown_core_parser_reread(parser, markdown_core_parser_image(parser, lookup->start));
            }
        }
    }
}

/* A node of a subtree the parse took whole keeps its facts (5.7), which
 * learn where it begins now. */
static void take_facts(void *context, const markdown_core_node *node, uint32_t start) {
    (void)context;
    for (markdown_core_fact *fact = node->facts; fact; fact = fact->sibling) {
        fact->start = start;
    }
}

/* The document is prepared: the headings the parse made declare their
 * labels, the roots whose lookups are answered otherwise now are read
 * again, and the subtrees the parse took list their declarations. */
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_prepare(headings, parser);
    }
    if (!parser->error) {
        reread_lookups(parser);
    }
    for (size_t i = 0; i < parser->took_count && !parser->error && !parser->reread; i++) {
        if (!markdown_core_publication_take(&((document_state *)self->state)->publication, parser->took[i].node,
                                            parser->took[i].start, take_facts, NULL)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
}
/* A node is complete: it numbers the nodes it holds, and the headings note
 * the explicit anchors among them. */
static void complete_node(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_member *member, uint32_t start) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!markdown_core_complete_node(parser, &((document_state *)self->state)->publication, member, start,
                                     headings ? markdown_core_headings_observe : NULL, headings)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* A numbered node waits on nothing any more: it settles. */
static void settle_member(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_member *member) {
    markdown_core_settle_member(parser, &((document_state *)self->state)->publication, member);
}
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_finish(headings, parser);
        markdown_core_headings_dispose(headings);
    }
    /* A parse that reads a node again leaves the edit's marks to the next. */
    if (!parser->reread) {
        markdown_core_registry_settle(parser->registry);
    }
}
static void publish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_publication *publication = &((document_state *)self->state)->publication;
    if (!markdown_core_publish_tree(parser, publication)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    markdown_core_publication_dispose(publication, parser->pool);
}
static void read_document_prefix(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_properties_parse(&((document_state *)self->state)->properties, parser);
}

const markdown_core_element MARKDOWN_CORE_ELEMENT_DOCUMENT = {
    .peers = DOCUMENT_PEERS,
    .state_size = sizeof(document_state),
    .name = "document",
    .init_document = init_document,
    .dispose_document = dispose_document,
    .read_document_prefix = read_document_prefix,
    .prepare_document = prepare_document,
    .finish_document = finish_document,
    .publish_document = publish_document,
    .complete_node = complete_node,
    .settle_member = settle_member,
};
