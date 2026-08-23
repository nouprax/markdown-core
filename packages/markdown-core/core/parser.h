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

/* THE CONCRETE RECORD SET (requirement 11a).
 *
 * A region is a byte range of the normalized source with exactly one owner and
 * exactly one role. Every block-level byte is in exactly one region, so the
 * regions on a line tile it and the regions in order reproduce the source.
 *
 * The three roles are what the byte DID, not what it looks like:
 *
 *   MARKER     the bytes that made the owner the kind of thing it is, and are
 *              not its content: `> `, `- `, `#`s, a fence, `[^label]:`.
 *   CONTENT    the bytes that went into the owner's content buffer, and so are
 *              the bytes its children are cut from.
 *   DISCARDED  the bytes the parse read and kept nowhere -- indentation
 *              stripped from a continuation line, the trailing hashes of a
 *              closed ATX heading, a line ending nothing owns. They still have
 *              an owner: the block they were read inside.
 *
 * A region may be REFINED -- split into adjacent regions covering the same
 * bytes -- which is how an extension takes credit for part of a span without
 * breaking the tiling. It may never be moved and never be deleted. */
#ifndef MARKDOWN_CORE_REGION_ROLE_TYPEDEF
#define MARKDOWN_CORE_REGION_ROLE_TYPEDEF
typedef enum markdown_core_region_role {
    MARKDOWN_CORE_REGION_ROLE_MARKER = 0,
    MARKDOWN_CORE_REGION_ROLE_CONTENT = 1,
    MARKDOWN_CORE_REGION_ROLE_DISCARDED = 2
} markdown_core_region_role;
#endif

typedef struct {
    /* Offset in the parser's normalized source, and the length in bytes. */
    bufsize_t start;
    bufsize_t length;
    /* The node these bytes belong to. Never NULL: bytes that belong to no
     * block belong to the document. */
    struct markdown_core_node *owner;
    uint8_t role;
} markdown_core_region_record;

/* ONE INLINE CLAIM: the bytes [`from`, `to`) of the block's CONTENT buffer are
 * `owner`'s, in role `role`.
 *
 * Requirement 11b's law is that every byte of every block's CONTENT region is
 * owned by exactly one INLINE node or by the block itself, and the inline phase
 * is the only thing that knows which. Claims are made as the bytes are read,
 * accumulated for the block being parsed, and applied once when its inlines are
 * done -- because emphasis and brackets are resolved at the END of the block,
 * and a `*` that turns out to open an emphasis was a `Text` node when it was
 * read. LATER CLAIMS WIN, which is exactly that: `S_insert_emph` re-claims the
 * delimiter bytes for the node it just made.
 *
 * THE ROLE IS DERIVABLE AND IS STATED ONCE: a byte is CONTENT when it survives
 * into a node's literal AS ITSELF, and MARKER when it does not. That decides
 * every case the requirement enumerates without a second list -- a matched
 * delimiter run, a matched bracket, a backslash, an entity, a destination, a
 * title and a smart-punctuation substitution all fail it; an UNMATCHED `*`
 * passes it, because the node's literal is that byte. */
typedef struct {
    bufsize_t from;
    bufsize_t to;
    struct markdown_core_node *owner;
    uint8_t role;
} markdown_core_inline_claim;

/* THE DIAGNOSTIC LIST (requirement 13).
 *
 * A parse produces, beside the two total views, an ORDERED list of
 * `(severity, code, scope, message)`. One law governs it:
 *
 *   RECORDING THE LIST CHANGES NOTHING THE PARSE BUILDS.
 *
 * For every input the semantic tree and the concrete records are byte-identical
 * with recording on and off. That is the whole of it, and it is what makes the
 * list an observation rather than a second engine.
 *
 * THERE IS NO TRUNCATED LIST. An allocation this list cannot afford abandons
 * the parse, exactly as one the region set cannot afford does -- OWNER RULING,
 * 2026-08-24: "we do not want fallback when OOM happens; the parser should
 * return an error rather than return a fallback." Either the parse produced its
 * complete diagnostics, or there is no document to hang them on.
 *
 * WHAT EARNS A DIAGNOSTIC is one sentence, and it is a fact about the two
 * views rather than a list of syntax rules:
 *
 *   A DIAGNOSTIC EXISTS EXACTLY WHERE THE TWO TOTAL VIEWS CANNOT SAY WHAT
 *   HAPPENED.
 *
 * Both views omit nothing, so what is missing is never a BYTE -- it is why a
 * byte that looks like a construct is not one. `:a[b` and `:a` followed by the
 * prose `[b` are byte-identical trees with byte-identical records; nothing but
 * a diagnostic can tell them apart. Conversely an unclosed fence is NOT
 * diagnosed, because `CodeBlock.closed` already says so, and a duplicate
 * definition is not, because both definitions are nodes and "first in document
 * order wins" is derivable from the tree the consumer already has.
 *
 * The corollary that keeps this from being noise: A DIAGNOSTIC NEEDS EVIDENCE
 * THE AUTHOR MEANT THE CONSTRUCT, and the evidence is the construct's own
 * unambiguous opener. A `:` in prose is not evidence; `:name` is. A `[` is not;
 * `[^` and `][` are. */
#ifndef MARKDOWN_CORE_DIAGNOSTIC_TYPEDEFS
#define MARKDOWN_CORE_DIAGNOSTIC_TYPEDEFS
/* WARNING: the author wrote something the engine did not read the way they
 *          meant, and the bytes stand as prose.
 * ERROR:   the author NAMED something that does not exist.
 *
 * Two levels because those are the two things a consumer does differently: a
 * documentation build fails on a cross-reference that names nothing and
 * reports a malformed attribute block. There is no fatal level -- a parse
 * failure is not a diagnostic (requirement 13's converse). */
typedef enum markdown_core_diagnostic_severity {
    MARKDOWN_CORE_DIAGNOSTIC_WARNING = 1,
    MARKDOWN_CORE_DIAGNOSTIC_ERROR = 2
} markdown_core_diagnostic_severity;

typedef enum markdown_core_diagnostic_code {
    /* A directive stood, and the `[` after its name did not become a label. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_LABEL_REJECTED = 1,
    /* A directive stood, and the `{` after it did not become an attribute
     * list. `attributes` is then null, which is also what `:name` alone gives. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_ATTRIBUTES_REJECTED = 2,
    /* A `::name`/`:::name` line did not open a directive block at all, so the
     * whole line is a paragraph and reads exactly like prose. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_REJECTED = 3,
    /* A container directive was closed by the end of the input rather than by
     * a fence. Nothing on the node records this; a code block's `closed` does. */
    MARKDOWN_CORE_DIAGNOSTIC_DIRECTIVE_UNCLOSED = 4,
    /* A delimiter row was found and the header row above it has a different
     * number of columns, so the paragraph is not a table. */
    MARKDOWN_CORE_DIAGNOSTIC_TABLE_REJECTED = 5,
    /* `[text][label]` or `[label][]` naming a label the document does not
     * define. The shortcut form is deliberately absent: `[a]` is
     * indistinguishable from ordinary bracketed prose. */
    MARKDOWN_CORE_DIAGNOSTIC_REFERENCE_UNDEFINED = 6,
    /* `[^label]` naming a footnote the document does not define. */
    MARKDOWN_CORE_DIAGNOSTIC_FOOTNOTE_UNDEFINED = 7,
    /* A label longer than MAX_LINK_LABEL_LENGTH. The author's label is
     * well-formed and the ENGINE refused it, which is a different fact from
     * "no such definition" and needs a different code to say so. */
    MARKDOWN_CORE_DIAGNOSTIC_LABEL_TOO_LONG = 8
} markdown_core_diagnostic_code;
#endif

/* One recorded diagnostic.
 *
 * The extent is stored as a PLACE and not as a byte range, because a place is
 * what the requirement's tuple names and what a consumer follows back to the
 * source; the line index turns one into the other and is complete for every
 * line already fed, which is when a diagnostic is recorded. The message is a
 * slice of the list's own pool, so a record is fixed-size and the pool grows
 * once per message rather than once per record. */
typedef struct {
    int32_t start_line;
    int32_t start_column;
    int32_t end_line;
    int32_t end_column;
    bufsize_t message_start;
    bufsize_t message_length;
    uint8_t severity;
    uint8_t code;
} markdown_core_diagnostic_record;

/* The list itself, moved out of the parser at `finish` exactly as the concrete
 * view is, and released with the document that keeps it. */
typedef struct {
    markdown_core_mem *mem;
    markdown_core_diagnostic_record *entries;
    bufsize_t entries_size;
    bufsize_t entries_alloc;
    markdown_core_strbuf messages;
} markdown_core_diagnostics;

/* THE CONCRETE VIEW, moved out of the parser and into the document.
 *
 * Requirement 12: one parse under two TOTAL views. The semantic one is the
 * tree; this is the other -- the normalized source, its line index, and every
 * region -- and the law that binds them is that every byte of the source is in
 * exactly one region and every region has exactly one owner, so the pair omits
 * nothing. It lived and died with the parser until Step 12, which is why 11a's
 * comment says "requirement 12 is where a document keeps this view".
 *
 * The regions name NODES, so whoever holds this must hold the tree: the
 * document owns both, and freeing it frees both. */
typedef struct {
    markdown_core_mem *mem;
    markdown_core_strbuf source;
    bufsize_t *line_starts;
    bufsize_t line_starts_size;
    markdown_core_region_record *regions;
    bufsize_t regions_size;
} markdown_core_concrete;

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
    /* See the documentation for markdown_core_parser_get_column() in markdown_core.h */
    bufsize_t column;
    /* See the documentation for markdown_core_parser_get_first_nonspace() in markdown_core.h */
    bufsize_t first_nonspace;
    /* See the documentation for markdown_core_parser_get_first_nonspace_column() in markdown_core.h
     */
    bufsize_t first_nonspace_column;
    bufsize_t thematic_break_kill_pos;
    /* See the documentation for markdown_core_parser_get_indent() in markdown_core.h */
    int indent;
    /* See the documentation for markdown_core_parser_is_blank() in markdown_core.h */
    bool blank;
    /* See the documentation for markdown_core_parser_has_partially_consumed_tab() in
     * markdown_core.h */
    bool partially_consumed_tab;
    /* Contains the currently processed line */
    markdown_core_strbuf curline;
    /* THE NORMALIZED SOURCE: every line exactly as S_process_line normalized
     * it -- UTF-8 validated if the option is on, NUL replaced, the line ending
     * a single '\n' whether the author wrote one, wrote CRLF, or wrote nothing
     * at all -- concatenated in order. The document retains it because the
     * concrete record set indexes it: a region is a byte range in HERE, not in
     * whatever buffer the caller fed, and requirement 11a's L3 says the regions
     * in order reproduce it byte for byte.
     *
     * It is NOT the caller's bytes. `markdown_core_parser_feed` may be called
     * with any split, the caller's buffer may be freed the moment feed returns,
     * and two different inputs normalize to the same source -- which is the
     * point: what the tree describes is this. */
    markdown_core_strbuf source;
    /* Where each line begins in `source`: line N starts at line_starts[N - 1].
     * The line index the same requirement names, and the only thing that can
     * turn a source offset back into a (line, column) after the parse. */
    bufsize_t *line_starts;
    bufsize_t line_starts_size;
    bufsize_t line_starts_alloc;
    /* The concrete record set (see markdown_core_region_record), in source order, and
     * how far the line in hand has been attributed. Regions are emitted while
     * the line that contains them is being processed and never afterwards,
     * which is L4: the record set is complete for lines 1..N once line N has
     * been fed. */
    markdown_core_region_record *regions;
    bufsize_t regions_size;
    bufsize_t regions_alloc;
    bufsize_t region_cursor;
    /* When set, markdown_core_parser_finish MOVES the normalized source, the
     * line index and the region set here instead of releasing them, and the
     * caller becomes their owner (requirement 12). */
    markdown_core_concrete *concrete_retain;
    /* REQUIREMENT 13's list, and the switch that decides whether it is built.
     *
     * Recording is OFF unless a caller asks for it, and the law is stated over
     * exactly that axis: for every input the semantic tree and the concrete
     * records are byte-identical with `diagnostics_on` true and false. It is a
     * RETAIN CALL and not a parse option -- Q14 deleted the option surface, and
     * a switch that changed the parse would be a second engine (Q24's own
     * argument). The facade always asks, so diagnostics are part of the model
     * there, the same way the concrete view is. */
    bool diagnostics_on;
    markdown_core_diagnostics diagnostics;
    markdown_core_diagnostics *diagnostics_retain;
    /* When set, markdown_core_parser_finish writes the record set here before
     * releasing it. There is no public reader: requirement 12 is where a
     * document keeps the concrete view, and until then the CLI's `--concrete`
     * and the gate that drives it are the only consumers. */
    FILE *concrete_out;
    /* See the documentation for markdown_core_parser_get_last_line_length() in markdown_core.h */
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
    size_t total_size;
    markdown_core_llist *syntax_extensions;
    markdown_core_llist *inline_syntax_extensions;
    markdown_core_ispunct_func backslash_ispunct;
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
    /* Requirement 11b's inline claims for the block being parsed, and the
     * scratch that resolves them. Both are reused block to block and grow to
     * the largest block seen; neither survives the parse. `paint` is one
     * claim index per CONTENT byte, which is what makes "later claims win" a
     * single pass rather than an interval structure. */
    markdown_core_inline_claim *inline_claims;
    bufsize_t inline_claims_size;
    bufsize_t inline_claims_alloc;
    int32_t *inline_paint;
    bufsize_t inline_paint_alloc;
};

/* Requirement 11b: resolve the inline claims made while `block`'s inlines were
 * parsed and refine its CONTENT regions into them. Implemented beside the rest
 * of the region set in core/blocks.c and called once per block by
 * markdown_core_parse_inlines. */
/* `base` is the claim count at the start of this block's inline parse: the
 * claim list is a STACK, because a directive's label is inline-parsed from
 * inside its own paragraph's inline parse and the inner call must not resolve
 * -- or discard -- the outer one's claims. */
void markdown_core_parser_refine_inline_regions(markdown_core_parser *parser, markdown_core_node *block,
                                                bufsize_t base);

/* Ask `finish` to hand the concrete view over rather than release it. `out` is
 * zeroed here and filled at finish; a parse that fails leaves it empty. */
void markdown_core_parser_retain_concrete(markdown_core_parser *parser, markdown_core_concrete *out);
void markdown_core_concrete_dispose(markdown_core_concrete *concrete);

/* Requirement 13. Turn diagnostic recording on and ask `finish` to hand the
 * list over rather than release it. MUST be called before any byte is fed:
 * recording happens as the lines are read, and `finish` moves what was
 * recorded. `out` is zeroed here and filled at finish; a parse that FAILS
 * leaves it empty, which is the requirement's converse -- a parse failure is
 * not a diagnostic. */
/* Record one diagnostic at the place `start_line`:`start_column` ..
 * `end_line`:`end_column`, counted the way every other scope in the engine is.
 * `message` is a sentence the code cannot say; `subject`, when non-NULL, is an
 * excerpt of the source it is about and is appended in quotes, cut at a
 * code-point boundary.
 *
 * A no-op when recording is off. An allocation it cannot make abandons the
 * parse: see the list's own comment above. */
void markdown_core_parser_diagnose(markdown_core_parser *parser, markdown_core_diagnostic_severity severity,
                                   markdown_core_diagnostic_code code, int start_line, int start_column, int end_line,
                                   int end_column, const char *message, const unsigned char *subject,
                                   bufsize_t subject_length);

/* The same over the LINE IN HAND, from line offset `from` to its last
 * non-space byte -- the block phase's form, where an offset IS a column. */
void markdown_core_parser_diagnose_line(markdown_core_parser *parser, markdown_core_diagnostic_severity severity,
                                        markdown_core_diagnostic_code code, const unsigned char *input, bufsize_t len,
                                        bufsize_t from, const char *message, const unsigned char *subject,
                                        bufsize_t subject_length);

void markdown_core_parser_retain_diagnostics(markdown_core_parser *parser, markdown_core_diagnostics *out);
void markdown_core_diagnostics_dispose(markdown_core_diagnostics *diagnostics);

/* The one writer of the diagnostic wire format, shared by the CLI and by
 * anything that has to read it back. One `diagnostics` header row and one
 * `diagnostic` row per entry, in the order they were recorded. */
void markdown_core_diagnostics_write(const markdown_core_diagnostics *diagnostics, FILE *out);
/* The stable spelling of a code, for the wire format and for a consumer that
 * would otherwise keep its own table. NULL for a value no version defines. */
const char *markdown_core_diagnostic_code_string(markdown_core_diagnostic_code code);

/* `to` takes `from`'s regions, with a forward-only cursor shared across a run
 * of merges. Pass -1 to have it initialised from `from`'s own start. */
void markdown_core_parser_absorb_regions(markdown_core_parser *parser, markdown_core_node *from, markdown_core_node *to,
                                         bufsize_t *cursor);

#ifdef __cplusplus
}
#endif

#endif
