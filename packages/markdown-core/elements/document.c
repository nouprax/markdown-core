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
    (void)parser;
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_init(headings);
    }
}
static void dispose_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (headings) {
        markdown_core_headings_dispose(headings);
    }
    markdown_core_publication_dispose(&((document_state *)self->state)->publication, parser->pool);
}
/* Roots in tree order (5.7). */
static int tree_order(const void *left, const void *right) {
    const uint64_t a = (*(const markdown_core_node *const *)left)->order->label;
    const uint64_t b = (*(const markdown_core_node *const *)right)->order->label;
    return (a > b) - (a < b);
}

/* RESOLUTION OF LOOKUPS (5.7). Every definition is declared once the parse
 * is complete, and every root it did not take has left the registry. A
 * marked key whose answer, whether it is defined, is not the one a root that
 * asked it was given has each such root parsed again in place, in source
 * order, each once: into `*roots`, with their count. False when the list
 * could not grow. */
static bool lookups_changed(markdown_core_parser *parser, const markdown_core_node ***roots, size_t *count) {
    const markdown_core_registry *registry = parser->registry;
    size_t capacity = 0;
    *roots = NULL;
    *count = 0;
    for (const markdown_core_key *key = registry->marked; key; key = key->marked_next) {
        if (key->group == MARKDOWN_CORE_KEY_FAMILY) {
            continue;
        }
        const bool defined = markdown_core_key_defined(key);
        for (const markdown_core_fact *lookup = key->lookups; lookup; lookup = lookup->next) {
            if (lookup->answer == defined) {
                continue;
            }
            const markdown_core_node **grown = markdown_core_reserve(*roots, &capacity, *count + 1, sizeof(*grown));
            if (!grown) {
                return false;
            }
            *roots = grown;
            grown[(*count)++] = lookup->node;
        }
    }
    if (!*count) {
        return true;
    }
    qsort(*roots, *count, sizeof(**roots), tree_order);
    size_t kept = 0;
    for (size_t i = 0; i < *count; i++) {
        if (!kept || (*roots)[kept - 1] != (*roots)[i]) {
            (*roots)[kept++] = (*roots)[i];
        }
    }
    *count = kept;
    return true;
}

/* The inline stage (5.7): the roots the block stage listed are parsed, and
 * then the roots whose lookups are answered otherwise now are parsed again
 * in place. */
static void prepare_document(const markdown_core_element_instance *self, markdown_core_parser *parser) {
    markdown_core_publication *publication = &((document_state *)self->state)->publication;
    const markdown_core_node **roots = NULL;
    size_t count = 0;
    markdown_core_parse_listed(parser, publication);
    if (!parser->error && !lookups_changed(parser, &roots, &count)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
    for (size_t i = 0; i < count && !parser->error; i++) {
        markdown_core_parse_again(parser, publication, roots[i]);
    }
    markdown_core_free((void *)roots);
}
/* A node is complete: it numbers the nodes it holds, and the headings note
 * the explicit anchors among them. */
static void complete_node(const markdown_core_element_instance *self, markdown_core_parser *parser,
                          markdown_core_frame *frame, uint32_t start) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!markdown_core_complete_node(parser, &((document_state *)self->state)->publication, frame, start,
                                     headings ? markdown_core_headings_observe : NULL, headings)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* A node takes another child: it numbers the children before it that have
 * completed, and the headings note the explicit anchors among them. */
static void number_children(const markdown_core_element_instance *self, markdown_core_parser *parser,
                            markdown_core_frame *frame) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!markdown_core_number_children(parser, &((document_state *)self->state)->publication, frame,
                                       headings ? markdown_core_headings_observe : NULL, headings)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* A node about to read its inline content decides what it continues. */
static void decide_frame(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_frame *frame) {
    if (!markdown_core_decide_frame(parser, &((document_state *)self->state)->publication, frame)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* An inline root's parse begins: the root decides what it continues, and
 * the old nodes its content may continue are offered from the start. */
static void open_root(const markdown_core_element_instance *self, markdown_core_parser *parser,
                      markdown_core_inline_root *root) {
    if (!markdown_core_open_root(parser, &((document_state *)self->state)->publication, root)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* The root's parse takes an old node whole: its draft continues it. */
static void take_old(const markdown_core_element_instance *self, markdown_core_parser *parser,
                     markdown_core_draft *draft) {
    if (!markdown_core_take_old(parser, &((document_state *)self->state)->publication, draft)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* A draft that continued an old node goes: the old node may be continued
 * again. */
static void forget_draft(const markdown_core_element_instance *self, markdown_core_parser *parser,
                         markdown_core_draft *draft) {
    markdown_core_forget_draft(&((document_state *)self->state)->publication, draft);
}
/* A node of an inline root's content is complete: it numbers the nodes it
 * holds, and the headings note the explicit anchors among them. */
static void complete_draft(const markdown_core_element_instance *self, markdown_core_parser *parser,
                           markdown_core_draft *draft) {
    const markdown_core_element_instance *headings = self->peers[DOCUMENT_HEADING];
    if (!markdown_core_complete_draft(parser, &((document_state *)self->state)->publication, draft,
                                      headings ? markdown_core_headings_observe : NULL, headings)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
    }
}
/* The document is finished: the headings take their anchors, each node of a
 * subtree the parse took that it replaces gives its place to the node
 * replacing it, unless that is itself, and each label whose key changed
 * takes its place in its roster (5.7). */
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
        replacement->node = NULL;
        if (node == replacement->old) {
            markdown_core_parser_release_node(parser, node);
        } else if (!markdown_core_publication_splice(parser, publication, replacement->old, node)) {
            markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
        }
    }
    if (!parser->error && !markdown_core_registry_resolve(parser->registry)) {
        markdown_core_parser_fail(parser, MARKDOWN_CORE_PARSE_ALLOCATION_FAILED);
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
    .number_children = number_children,
    .decide_frame = decide_frame,
    .open_root = open_root,
    .complete_draft = complete_draft,
    .take_old = take_old,
    .forget_draft = forget_draft,
};
