#ifndef MARKDOWN_CORE_NAMES_H
#define MARKDOWN_CORE_NAMES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "array.h"
#include "buffer.h"
#include "chunk.h"
#include "map.h"
#include "measure.h"
#include "node.h"
#include "tree.h"

/* THE NAMES SERVICE (names.c): the engine names every node an element asks
 * a name for, as anchors.md says -- the base its text projects to, or the
 * base, a hyphen and a suffix -- past the explicit anchors the symbols
 * reserve, across a parse and its edits. The element knows its
 * node's grammar: where the node keeps its name, and what its text projects
 * to. The service knows the rest: the order of the nodes, the names taken,
 * and the names an edit moves. */

struct markdown_core_parser;
struct markdown_core_element_instance;

/* THE SPELLING OF NAMES (element.h, `spelling`): the base of a node whose
 * text projects to nothing, `empty`, which a name borrows, and the byte
 * that parts a base from its suffix's digits, `separator`. The dialect's
 * last element that declares a spelling spells every name; a dialect with
 * none names no node. */
typedef struct markdown_core_name_spelling {
    markdown_core_strbuf empty;
    unsigned char separator;
} markdown_core_name_spelling;

/* WHAT A NAME OWNER DECLARES (element.h, `names`): `value`, where its node
 * keeps its name (node.h, A NODE'S NAME), which its kind's facet shows too
 * (element.h, the facet's `named`); `project`, the projection of the node's
 * children from `from` up to `to`, appended to `base`, the work it took
 * added to `*work`: false when storage could not be had. */
typedef struct markdown_core_name_ops {
    markdown_core_name *(*value)(const markdown_core_node *node);
    bool (*project)(const struct markdown_core_element_instance *self, const markdown_core_node *node, uint32_t from,
                    uint32_t to, markdown_core_strbuf *base, size_t *work);
} markdown_core_name_ops;

/* A node that asks for a name is entered once its block closes, with where
 * it starts. Source order is settled before resolution, independently of the
 * order in which mapped inputs close; the owner that entered it reads it
 * before the names do (markdown_core_names_order), and says how many it
 * read (markdown_core_names_read_to). Nodes remain owned by the tree, and an
 * entry names its node, which keeps its address for its whole life. The
 * parse writes the entry's fields after `start` anew. `owner` is the
 * entering element's place among the dialect's name owners. `authored`
 * says its attributes wrote its anchor. `named` says its node holds the
 * base its text projects to, which its inline content made again since
 * does not; `suffix` is the suffix of the name the names committed for it,
 * which its node shows. `stem` is its base's stem, its place plus one, once an edit keeps
 * them (names.c, THE NAMES AN EDIT MOVES), and `id` its number there, which
 * says where its entry is (`where`). */
typedef struct markdown_core_named {
    markdown_core_node *node;
    uint32_t start, suffix, stem, id;
    uint8_t owner;
    bool authored, named;
} markdown_core_named;

/* THE STEMS (names.c, THE NAMES AN EDIT MOVES): a name that is the base of
 * an entry's anchor, kept as its `measure` alone, its bytes those its
 * witness holds (names.c, stem_text): its first entry's base, else its
 * first child's bytes, else, once it holds neither, the bytes of what it
 * lost last: the base its last entry to go had before the edit, `relic`,
 * or its last child to go, `heir`. The entries of that base, in order, by
 * their numbers, a tree (tree.h); the stem's parent, `up`, the stem's own
 * name less one suffix `-k` of its end, and the present stems whose parent
 * it is, linked through `first`, then `next` and `prior`, each a stem's
 * place plus one or zero; what an edit's pass of the names last saw it in,
 * the pass an anchor the edit made or let go names it for, and the suffix
 * that pass tries next for its entries. A stem whose measure is empty is
 * free. */
typedef struct {
    markdown_core_tree members;
    markdown_core_measure measure;
    const markdown_core_strbuf *relic;
    uint32_t up, first, next, prior, heir, seen, anchored;
    size_t suffix;
} markdown_core_name_stem;

/* What an edit lets go of an entry: its stem and the suffix of its name. */
typedef struct {
    uint32_t stem, suffix;
} markdown_core_name_lost;

/* THE BASE AN ENTRY THE EDIT READ TAKES, before the edit's end (names.c,
 * THE NEW BASES): the bytes of `old`, the base its node had, but for those
 * from `front` to `back` before its end, which `mid` takes the place of; or,
 * with no `old`, `mid` whole. It measures `measure`. */
typedef struct {
    const markdown_core_strbuf *old;
    bufsize_t front, back;
    markdown_core_strbuf mid;
    markdown_core_measure measure;
} markdown_core_name_base;

/* The base of a node an edit made another kind again (markdown_core_names_retire),
 * kept to its end. */
typedef struct {
    const markdown_core_node *node;
    markdown_core_strbuf *base;
} markdown_core_name_retired;

/* WHAT AN EDIT KEEPS OF THE NAMES (names.c, THE NAMES AN EDIT MOVES):
 * every stem, those free chained from `spare` through `next`, and the index
 * of their names; where each entry of a number has its place (tree.h, A
 * HANDLE), a free number with no leaf and its slot the next free number
 * plus one, as `free` is the first's; where the edit's reading starts and
 * its first entry; the pass under way, the stems the edit touched, to name
 * again, what the entries it let go held, and the new bases of the entries
 * it read, one for each from `fresh` on; and the bases of the nodes the edit
 * made another kind again (markdown_core_names_retire), which their stems'
 * relics read until the edit's end. */
typedef struct markdown_core_name_edit {
    markdown_core_name_stem *stems;
    size_t stem_count, stem_room;
    uint32_t spare;
    markdown_core_key_index index;
    MARKDOWN_CORE_ARRAY(markdown_core_tree_handle) where;
    uint32_t free;
    uint32_t rest;
    size_t fresh;
    uint32_t pass;
    uint32_t *touched;
    size_t touched_count, touched_room;
    markdown_core_name_lost *lost;
    size_t lost_count, lost_room;
    markdown_core_name_base *bases;
    size_t base_count, base_room;
    markdown_core_name_retired *retired;
    size_t retired_count, retired_room;
} markdown_core_name_edit;

/* THE NAMES OF ONE PARSE (the parser's `names`).
 *
 * First, what a mark keeps of them (blocks.c, THE MARK): `count`, the
 * entries the reading so far closed, and `parsed`, of which the first are
 * read by their owners and in source order, before every entry after them.
 * A reading put back to a mark finds the entries the mark found, the
 * entries after them gone.
 *
 * The entries themselves are a tree (tree.h) whose focus is an edit's rest
 * point: its front holds the reading's, in the order they closed, `count`
 * of them, which the front's own count follows once names.c next reads
 * them (names_follow); its back the old tail's, after the rest point, in the
 * text before the edit until the reading takes them back at its splice
 * (`spliced`), or lets them go at its finish; `end` is where that text
 * ended. A parse no edit reopened keeps no back.
 *
 * Then the committed names (names.c, THE COMMITTED NAMES): the names
 * taken, where the reading of the symbols' ANCHOR definitions stands and how
 * many it read, and how many entries are named; the projection and lookup work the assignment of their anchors did,
 * for its complexity gate; what only an edit keeps of the names, made at its
 * first reopen; and the dialect's name owners (dialect.h, `namers`). */
typedef struct markdown_core_names {
    size_t count, parsed;
    markdown_core_tree entries;
    size_t end;
    bool spliced;
    markdown_core_key_index taken;
    size_t named;
    size_t work;
    markdown_core_name_edit *edit;
    const struct markdown_core_element_instance *const *owners;
    const markdown_core_name_spelling *spelling;
} markdown_core_names;

/* THE OWNER'S SIDE. The node `node`, whose block closed, asks the element
 * `self` declares the names of (element.h, `names`) for a name: its entry
 * goes in, numbered in an edit's parse. A failure fails the parse. */
void markdown_core_parser_name(struct markdown_core_parser *parser, const struct markdown_core_element_instance *self,
                               markdown_core_node *node);
/* Whether entries wait for their owners to read them: some closed since the
 * last read, or a reading put back to a mark left some past it. */
static inline bool markdown_core_names_due(const markdown_core_names *names) {
    return names->parsed < names->count || names->entries.front > names->count;
}
/* The entries that wait, from `*first` up to `*count`, put in source order:
 * false, with the parse failed, when storage could not be had. Then the
 * stretch of storage that holds them all, or NULL, and entry `at` of them. */
bool markdown_core_names_order(struct markdown_core_parser *parser, size_t *first, size_t *count);
markdown_core_named *markdown_core_names_span(struct markdown_core_parser *parser, size_t first, size_t count);
markdown_core_named *markdown_core_names_at(struct markdown_core_parser *parser, size_t at);
/* The entry its owner read, and whether its attributes write its anchor;
 * and how many of the entries in source order are read now. */
void markdown_core_names_read(markdown_core_named *entry, bool authored);
void markdown_core_names_read_to(struct markdown_core_parser *parser, size_t parsed);
/* The node, read again (element.h, the finish's `reparse`): whether its
 * attributes write its anchor now, and its base is to be projected again;
 * one whose attributes do keeps no base. */
void markdown_core_names_reread(struct markdown_core_parser *parser, markdown_core_node *node, bool authored);
/* The node, which an edit makes a kind that asks no name, keeps its base
 * aside to the edit's end, which the stem it named reads while its old
 * entry or what that entry let go is a witness (THE STEMS), and which the
 * node a redo makes it again takes up (THE NEW BASES). False when storage
 * could not be had. */
bool markdown_core_names_retire(struct markdown_core_parser *parser, const struct markdown_core_element_instance *self,
                                markdown_core_node *node);
/* The anchor the name shows, in `*anchor`, as `spelling` spells it: its
 * base itself when that is whole and takes no suffix, joined at its first
 * read otherwise; empty for a node that holds none. False, `*anchor`
 * untouched, when storage could not be had for it. */
bool markdown_core_name_anchor(markdown_core_name *name, const markdown_core_name_spelling *spelling,
                               markdown_core_chunk *anchor);

/* THE ENGINE'S SIDE (blocks.c, THE STAGES): the names start with the parse,
 * finish at the parse's end, let go at the release, and
 * follow an edit's reopen at `start` and splice at `point`; false when
 * storage could not be had. */
void markdown_core_names_init(markdown_core_names *names, const struct markdown_core_element_instance *const *owners,
                              const markdown_core_name_spelling *spelling);
void markdown_core_names_finish(struct markdown_core_parser *parser);
void markdown_core_names_release(markdown_core_names *names);
bool markdown_core_names_reopen(struct markdown_core_parser *parser, uint32_t start);
bool markdown_core_names_splice(struct markdown_core_parser *parser, uint32_t point);

#endif
