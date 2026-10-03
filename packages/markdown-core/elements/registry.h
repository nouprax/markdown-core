#ifndef MARKDOWN_CORE_REGISTRY_H
#define MARKDOWN_CORE_REGISTRY_H

#include <facts.h>
#include <map.h>
#include <parser.h>

#ifdef __cplusplus
extern "C" {
#endif

/* THE SESSION'S REGISTRIES (docs/plans/2026-09-29-incremental-parsing.md,
 * 5.7): a source-ordered sequence of facts per kind (facts.h), and an index
 * of the keys the facts declare and the lookups ask for.
 *
 * A key is a group and a label: a reference label, which explicit
 * definitions and then headings declare; a footnote label; a specimen id;
 * and an anchor family, the spellings whose stem is the same once trailing
 * `-N` groups are stripped, which explicit anchors reserve and headings take
 * their computed anchors from. An entry lists the facts of its key in their
 * registry's order, so its winner is its first fact, and the lookups that
 * asked for it.
 *
 * A PARSE IS A ROUND. The facts of the nodes it takes stay where they are;
 * the facts of the nodes it reads are DROPPED and the ones it declares are
 * FRESH. A key whose facts changed answers its lookups from the facts that
 * stay and the fresh ones, by position; every other key answers from its
 * entry. Once the parse is complete, the anchor families whose facts changed
 * are assigned again in source order, and each key whose answer changed
 * TOUCHES the lookups of it that a taken node made, as does a taken heading
 * whose anchor changed: the round is discarded, and the session parses again
 * with those places marked as an edit marks them (5.2), so the nodes there
 * are read. A lookup a round makes is answered by that round, and a heading
 * it reads takes its anchor in it, so a round touches only taken nodes, and
 * every touched node is read in every later round: the rounds end. A round
 * that touches nothing commits: the registries are rebuilt from the runs of
 * the taken facts and the fresh facts, and the index follows. */

typedef enum {
    MARKDOWN_CORE_KEY_REFERENCE,
    MARKDOWN_CORE_KEY_FOOTNOTE,
    MARKDOWN_CORE_KEY_SPECIMEN,
    MARKDOWN_CORE_KEY_FAMILY
} markdown_core_key_group;

typedef struct {
    markdown_core_fact **values;
    size_t count, capacity;
} markdown_core_fact_list;

typedef struct markdown_core_registry_entry {
    /* The group's byte, then the label. Owned. */
    unsigned char *key;
    bufsize_t key_length;
    /* The facts of the key, each list in its registry's order: a reference
     * label's explicit definitions and headings; a footnote's or specimen's
     * definitions; a family's headings and anchors. */
    markdown_core_fact_list lists[2];
    /* The first of the lookups of the key. */
    markdown_core_fact *lookups;
    /* The round that touched the entry last, the entry it touched before
     * it, and in that round: whether the key's facts changed, and the fresh
     * ones; whether a taken heading's anchor changed its answer; and its
     * answer once a lookup asked for it. */
    uint64_t round;
    struct markdown_core_registry_entry *touched;
    bool moved, changed, answered;
    markdown_core_fact_list fresh;
    markdown_core_fact *answer;
} markdown_core_registry_entry;

typedef struct markdown_core_registries {
    /* The registry of each kind, NULL while it holds nothing, and the
     * footnote and specimen definitions that win their labels, by label. */
    markdown_core_node *registries[MARKDOWN_CORE_FACT_KINDS];
    markdown_core_node *labels[2];
    markdown_core_key_index index;
    /* THE ROUND: its number, the facts it declared and dropped, by kind, the
     * entries it touched, and the places it touched. */
    uint64_t round;
    markdown_core_fact_list fresh[MARKDOWN_CORE_FACT_KINDS], dropped[MARKDOWN_CORE_FACT_KINDS];
    markdown_core_registry_entry *touched;
    uint32_t *touches;
    size_t touch_count, touch_capacity;
    /* What the round staged to commit (registries_stage). */
    markdown_core_registry_builder builders[MARKDOWN_CORE_FACT_KINDS];
    markdown_core_node *built[MARKDOWN_CORE_FACT_KINDS], *built_labels[2];
    /* A label in its normal form, and a key. */
    markdown_core_strbuf label, key;
} markdown_core_registries;

void markdown_core_registries_init(markdown_core_registries *registries);
void markdown_core_registries_dispose(markdown_core_registries *registries, markdown_core_node_pool *pool);

/* The round of the parse. `begin` starts it; `end` gives back what it did
 * not commit, and runs at the end of every parse. */
void markdown_core_registries_begin(markdown_core_parser *parser);
void markdown_core_registries_end(markdown_core_parser *parser);

/* DECLARATIONS, each at `position`. A failure fails the parse. */
/* A reference definition of `label`, as written, stating `resource`, whose
 * hold it takes. */
void markdown_core_registries_declare_reference(markdown_core_parser *parser, markdown_core_chunk *label,
                                                markdown_core_resource *resource, uint32_t position);
/* A heading, declaring the reference label `label`, as written, with the
 * target `resource`, whose hold it takes, or no label when `resource` is
 * NULL. NULL when the declaration failed. */
markdown_core_fact *markdown_core_registries_declare_heading(markdown_core_parser *parser, markdown_core_chunk *label,
                                                             markdown_core_resource *resource, uint32_t position);
/* A heading's computed anchor base, `length` bytes the fact copies. */
void markdown_core_registries_heading_base(markdown_core_parser *parser, markdown_core_fact *heading,
                                           const unsigned char *base, bufsize_t length);
/* A footnote or specimen definition, which the fact holds. */
void markdown_core_registries_declare_definition(markdown_core_parser *parser, markdown_core_node *node,
                                                 uint32_t position);
/* An explicit anchor spelled `anchor`, which the fact copies. */
void markdown_core_registries_declare_anchor(markdown_core_parser *parser, const markdown_core_chunk *anchor,
                                             uint32_t position);

/* After the block parse and the headings' labels: the facts of the nodes
 * the parse read are dropped. */
void markdown_core_registries_seal(markdown_core_parser *parser);

/* LOOKUPS, each recorded at the start of the inline root being parsed
 * (parser.h, `lookup_at`). */
/* The resource the reference label `label`, as written, resolves to, or
 * NULL. */
markdown_core_resource *markdown_core_registries_reference(markdown_core_parser *parser, markdown_core_chunk *label);
/* Whether a footnote definition declares the label `label`, as written;
 * `*normal` is then the label's normal form, which the registries keep. */
bool markdown_core_registries_footnote(markdown_core_parser *parser, markdown_core_chunk *label,
                                       markdown_core_chunk *normal);
/* Whether a specimen definition declares the id `id`. */
bool markdown_core_registries_specimen(markdown_core_parser *parser, const markdown_core_chunk *id);

/* Once the inline parse is done and every fresh heading has its base: the
 * families whose facts changed are assigned, each fresh heading taking its
 * anchor. */
void markdown_core_registries_assign_anchors(markdown_core_parser *parser);
/* Once the fresh headings' targets hold their anchors: the keys whose
 * answer changed touch their lookups. The places to touch are the
 * revision's (parser.h); when there are any, the round ends there. */
void markdown_core_registries_resolve(markdown_core_parser *parser);
/* Publishing replaced the fresh definition that starts at `start` with the
 * old `node`, equal to it. */
void markdown_core_registries_remap(markdown_core_parser *parser, uint32_t start, const markdown_core_node *node);
/* COMMITTING A ROUND is in two steps, as publishing is (ast_internal.h):
 * `stage` builds what can fail beside what the registries hold, false when
 * storage ran out, and `commit` puts it in place, which nothing can fail.
 * What a round staged and did not commit goes at its end. */
bool markdown_core_registries_stage(markdown_core_parser *parser);
void markdown_core_registries_commit(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
