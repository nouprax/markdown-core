#ifndef MARKDOWN_CORE_PARSER_H
#define MARKDOWN_CORE_PARSER_H

#include <stdint.h>
#include <stdio.h>
#include "references.h"
#include "node.h"
#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LINK_LABEL_LENGTH 1000

/* Where one source line's bytes landed in a block's content buffer.
 *
 * A block's content is the concatenation of the line slices `add_line` copies
 * into it, and the source column a slice starts at is NOT derivable from the
 * block's own `start_column`: the container prefix stripped from a
 * continuation line need not match the one stripped from the first, so
 * `"> foo\nbar"` strips two bytes then none. One mark per `add_line` call
 * records where the slice came from, and `markdown_core_parser_content_place`
 * reads them back.
 *
 * Marks are appended in parse order and only the deepest open block takes
 * lines, so one block's marks are the contiguous run
 * [node->content_mark, node->content_mark + node->content_mark_count). */
typedef struct {
    /* Offset in the owning block's content where this slice begins. */
    bufsize_t content_offset;
    /* The source line the slice was copied from, counted from 1. */
    int line;
    /* The BYTE column on that line the slice begins at, counted from 1. */
    int column;
} markdown_core_line_mark;

struct markdown_core_parser {
    struct markdown_core_mem *mem;
    /* A hashtable of urls in the current document for cross-references */
    struct markdown_core_map *refmap;
    /* The labels this document defines footnotes for (see references.h). The
     * block phase fills it as each definition opens; the inline phase reads it
     * to decide whether a `[^label]` is a call at all. */
    struct markdown_core_map *footnote_defs;
    /* The root node of the parser, always a MARKDOWN_CORE_NODE_DOCUMENT */
    struct markdown_core_node *root;
    /* The last open block after a line is fully processed */
    struct markdown_core_node *current;
    /* See the documentation for markdown_core_parser_get_line_number() in markdown_core.h */
    int line_number;
    /* See the documentation for markdown_core_parser_get_offset() in markdown_core.h */
    bufsize_t offset;
    /* The offset in columns: differs from `offset` inside a tab, which
     * expands to the next multiple of 4 columns. */
    bufsize_t column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    bufsize_t first_nonspace;
    /* `first_nonspace` measured in columns, tabs expanded. */
    bufsize_t first_nonspace_column;
    bufsize_t thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* Whether `offset` sits inside an expanded tab. */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* Length in bytes of the previously processed line, excluding the
     * trailing newline and carriage return. */
    bufsize_t last_line_length;
    /* Accumulates partial feed chunks until a complete line is available;
     * curline holds the normalized line currently being parsed. */
    markdown_core_strbuf linebuf;
    /* Options set by the user, see the Options section in markdown_core.h */
    int options;
    /* Sticky allocation-failure flag: once any parse structure is lost,
     * markdown_core_parser_finish reports the whole parse as failed (NULL)
     * instead of returning a silently truncated document. */
    bool oom;
    bool last_buffer_ended_with_cr;
    markdown_core_llist *syntax_extensions;
    markdown_core_llist *inline_syntax_extensions;
    /* THE EXTENSION SET's generation (T9 amendment): advanced by every
     * attach. An attach changes what a projection produces for every block,
     * closed ones included, and a closed block's stamp never moves -- the
     * write clock cannot carry this axis, so the cache key carries it
     * directly, the same shape as a map's generation (T4). An attach that
     * fails half-way still counts: a spurious invalidation is a slow feed
     * where a missed one is a wrong tree. */
    size_t extension_generation;
    /* Inline special-character tables for this parser: the core defaults plus
     * the special/emphasis-skip characters of the attached inline extensions.
     * Parser-local so concurrent parsers with different extension sets never
     * observe each other's characters. */
    int8_t special_chars[256];
    int8_t skip_chars[256];
    /* The content-to-source map (see markdown_core_line_mark): one run per
     * block that took lines, appended in parse order. It is read while the
     * parse is still running -- the block phase reads it as blocks close and
     * the inline phase reads it before markdown_core_parser_finish resets --
     * and it is released with the rest of the per-parse state. */
    markdown_core_line_mark *line_marks;
    bufsize_t line_marks_size;
    bufsize_t line_marks_alloc;
    /* THE WRITE CLOCK (T3): advanced by every write to a CST block, and read
     * into the block's `stamp`. Every write happens on the open spine, so the
     * spine is stamped once per processed line as well as at each write --
     * which is what covers an extension's opaque state, written where the
     * core cannot see it. Wraps at 2^32 writes in one parse, which is more
     * lines than a parse can be handed. */
    uint32_t write_clock;
    /* THE IDENTITY MINT (T2): counts the block ids handed out this parse.
     * Advanced only by the block phase -- a projection never mints -- so the
     * ids are a fact about the document rather than about how its bytes
     * arrived, which is what makes them chunking-stable (F11). */
    uint32_t block_ids_minted;
    /* THE PROJECTION CACHE's switches and ledger (T9). `no_projection_cache`
     * is for a runner that plays the cache's part itself or measures without
     * it; it survives the reset, as the options do. The counters are per
     * parse and read before `finish` resets them. */
    bool no_projection_cache;
    size_t cache_hits;
    size_t cache_misses;
    /* THE DERIVE PATH'S WORK LEDGER (#169): one increment on a path that
     * already exists, per unit of work a derivation does, so that a gate
     * can hold F27's bound -- a steady-state derivation is O(open +
     * changed) -- in counts rather than through a wall-clock ratio, under
     * the sanitizer presets as well; and so that a term the miss ledger
     * cannot see (one unstorable block turning every later hit from a
     * memcpy entry into a per-child hold, #170) shows as the count it is.
     * Per parse, zeroed with everything else at the reset, read by the
     * runners as the difference around one derivation. Nothing branches on
     * a counter; the Release build pays the adds and nothing else. */
    struct markdown_core_derive_ledger {
        /* CST nodes the clone walk stood on (S_clone_block_node calls,
         * hits included) and the nodes it built (hits excluded). */
        size_t clone_visited;
        size_t clone_built;
        /* Entries a memo run served by memcpy (S_spine_consume). */
        size_t memo_served;
        /* CST children counted to size a vector (S_vec_open): the one
         * Theta(width) term the memcpy leaves, stated so it stays exact. */
        size_t children_counted;
        /* Spine table slots compared, at the consume and at the record. */
        size_t spine_slots;
        /* Pairs the record proved or refused (S_spine_memo_record_level). */
        size_t record_paired;
        /* Store pass frames pushed, entries tested, ORIGIN nodes sorted. */
        size_t store_frames;
        size_t store_visited;
        size_t store_dispatched;
        /* Row compares in S_names_row. */
        size_t name_probes;
        /* Tail queue entries drained, (block, extension) offers asked, and
         * children stepped by the tail's own walks -- S_has_inline_child,
         * the label pass, the numbering. */
        size_t tail_pops;
        size_t tail_offers;
        size_t tail_cursor_steps;
        /* Fresh blocks whose inlines were parsed. */
        size_t inline_parses;
    } ledger;
    /* THE DERIVATION'S ARENA (#161), set only for the span of one
     * `derive_tree` call so the clone can see it; the arena itself leaves on
     * the derived root. NULL whenever the parser is at rest. */
    markdown_core_node_arena *derive_arena;
    /* THE SPINE'S CHILD MEMOS (#161, F25, generalized by F27): one stable
     * prefix of SHARED children per OPEN container on the spine -- the
     * document always, an open list or quote while it grows -- recorded
     * after a derivation and consumed by the next for one memo hold and a
     * memcpy instead of a freshness check, a hold and a release per
     * closed child. THE TABLE IS THE SPINE (review-found, round 3): slot k
     * names the spine container at depth k, the document at 0, and holds
     * the parser's own hold on that container's memo -- NULL while the
     * level has no proved prefix. The clone asks for a container's run at
     * the depth it counted on the way down, and the record retakes the
     * table by the same walk down `last_child`, so both pay one index per
     * level where a scan of the table per container was quadratic in the
     * depth. A slot naming another container is a miss, never a wrong
     * tree: the pointer is COMPARED, dereferenced only while its
     * container is on the spine, and a CST container is freed only with
     * the parse. Containers close leaf-first, so the levels that left the
     * spine are always the table's suffix, released whole where the record
     * stops (trees still holding a memo keep it alive). The table's length
     * is the depth open at the last record. */
    struct markdown_core_spine_memo {
        const markdown_core_node *container;
        markdown_core_child_memo *memo;
    } *spine_memos;
    size_t spine_memo_size;
    size_t spine_memo_alloc;
    /* THE STORE PASS's frames (F27): one per level of the fresh subtree
     * being walked after the drain, so the pass is iterative at any
     * nesting. Reused across the projections of one parse; released with
     * the parse. */
    struct markdown_core_store_frame {
        markdown_core_node *node;
        /* The resume position among the node's children: the index for a
         * vector container, the next sibling pointer for an intrusive
         * one. */
        size_t next_index;
        markdown_core_node *next_intrusive;
    } *store_stack;
    size_t store_stack_size;
    size_t store_stack_alloc;
    /* THE PER-BLOCK TAIL'S QUEUE (T18): the blocks a projection's walk found
     * tail work for, in EXIT order, acted on after the walk -- a hook may
     * replace or remove the block, and the walk must not be standing on it
     * when it does. Reused across the projections of one parse; released
     * with the parse. */
    markdown_core_node **tail_queue;
    size_t tail_queue_size;
    size_t tail_queue_alloc;
    /* THE DERIVE'S FRESH LIST (#161): every node the clone BUILDS, in clone
     * (pre-)order; the retained nodes it reuses never enter. Armed only for
     * the span of one `derive_tree`, so the projection can serve exactly
     * the built set instead of walking the whole width past the shared
     * blocks. */
    markdown_core_node **fresh_queue;
    size_t fresh_queue_size;
    size_t fresh_queue_alloc;
    bool fresh_queue_armed;
    /* THE NAME MASKS (F15, #161, review-found): which attached extensions
     * declared a given answered name, as a bitset in `syntax_extensions`
     * list order -- `tail_mask_words` words per row, so EVERY extension
     * follows the same algorithm at any count -- plus the fixed row of
     * `"*inlines"` declarers. One lookup per tail replaces the per-(block x
     * extension) memo scan the old shape paid on every projection -- 14% of
     * a hit-dominated feed, measured. Keyed on the name's POINTER -- every
     * `get_type_string` answers a literal -- and per parser, so parsers on
     * different threads share nothing. Rebuilt lazily when
     * `extension_generation` moves (`tail_mask_generation` is that
     * generation plus one, so zero means never built); the row table grows
     * on demand, so a name never falls back to a second code path. A
     * rebuild that cannot allocate poisons the parse (`oom`), the same
     * answer every other lost allocation gives. Row 0 of the pool is the
     * inlines row; name rows follow. */
    size_t tail_mask_words;
    uint64_t *tail_mask_pool;
    const char **tail_name_rows;
    size_t tail_name_row_size;
    size_t tail_name_row_alloc;
    size_t tail_mask_generation;
};

/* THE PROJECTION (§12.1): a new tree derived from the parser's CST -- the
 * block tree, each block's content bytes -- against `refmap` as it now
 * stands, inlines resolved, consolidation, the extension postprocessors and
 * the comment strip applied. The CST is not written; the caller owns and
 * frees the result. NULL on allocation loss, with `parser->oom` set.
 *
 * Internal: this is the RE-projection, what a snapshot accessor calls while
 * the parser lives on. `finish` shares its body but not its clone -- the last
 * projection is taken in place on the CST (T1), because nothing can observe
 * the CST afterwards. Not part of the public surface. */
/* Stamp `node` with the next reading of the write clock (T3). Called by the
 * core at every write it makes to a CST block and by an extension at a
 * retype, and by the line loop over the whole open spine. */
void markdown_core_parser_touch(markdown_core_parser *parser, markdown_core_node *node);

/* Give `node` the next block identity (T2). Called by `add_child` for every
 * block the block phase opens, and wherever a block is born outside it -- the
 * root, a reference definition, a table's lead paragraph. */
void markdown_core_parser_mint_block_id(markdown_core_parser *parser, markdown_core_node *node);

markdown_core_node *markdown_core_parser_derive_tree(markdown_core_parser *parser, markdown_core_map *refmap);

/* Close the stream without projecting it (#162): the held partial line is
 * processed and every block finalized, so the CST stands sealed for a
 * DERIVATION -- the delta seal's path, whose diff is by pointer identity
 * against the last derived tree and so cannot use `finish`'s in-place
 * projection. Answers false when the parse lost an allocation (nothing to
 * derive) or was already finished. Once per parser; `finish` is not a path
 * after it -- free the parser. Not part of the public surface. */
bool markdown_core_parser_close(markdown_core_parser *parser);

#ifdef __cplusplus
}
#endif

#endif
