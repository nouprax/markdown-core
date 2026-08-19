#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"
#include "delimiter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

/* ONE FLIPPED UNIT: a settled unit whose answer a definition changed, so it
 * was refined again. The unit keeps its identity; its children as they were
 * are kept here, detached and still borrowing the unit's bytes, for the
 * caller to pair the new ones against. When the flip was the close's, the
 * retract does not put the old children back — it takes the flip's children
 * off (kept in `published`, for the caller to pair the unit's next children
 * against) and REFINES THE UNIT AGAIN against the tables as they are then:
 * the same bytes and the same tables give the same answer the old children
 * gave, in new objects. */
/* A UNIT'S SETTLED INLINE PREFIX (the living tree plan §8). A unit that is
 * still growing has an inline stream in two parts, exactly as the document
 * has settled blocks and an open spine: the children entirely before
 * `content` are what any longer buffer would build too, so a later refine
 * KEEPS them — their nodes, ids, hashes, records, probes and diagnostics —
 * and derives only what follows. `content` is the settle point the scan
 * proved (inlines.c); `unit` names whose prefix this is, and NULL means
 * there is none and a refine begins at zero.
 *
 * The counts are what the prefix owns of the three things a refine
 * otherwise rebuilds whole, so the retract truncates them to it instead of
 * throwing them away, and `line`/`column` are the source position at
 * `content`, so a scan that begins there gives its nodes the coordinates a
 * scan from zero would have given them. */
typedef struct markdown_core_inline_frontier {
    struct markdown_core_node *unit;
    struct markdown_core_node *last_child;
    /* WHERE THE REFINE THAT LEFT THIS FRONTIER BEGAN — the child it resumed
     * after, or NULL when it began at zero. This is what a walk over "what
     * this refine produced" must start from, and it is NOT `last_child`:
     * that one is where the NEXT refine may begin, and it stands past
     * children this refine derived a moment ago. A postprocess that reads
     * the wrong one of the two skips its own work (the witness: three `-`
     * text nodes left unconsolidated because the walk began after them). */
    struct markdown_core_node *begin_child;
    markdown_core_bufsize content;
    int line;
    int column;
    size_t concrete;
    size_t probes;
    size_t diagnostics;
} markdown_core_inline_frontier;

typedef struct markdown_core_warm_undo markdown_core_warm_undo;

struct markdown_core_warm_flip {
    struct markdown_core_node *unit;
    struct markdown_core_node *children;  /* the run as it was; dead once paired */
    struct markdown_core_node *published; /* the run the flip published, set by the retract */
};

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* Which footnote labels the document defines. Block parsing registers a
     * definition the moment its container opens; the inline phase, which runs
     * only once every block is closed, asks whether `[^x]` names one. A
     * reference to a label nobody defines is not a footnote at all — it is the
     * literal text the author typed — so this answer decides a node's type,
     * and it has to be the whole document's answer: see
     * markdown_core_footnote_definition_create for why it is a second map
     * rather than a discriminated column of `refmap`. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in
     * markdown-core-extension-api.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in
     * markdown-core-extension-api.h */
    markdown_core_bufsize offset;
    /* Tab-expanded column of the parse position in the current line; one tab
     * advances it to the next multiple of the tab stop, so it can run ahead
     * of `offset`. */
    markdown_core_bufsize column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in
     * markdown-core-extension-api.h */
    markdown_core_bufsize first_nonspace;
    /* Tab-expanded column of the byte at `first_nonspace`. */
    markdown_core_bufsize first_nonspace_column;
    markdown_core_bufsize thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in
     * markdown-core-extension-api.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in
     * markdown-core-extension-api.h */
    bool blank;
    /* The parse position sits inside a tab: `offset` has not passed the tab
     * byte, but `column` has consumed part of its width. */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* Byte length of the most recently finished line excluding its line
     * ending; closing blocks stamp their end column from it. */
    markdown_core_bufsize last_line_length;
    /* Accumulates partial feed chunks until a complete line is available;
     * curline holds the normalized line currently being parsed. */
    markdown_core_strbuf linebuf;
    /* Options set by the user, see the Options section in markdown_core.h */
    int options;
    /* Sticky allocation-failure flag: once any parse structure is lost,
     * markdown_core_parser_finish reports the whole parse as failed (NULL)
     * instead of returning a silently truncated document. */
    bool oom;
    /* A concrete marker capture (markdown_core_parser_capture_marker) lost
     * its allocation on the line being processed. Deferred rather than
     * folded into `oom` immediately: the oom guard between open_new_blocks and
     * add_text_to_container cuts a line short, and several capture sites
     * sit where that skip would strand parser->current on a block the line
     * already finalized. The line completes with consistent structure and
     * S_process_line folds this into `oom` at the line boundary. */
    bool capture_lost;
    /* Whether the close now running can be RETRACTED. A stream's publish
     * closes the open spine to show it and takes it back at the next tick,
     * so a block whose literal is its own content buffer (code, HTML)
     * publishes the buffer itself — the literal borrows it, and the retract
     * hands the borrow back, copying and reallocating nothing. A close the
     * FEED makes, or the terminal one markdown_core_parser_finish makes, is
     * final: it detaches the buffer, because nothing will put it back. Set
     * only around markdown_core_parser_warm_publish's close. */
    bool retractable_close;
    /* Sticky engine-invariant failure. This is separate from allocation loss
     * so facade callers can report MARKDOWN_CORE_ERROR_INTERNAL rather than
     * misclassifying a broken refinement lifecycle as OOM. */
    bool internal_error;
    bool last_buffer_ended_with_cr;
    /* Set by the first feed that delivers any bytes (a zero-length feed does
     * not count); markdown_core_parser_attach_extension refuses new syntax
     * from then on, so the grammar is fixed before parsing begins. */
    bool feed_started;
    markdown_core_llist *extensions;
    /* Immutable for each inline pass and parser-local. It owns compiled
     * trigger buckets and delimiter rule bindings for the attached syntax
     * set, so parsing never scans the extension list to recover an owner. */
    struct markdown_core_inline_config *inline_config;
    /* Parser-lifetime inline scratch. Every inline unit begins at the empty
     * mark and retains lane/record capacity for the next unit. */
    markdown_core_delimiter_engine inline_delimiters;
    /* Where each appended line of the open paragraph came from, so a link
     * reference definition harvested out of that paragraph's accumulated
     * content at finalize can be given the position it was written at.
     *
     * A paragraph is a leaf, so at most one is ever open and one array on the
     * parser suffices — no node grows a field. It is not an asymptotic cost:
     * three ints per line describing a buffer that already holds the line. */
    /* Diagnostics raised while parsing, in the order raised — source order
     * for a parse that refines once; a unit refined again (a definition
     * flipped it) raises its own again at the end, and the document sorts
     * when it takes them. Kept as plain ints because core cannot see the
     * facade's types. One code exists today (a directive's attribute block
     * that did not parse), and the vector stays empty for every document
     * that has none. `unit` is the inline-owning block whose refine raised
     * the diagnostic (NULL for the block phase), so a refine that is undone
     * takes its diagnostics with it; `dead` hides one a close's flip
     * superseded until the retract drops it. */
    struct markdown_core_parser_diagnostic {
        int code;
        int start_line;
        int start_column;
        int end_line;
        int end_column;
        const struct markdown_core_node *unit;
        bool dead;
    } *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    /* The inline-owning block being refined, while it is. */
    const struct markdown_core_node *refining;
    /* THE PROBE SCRATCH: the labels the unit being parsed has asked the
     * definition tables about, handed to the unit as its `probes` when its
     * parse ends. Parser-lifetime storage, empty between units. */
    uint64_t *probe_hashes;
    size_t probe_count;
    size_t probe_capacity;
    /* THE PROBE INDEX (map.h): every unit's probes, threaded by label, so a
     * definition finds the units it flips in the size of that set. Made on
     * first use; released with the parser, and outlives it while any probed
     * node does. */
    markdown_core_probe_index *probe_index;
    /* THE PENDING FLIPS: labels a definition arrived for that changed what a
     * lookup answers, recorded by the block phase where the definition
     * registered and consumed by the next settle or publish, which
     * re-refines the units that asked about them. */
    uint64_t *pending_flips;
    size_t pending_count;
    size_t pending_capacity;
    /* THE FLIPPED UNITS: units a settle re-refined for good because a
     * definition changed their answer, each with its old children — for the
     * caller to pair the new against, then drain with
     * markdown_core_parser_warm_flipped_free. */
    struct markdown_core_warm_flip *flipped;
    size_t flipped_count;
    size_t flipped_capacity;
    /* THE VANISHED PARAGRAPHS: a paragraph that was nothing but definitions
     * is unlinked by its own finalize and kept here — a list through the
     * unlinked nodes' `next`, youngest first, each `prev` the sibling it
     * followed — since a close that is to be undone must put it back, and a
     * record that named it as its open leaf must be told it left rather
     * than read it freed. The close's own is claimed by its record at the
     * publish; the feed's stay until markdown_core_parser_warm_vanished_free
     * (the caller's, once its identity step has looked), a fresh build's
     * settle, or the end of the parse. */
    struct markdown_core_node *vanished;
    /* THE INLINE FRONTIER a refine may begin from. Set by the retract out of
     * the record's leaf entry and consumed by the refine of that same unit
     * later in the same tick; a refine of any other unit ignores it, and
     * every refine leaves its own in its place. It lives on the PARSER for
     * the length of one tick rather than on the node, where it would cost
     * every node in the tree the fields only the open leaf ever uses. */
    markdown_core_inline_frontier inline_frontier;
    /* THE SAME, AS THE LAST PUBLISH LEFT IT — captured at the retract,
     * before this tick's refine moves the one above. The identity step pairs
     * what this close APPENDED, which is everything after the prefix the
     * last close had settled; the retract keeps what THIS close settled.
     * Two questions, two checkpoints, and both are the parser's: it outlives
     * every tick, so nothing has to be copied into a record and reconciled
     * with it afterwards. */
    markdown_core_inline_frontier inline_published;
    /* WHAT THE REFINE NOW ENDING PROVED, and where it began. Every refine
     * writes it; the walks that follow one read `begin_child` from it (the
     * postprocess, the consolidation, an extension's hook — they want what
     * THIS refine produced, which is not what is settled). The step that
     * knows which of its refines was the unit still growing promotes it to
     * the checkpoint above; nothing else may, or the last unit refined in a
     * step — a settled block, a flipped one — would take it over. */
    markdown_core_inline_frontier inline_refine;
    /* THE CLOSE THIS PARSER IS INSIDE, and the one it just came out of. A
     * record is what a close took and how to give it back, which is this
     * parser's state and nothing the caller has to hold: it is written by
     * markdown_core_parser_warm_publish, moves to the second slot when
     * markdown_core_parser_warm_retract gives the parser back, and is freed
     * by markdown_core_parser_warm_commit once the caller has paired the
     * identities off its retired runs — or by the parser's own free. Both
     * are NULL for a parser mid-stream that has never been published from,
     * which is what makes `warm_retracted` say "cold" to a settle. */
    struct markdown_core_warm_undo *warm_published;
    struct markdown_core_warm_undo *warm_retracted;
    struct markdown_core_line_mark {
        markdown_core_bufsize content_offset;
        int line;
        int column;
        /* The byte offset in the normalized line where the append began —
         * `column` tab-expands, concrete records do not. The table
         * extension's look-back header capture maps buffer offsets to
         * normalized-line record columns through this. */
        markdown_core_bufsize byte_offset;
        /* Stand-in spaces the append wrote before the line's own bytes: a
         * lazy continuation whose matched prefixes stopped inside a tab
         * buffers the tab's remaining columns as spaces, so the buffer
         * holds `pad` spaces where the normalized line holds the one tab
         * byte at `byte_offset`. Zero everywhere else — the non-lazy
         * paragraph paths advance byte-wise to first_nonspace, which
         * clears the partial-tab state before the append. */
        int pad;
    } *line_marks;
    size_t line_mark_count;
    size_t line_mark_capacity;
};

/** One open block as it was before a close touched it. `last_child` is the
 * youngest child it had, so anything the close appended can be found without
 * asking the close to have recorded it; after a retract, `retired` is that
 * run — detached, kept alive, and read by nothing but the identity handover
 * of the next publish, which then frees it. */
typedef struct markdown_core_warm_open_block {
    markdown_core_node *node;
    /* NULL FOR A BLOCK THAT OWNS INLINES: its children are derived, and
     * which of them are settled is the parser's checkpoint to say
     * (`inline_frontier`), not a pointer copied in here that every step
     * taking a child would then have to correct. */
    markdown_core_node *last_child;
    /* The sibling before `last_child`, so what a close INSERTS before the
     * youngest child — a definition harvested out of a paragraph, the lead
     * paragraph split off a table — is inside the run a step refines. */
    markdown_core_node *prev;
    markdown_core_node *retired;
    /* Likewise what the close INSERTED before the youngest child — the
     * definitions harvested out of a paragraph, the lead paragraph split off
     * a table — retired at the retract for the next publish's insertions to
     * pair against. */
    markdown_core_node *retired_inserted;
    /* The block itself, when the close's refine REPLACED it — a paragraph
     * that is one display formula is promoted to a formula block, which
     * takes its place while the block is kept here, unlinked, for the
     * retract to put back. `node` then names the survivor. */
    markdown_core_node *replaced;
    /* The survivor, once the retract has put the block back: published, so
     * kept — at the end of the PARENT's retired inserted run, where it stood
     * — for the next publish to pair what takes its place against. Set on
     * the entry above the replaced one while the retract runs; NULL
     * otherwise. */
    markdown_core_node *survivor;
    /* The block VANISHED at the close — a paragraph that was nothing but
     * definitions is unlinked by its own finalize — and is put back at the
     * retract after its parent's youngest-but-one child. */
    bool vanished;
    markdown_core_node *vanished_prev;
    /* THE BLOCK AS IT WAS, WHOLE. A close may write anywhere in the node it
     * closes — a list's tightness, a setext retype, the coordinates it ends
     * at — and the bytes it wrote are not a list anyone can be trusted to
     * keep: the fields this record named by hand were right, but only
     * because something checked (close_retract_exact), and a field added to
     * a node tomorrow would be right by nobody. So the record keeps the node
     * and the retract puts it back, minus the bytes another owner already
     * has: the links its own list surgery computes, the content buffer whose
     * fate `content` below describes, the three vectors a node only points
     * at, the extension payload the extension restores, and the facade's
     * identity fields, which no close writes. */
    unsigned char saved[sizeof(struct markdown_core_node)];
    /* THE FACADE'S, carried here because the spine is its index: the
     * block's own projection as this record PUBLISHED it, written as bytes
     * (extensions/ast.c; a moved content buffer by its length, which is
     * what MARKDOWN_CORE_WARM_CONTENT_MOVED below guarantees suffices), so
     * the next publish is compared against it — a list whose tightness the
     * close recomputes to the same value keeps its revision, a paragraph
     * the feed retyped into a heading takes the tick's, a table counting
     * one more row behind its payload pointer likewise. The engine writes
     * nothing to it and only frees it. */
    unsigned char *published_projection;
    size_t published_projection_size;
    /* The youngest child, WHOLE: a blank line at the close writes "ends with
     * a blank line" onto the current block's youngest child, which is a
     * SETTLED node the record would otherwise not hold at all — and once it
     * is held, it is held the same way as the block, so nothing has to
     * predict which of its fields a close reaches. */
    unsigned char saved_last_child[sizeof(struct markdown_core_node)];
    /* WHAT THE CLOSE DOES TO THE BLOCK'S CONTENT BUFFER, and so how the
     * retract puts it back. */
    enum markdown_core_warm_content {
        /* The buffer stays where it is; the retract truncates the held
         * line's bytes off its end (`content_size`). */
        MARKDOWN_CORE_WARM_CONTENT_KEPT,
        /* The close moves the buffer WHOLE into the block's literal — a code
         * block's, an HTML block's — and the retract moves it back: no byte
         * is copied, so a growing fence costs a tick nothing per line it
         * already holds. An indented code block's close first cuts the
         * trailing blank lines the literal must not hold; those bytes are
         * the copy below, and go back on the end. */
        MARKDOWN_CORE_WARM_CONTENT_MOVED,
        /* The close CONSUMES the buffer — a paragraph beginning with a
         * definition has definitions harvested off its front, an
         * extension's block that takes lines mints its payload from them —
         * and the retract restores it from the copy below, whole. */
        MARKDOWN_CORE_WARM_CONTENT_CONSUMED
    } content;
    /* The bytes the retract puts back that the block's literal will not
     * hold: an indented code block's trailing blank lines (MOVED), or the
     * whole buffer (CONSUMED). NULL when there are none. */
    unsigned char *content_copy;
    markdown_core_bufsize content_copy_size;
    /* A close may RETYPE the block — a setext underline turns the paragraph
     * into a heading, a table's delimiter row turns it into a table with an
     * extension, a payload behind `as.opaque`, and a start moved to its
     * header row. What a retype overwrites is in `saved` with everything
     * else; what is here is the one thing a snapshot cannot hold, the bytes
     * BEHIND the payload pointer (its declared plain-data size) for a block
     * that already carried one. */
    void *opaque_copy;
    size_t opaque_copy_size;
    /* How many marker records the block carried: a line captures markers on
     * a container it continues (a quote's `>`), and the close's held line
     * would leave one more. */
    size_t concrete_count;
    markdown_core_bufsize content_size;
} markdown_core_warm_open_block;

/** What a projection took, so it can be given back — and, once given back,
 * what it left behind for the next projection to inherit from.
 *
 * The open spine root-down as it was before the close, plus everything on
 * the parser that outlives a line: marks, held bytes, line counters, the
 * current block, the CR seam, and how many diagnostics there were. THE
 * PARSER OWNS IT, in whichever of its two slots says what state it is in:
 * `warm_published` (the parser is closed, the record says how to reopen it)
 * and `warm_retracted` (the parser is open again, and `spine[i].retired`
 * holds what the close had appended under each block — the frontier a
 * caller pairs identities from). A caller reads it and never holds it. */
struct markdown_core_warm_undo {
    markdown_core_warm_open_block *spine;
    size_t spine_count;
    struct markdown_core_line_mark *marks;
    size_t mark_count;
    unsigned char *held;
    markdown_core_bufsize held_size;
    int line_number;
    int last_line_length;
    markdown_core_node *current;
    bool last_buffer_ended_with_cr;
    size_t diagnostic_count;
    /* Both definition tables' sizes before the close: what the close
     * harvested is taken back to these. */
    size_t definitions;
    size_t footnotes;
    /* THE FLIPS THE CLOSE MADE: a definition the close registered changed
     * what settled units answered, and those units were re-refined for the
     * projection; each keeps its old inline children here, detached, and the
     * retract refines the unit again as it stood. */
    struct markdown_core_warm_flip *flips;
    size_t flip_count;
    size_t flip_capacity;
};

/** The run of children a spine entry gained since its record was taken:
 * from just after the sibling that preceded its saved youngest child (or
 * from the first child), skipping that youngest child itself — which is the
 * next spine entry, and has an entry of its own. */
/* THE FIRST CHILD OF THE RUN THIS RECORD'S CLOSE APPENDED — which the
 * retract retires and the identity step pairs, one run and one answer. It
 * begins after the unit's SETTLED PREFIX when it has one, because those
 * children are older than this close: the refine that settled them is the
 * one this record was written by, they were minted then, and the next
 * refine begins after them (the living tree plan §8). Otherwise it begins
 * after the youngest child the record saved, as it always did. */
/* THE FIRST CHILD OF THE RUN THIS RECORD'S CLOSE APPENDED to a block — what
 * the retract retires and the identity step pairs. For a block that owns
 * inlines the record saved no youngest child, so the run is every child it
 * has, and the caller narrows it with the parser's checkpoint (parser.h,
 * `inline_frontier`). */
static inline markdown_core_node *markdown_core_warm_appended_first(const markdown_core_warm_open_block *entry) {
    return entry->last_child ? entry->last_child->next : (entry->prev ? entry->prev->next : entry->node->first_child);
}

static inline markdown_core_node *markdown_core_warm_run_first(const markdown_core_warm_open_block *entry) {
    markdown_core_node *first = entry->prev ? entry->prev->next : entry->node->first_child;
    return first == entry->last_child && first ? first->next : first;
}

static inline markdown_core_node *markdown_core_warm_run_next(
    const markdown_core_warm_open_block *entry,
    const markdown_core_node *node
) {
    markdown_core_node *next = node->next;
    return next == entry->last_child && next ? next->next : next;
}

/** THE UNIT IS NOT THE UNIT ITS PREFIX BELONGED TO. A block a step RETYPES
 * keeps its bytes and loses their reading: the inline children a previous
 * tick settled under it describe the block it was, so they go, with the
 * records and probes that came with them, and the frontier that named them
 * is cleared. Their ids retire, which is what a kind change means (the
 * canonical AST's identity contract). The core's own setext retype is not a
 * site: it keeps the reading and drops only the underline, so the prefix
 * still describes the same bytes. */
void markdown_core_parser_drop_inline_prefix(markdown_core_parser *parser, markdown_core_node *unit);

/** The last child of `unit`'s settled inline prefix when a refine RESUMED
 * from it — so a walk over the children may begin after it — and NULL
 * otherwise, including for the refine that first derived that prefix and
 * had to walk all of it. */
markdown_core_node *markdown_core_parser_settled_inline_child(
    const markdown_core_parser *parser,
    const markdown_core_node *unit
);

/** Whether a parser at end of feed can publish: it has a tree and has not
 * failed. Every open state is one a publish can be retracted from (see the
 * note above this function's definition in blocks.c). */
bool markdown_core_parser_warm_eligible_at_eof(const markdown_core_parser *parser);

/** SETTLES what a step closed: refines, once and for good, every unit that
 * is closed and lies in the region a record describes — for each saved open
 * block, deepest first, the children appended past its saved youngest child
 * (whole subtrees, children before their container), then the block itself
 * if it is closed now. Open nodes are descended into and left alone; the
 * document root is never refined. With `before == NULL` the region is the
 * whole closed part of the tree: the form for a fresh parser, whose every
 * closed unit is unrefined.
 *
 * A warm tick calls this after the feed with the record it retracted, so
 * exactly the units the feed closed are refined and keep their inline
 * children — and therefore their identities — from then on. A spine block
 * its own refine replaces (a formula promotion) is swapped for the survivor
 * in the entry, so nothing dangles; the replaced block is freed here, since
 * a settle is for good. */
bool markdown_core_parser_warm_settle(markdown_core_parser *parser);

/* Frees the old children the settle's flips kept, once the caller has paired
 * against them. */
void markdown_core_parser_warm_flipped_free(markdown_core_parser *parser);

/* Frees the paragraphs the feed took (nothing but definitions), once the
 * caller has seen which of them a record named. */
void markdown_core_parser_warm_vanished_free(markdown_core_parser *parser);

/** PUBLISHES a projection from a parser that is still mid-stream: the held
 * partial line is processed for real, every open block is finalized up to
 * the root, and every unit THAT CLOSE closed — the spine, and whatever the
 * held line put under it — is refined. Units the feed closed are the
 * caller's to settle (markdown_core_parser_warm_settle) and are not looked
 * at, which is what keeps a publish O(open spine + held line) rather than
 * O(tree). The parser keeps the record of what the close took, and
 * markdown_core_parser_warm_retract puts it back exactly as it was, so the
 * next chunk continues as if the projection had never been asked for.
 *
 * Returns false, having closed nothing and left the parser untouched, if
 * the record cannot be allocated, if the parser is already published from,
 * or if it has failed (see markdown_core_parser_warm_eligible_at_eof).
 *
 * WHAT MAKES A RECORD RETRACTABLE: every close's effects stay inside the
 * record — see the note above markdown_core_parser_warm_eligible_at_eof in
 * blocks.c for what the record holds, and for the one thing an extension
 * must say for that to be true of its blocks. */
bool markdown_core_parser_warm_publish(markdown_core_parser *parser);

/** Gives back everything the publish took: the blocks it closed are reopened
 * with their end coordinates and content restored, the line counters, the
 * marks, the diagnostics count and the held partial line return to what
 * they were, and what the close had appended under each open block — the
 * spine leaf's tentative inline children included — is DETACHED and kept on
 * the record as `spine[i].retired`, not freed: the next publish hands its
 * identities to whatever takes its place, and frees it then — so first the
 * frontier is made to own its bytes, which its literals borrow from the leaf's
 * content buffer that the next feed will grow. The parser is then fed exactly
 * as if it had never been published from. Returns false, touching nothing,
 * for a parser with nothing published, or with a retracted record its
 * caller has not committed, and — with the parser's sticky allocation
 * bit set — when the frontier could not be given its bytes before anything
 * moved (the record is still the published one), or when, once things have
 * moved, a block could not get its bytes back or the block that had
 * replaced the leaf could not be given its (the parser is not where it was,
 * and the caller's tick fails and takes the chain with it). */
bool markdown_core_parser_warm_retract(markdown_core_parser *parser);

/** Drops the record the parser has retracted, and with it the retired
 * frontier it holds — the caller's signal that it has paired every identity
 * off the last close and the tick may take another. Does nothing for a
 * parser with no retracted record. */
void markdown_core_parser_warm_commit(markdown_core_parser *parser);

/** Refines ONE closed unit: its inlines if it owns any, then its own
 * block-local postprocess. Refining each unit once, as it closes, is what
 * lets settled nodes keep being the same nodes — re-parsing a settled unit
 * would retire and re-mint every inline node it owns, and identity is what a
 * consumer keys on.
 *
 * RETURNS THE NODE NOW AT THE UNIT'S POSITION. A postprocessor may replace a
 * unit (a fenced code block whose info is `formula`, and a paragraph that is
 * nothing but a display formula, both in the formula extension); the
 * replaced unit comes back through `replaced`, unlinked and alive, for the
 * caller to free or to keep. Two rules come with it: anything caching the unit's CHILD
 * pointers must run AFTER this call, because the autolink pass splices that
 * list; and no stamp is performed, which is the caller's to do if the tree
 * may still meet the append diff.
 *
 * Call order is close order — children before the containers that closed
 * them — which markdown_core_parser_warm_settle keeps for its callers. */
markdown_core_node *markdown_core_parser_warm_refine_settled(
    markdown_core_parser *parser,
    markdown_core_node *unit,
    markdown_core_node **replaced
);

/** Everything a projection may read but must not change, in one value: the
 * line counters and sticky failure bits, the held partial line and its
 * pending CR, every node's type, flags, coordinates and content bytes, both
 * definition tables in order, and the paragraph's line marks.
 *
 * The question it answers is whether a parser is EXACTLY where it was — the
 * decidable form of "the projection left no trace" — so a speculative close
 * and its undo can be gated on restoring it bit for bit. Anything added to
 * the parser or to a node must be added here, or the gate goes blind to it.
 *
 * O(tree + text) per call: a gate's budget, not a tick's. */
uint64_t markdown_core_parser_warm_fingerprint(const markdown_core_parser *parser);

/** Maps the content-buffer extent [x0, x1) of the line `mark` records onto
 * that normalized source line: `*column` receives the record column and the
 * return value the record length. The map is affine at slope one except for
 * the mark's stand-in pad: an extent that begins inside the pad begins on
 * the tab byte itself, and every offset past the pad sits one tab byte —
 * not `pad` spaces — after `byte_offset`. `x1` never lands inside the pad,
 * because every extent a caller maps ends on a marker or spelling byte and
 * the pad holds only the append's stand-in spaces. */
markdown_core_bufsize markdown_core_line_mark_extent(
    const struct markdown_core_line_mark *mark,
    markdown_core_bufsize x0,
    markdown_core_bufsize x1,
    markdown_core_bufsize *column
);

/** Records one diagnostic at the given extent. A lost diagnostic is not a
 * lost parse: the tree is unaffected and the document is still correct, it
 * is only missing an underline, so this reports nothing on allocation
 * failure rather than poisoning a parse that otherwise succeeded. */
void markdown_core_parser_record_diagnostic(
    markdown_core_parser *parser,
    int code,
    int start_line,
    int start_column,
    int end_line,
    int end_column
);

/** Applies the core list-item continuation rule to the current line.
 * Extension-owned list items use this to stay in lockstep with plain list
 * items. */
bool markdown_core_parser_match_list_item_prefix(
    markdown_core_parser *parser,
    markdown_core_chunk *input,
    markdown_core_node *container
);

/** Captures one concrete marker record on the node whose ownership region
 * the marker belongs to (11.1). `column` and `length` are byte extents in
 * the current normalized line; the record's line is derived here as the
 * offset from the node's still-absolute start_line. The one capture
 * funnel for block-phase marker material, core and extensions alike: a
 * lost record sets parser->capture_lost, which S_process_line folds into
 * `oom` at the line boundary rather than mid-line (see the field's
 * comment above). */
void markdown_core_parser_capture_marker(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    markdown_core_bufsize column,
    markdown_core_bufsize length
);

/** The look-back variant: identical, but the marker was spelled on
 * `line` (absolute, same numbering as the live parse) rather than the
 * line being processed — the table extension recovers a header row from
 * the paragraph's accumulated content one line after buffering it. */
void markdown_core_parser_capture_marker_at(
    markdown_core_parser *parser,
    markdown_core_node *node,
    uint8_t kind,
    int line,
    markdown_core_bufsize column,
    markdown_core_bufsize length
);

#ifdef __cplusplus
}
#endif

#endif
