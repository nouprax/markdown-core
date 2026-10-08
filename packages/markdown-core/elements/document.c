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
/* A ROOT WHOSE LOOKUPS ARE ANSWERED OTHERWISE (docs/plans/2026-09-29-
 * incremental-parsing.md, 5.7): the node, and where it began in the old
 * source. */
typedef struct {
    const markdown_core_node *node;
    uint32_t start;
} again_root;

static int again_order(const void *left, const void *right) {
    const again_root *a = left, *b = right;
    if (a->start != b->start) {
        return (a->start > b->start) - (a->start < b->start);
    }
    return ((uintptr_t)a->node > (uintptr_t)b->node) - ((uintptr_t)a->node < (uintptr_t)b->node);
}

static int address_order(const void *left, const void *right) {
    const uintptr_t a = (uintptr_t)*(const markdown_core_node *const *)left;
    const uintptr_t b = (uintptr_t)*(const markdown_core_node *const *)right;
    return (a > b) - (a < b);
}

/* RESOLUTION OF LOOKUPS (5.7). Every definition is declared once the blocks
 * are complete and the headings the parse made have declared their labels,
 * and the questions asked of the registry then that an earlier parse asked
 * are those of the roots the parse took. A key whose answer, whether it is
 * defined, is not the one it gave the old document has each such root
 * parsed again in place, in source order, each once: into `*roots`, with
 * their count. False when the list could not grow. */
static bool lookups_changed(markdown_core_parser *parser, again_root **roots, size_t *count) {
    const markdown_core_registry *registry = parser->registry;
    size_t capacity = 0;
    *roots = NULL;
    *count = 0;
    for (const markdown_core_key *key = registry->marked; key; key = key->marked_next) {
        if (key->group == MARKDOWN_CORE_KEY_FAMILY || key->was == markdown_core_key_defined(key)) {
            continue;
        }
        for (const markdown_core_fact *lookup = key->lookups; lookup; lookup = lookup->next) {
            if (lookup->edit == registry->edit) {
                continue;
            }
            again_root *grown = markdown_core_reserve(*roots, &capacity, *count + 1, sizeof(*grown));
            if (!grown) {
                return false;
            }
            *roots = grown;
            grown[(*count)++] = (again_root){lookup->node, lookup->start};
        }
    }
    if (!*count) {
        return true;
    }
    qsort(*roots, *count, sizeof(**roots), again_order);
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (!kept || (*roots)[kept - 1].node != (*roots)[i].node) {
            (*roots)[kept++] = (*roots)[i];
        }
    }
    *count = kept;
    return true;
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
 * labels; the subtrees the parse took list their declarations but for the
 * content of the roots whose lookups are answered otherwise now, which are
 * parsed again in place, their headings with the others. */
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    markdown_core_publication *publication = &((document_state *)self->state)->publication;
    if (headings) {
        markdown_core_headings_prepare(headings, parser);
    }
    again_root *roots = NULL;
    const markdown_core_node **nodes = NULL;
    size_t count = 0;
    if (!parser->error && !lookups_changed(parser, &roots, &count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (count && !(nodes = markdown_core_alloc(count, sizeof(*nodes)))) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    if (nodes) {
        for (size_t i = 0; i < count; i++) {
            nodes[i] = roots[i].node;
        }
        qsort(nodes, count, sizeof(*nodes), address_order);
    }
    for (size_t i = 0; i < parser->took_count && !parser->error; i++) {
        if (!markdown_core_publication_take(publication, parser->took[i].node, parser->took[i].start, nodes,
                                            nodes ? count : 0, take_facts, NULL)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    for (size_t i = 0; i < count && !parser->error; i++) {
        markdown_core_parse_again(parser, roots[i].node, roots[i].start,
                                  markdown_core_parser_image(parser, roots[i].start));
    }
    markdown_core_free(roots);
    markdown_core_free((void *)nodes);
    if (headings && count && !parser->error) {
        markdown_core_headings_prepare(headings, parser);
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
/* The document is finished: the headings take their anchors, and each node
 * of a subtree the parse took that it replaces gives its place to the node
 * replacing it, unless that is itself (5.7). */
static void finish_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    markdown_core_publication *publication = &((document_state *)self->state)->publication;
    if (headings) {
        markdown_core_headings_finish(headings, parser);
        markdown_core_headings_dispose(headings);
    }
    for (size_t i = 0; i < parser->replacement_count && !parser->error; i++) {
        struct markdown_core_replacement *replacement = &parser->replacements[i];
        markdown_core_node *node = replacement->node;
        if (replacement->member) {
            node = markdown_core_node_retain(replacement->member->node);
            markdown_core_member_release(parser->pool, replacement->member);
        }
        replacement->member = NULL;
        replacement->node = NULL;
        if (node == replacement->old) {
            markdown_core_parser_release_node(parser, node);
        } else if (!markdown_core_publication_splice(parser, publication, replacement->old, node, replacement->start)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    markdown_core_registry_settle(parser->registry);
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
