#ifndef MARKDOWN_CORE_CONCRETE_RECORDS_H
#define MARKDOWN_CORE_CONCRETE_RECORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "markdown-core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Concrete marker records (incremental-canonical-ast.md 0, 11.1).
 *
 * The block phase consumes marker bytes — `>` runs, list bullets and
 * ordinals, `#` runs, fence lines and their info strings, setext
 * underlines, thematic breaks, `[^label]:` openers, the `[label]:`,
 * destination, and title spellings of a reference definition — and until
 * now kept only what the canonical AST needs (a level, a fence length
 * clamped to 255, an int the leading zeros of an ordinal collapse into).
 * These records keep the marker material itself: for each marker the grammar
 * assigns to a node's own ownership region, one compact record of where
 * it is and what it is. Trivia — indentation, the spacing around a
 * marker, blank-line runs — stays an implicit source gap with no record
 * and no identity, which section 0 permits and the compactness of the
 * region's record vector depends on.
 *
 * Records live on the node that owns the marker per 11.1: the `>` run of
 * a line belongs to its BlockQuote, a bullet to its ListItem (the List
 * groups items but owns no marker bytes of its own, like Document), the
 * fence lines to their CodeBlock, the `[^label]:` opener to its
 * FootnoteDefinition. Storing them on the owning node is what makes the
 * unified CST one physical tree rather than a parallel structure: the
 * records ride the node through adoption, suffix transplant, and the
 * one-shot detach with no id remapping and no synchronization pass
 * (14.1.9).
 *
 * The coordinate encoding is what keeps every record out of every bound
 * that depends on document size (the first layout rule of section 0):
 *
 * - `line` is the offset of the marker's line from the owning node's own
 *   first line, so a suffix reflow that shifts the node moves every
 *   record with it, untouched — the same property the sealed
 *   parent-relative line encoding gives node positions.
 * - `column` and `length` are byte extents within that line as the
 *   parser scanned it: the normalized line (each NUL replaced by the
 *   3-byte U+FFFD, EOL excluded). A normalized-line
 *   offset equals the stored-source offset only until the line's first
 *   replacement. A replaced byte is a non-space byte no marker scanner
 *   accepts, so it never sits inside or before a marker recognized at
 *   the line's first non-space byte — but FENCE_INFO can contain
 *   replacements, FOOTNOTE_OPENER's label can contain them, and
 *   ATX_CLOSER, recognized from the line's end, can sit entirely after
 *   one. Resolving a record against the stored source bytes therefore
 *   composes with the line's replacement positions; the capture gates
 *   re-derive the normalized line for exactly that reason.
 *
 * Records are appended in parse order, which within one node ascends by
 * (line, column); nothing reorders them. `flags` is reserved (always 0)
 * for the recovery material of 14.1.10.
 */

typedef enum markdown_core_concrete_record_kind {
    /** One `>` of a block quote, one record per line it prefixes. Lazy
     * continuation lines carry no `>` and get no record. */
    MARKDOWN_CORE_CONCRETE_BLOCK_QUOTE_MARKER = 1,
    /** A list item's bullet (`-`, `+`, `*`) or its ordinal digits plus
     * delimiter (`.` or `)`), exactly as written — leading zeros and all. */
    MARKDOWN_CORE_CONCRETE_LIST_MARKER,
    /** An ATX heading's opening `#` run; length is the heading level. */
    MARKDOWN_CORE_CONCRETE_ATX_OPENER,
    /** An ATX heading's closing `#` run, when one exists. */
    MARKDOWN_CORE_CONCRETE_ATX_CLOSER,
    /** A setext heading's full `=` or `-` underline run. */
    MARKDOWN_CORE_CONCRETE_SETEXT_UNDERLINE,
    /** A fenced code block's opening fence run, unclamped where
     * `fence_length` saturates at 255. */
    MARKDOWN_CORE_CONCRETE_FENCE_OPEN,
    /** The raw info string of an opening fence, whitespace-trimmed but
     * not unescaped: `as.code.info` is the decoded scalar, this is its
     * spelling. Absent when the fence line carries none. */
    MARKDOWN_CORE_CONCRETE_FENCE_INFO,
    /** A fenced code block's closing fence run; absent when the fence is
     * unclosed. */
    MARKDOWN_CORE_CONCRETE_FENCE_CLOSE,
    /** A thematic break's full construct, first marker byte through last
     * (internal spacing included, trailing whitespace not). */
    MARKDOWN_CORE_CONCRETE_THEMATIC_BREAK,
    /** A footnote definition's `[^label]:` opener. */
    MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER,
    /** A table's full delimiter row (`| :-: | - |`), first marker byte
     * through last, on the Table node itself: 11.1 names the delimiter
     * row the Table's own marker material. Alignments are its decoded
     * scalars; this is their spelling. */
    MARKDOWN_CORE_CONCRETE_TABLE_DELIMITER_ROW,
    /** One `|` cell separator of a header or data row, one record per
     * pipe, on the TableRow that owns the row's marker material. The
     * delimiter row's pipes live inside TABLE_DELIMITER_ROW instead. */
    MARKDOWN_CORE_CONCRETE_TABLE_PIPE,
    /** The backslash of a `\|` escape the table layer collapses while
     * rebuilding a cell's content buffer. It belongs to the TableCell —
     * the cell owns the source backing its inline sequence — and it is
     * block-side because the rebuilt buffer no longer contains the
     * backslash for an inline record to cover. The surviving `|` is
     * content, exactly like the escaped character of an inline ESCAPE. */
    MARKDOWN_CORE_CONCRETE_TABLE_CELL_ESCAPE,
    /** A task item's `[x]`/`[ ]` checkbox trio, on the ListItem whose
     * checked state it sets. The scanner consumes it wherever it fires —
     * every firing records, later lines included; trailing spacing is
     * trivia. */
    MARKDOWN_CORE_CONCRETE_TASK_MARKER,
    /** A block directive's name as spelled after its fence colons;
     * the decoded scalar is the directive name field. */
    MARKDOWN_CORE_CONCRETE_DIRECTIVE_NAME,
    /** The `[` opening a block directive's label. The label's interior
     * belongs to the DirectiveLabel child region, so the brackets are
     * two one-byte records rather than one spanning record. */
    MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_OPEN,
    /** The `]` closing a block directive's label. */
    MARKDOWN_CORE_CONCRETE_DIRECTIVE_LABEL_CLOSE,
    /** A block directive's `{...}` attribute block, braces included; the
     * normalized attributes JSON is the decoded scalar, this is its one
     * source spelling. Absent when the attributes fail to parse, because
     * the braces then stay literal text. */
    MARKDOWN_CORE_CONCRETE_DIRECTIVE_ATTRIBUTES,
    /** One line's segment of a reference definition's `[label]:`, bracket
     * through colon where they sit on that line. A label may span lines,
     * and records are single-line extents, so the spelling is the
     * segments in (line, column) order; `as.definition.label` keeps the
     * raw interior bytes, this is where the construct sits in the source. */
    MARKDOWN_CORE_CONCRETE_REFDEF_LABEL,
    /** A reference definition's destination as spelled — angle brackets
     * included in the `<...>` form. Single-line by grammar, so exactly one
     * record; `as.definition.url` is the decoded scalar. */
    MARKDOWN_CORE_CONCRETE_REFDEF_DESTINATION,
    /** One line's segment of a reference definition's title, quote or
     * paren delimiters included; `as.definition.title` is the decoded
     * scalar. Absent when the definition carries no title. */
    MARKDOWN_CORE_CONCRETE_REFDEF_TITLE
} markdown_core_concrete_record_kind;

typedef struct markdown_core_concrete_record {
    /** Line offset from the owning node's first line (0 = same line). */
    uint32_t line;
    /** Byte offset of the marker within that normalized line. */
    uint32_t column;
    /** Marker length in bytes; never 0. */
    uint32_t length;
    /** A markdown_core_concrete_record_kind. */
    uint8_t kind;
    /** Reserved for 14.1.10 recovery material; always 0. */
    uint8_t flags;
} markdown_core_concrete_record;

/** A node's record vector, allocated on first append and owned by the
 * node (freed with it). The header and the records share one allocation
 * so an append costs at most one allocator call. */
typedef struct markdown_core_concrete_records {
    size_t count;
    size_t capacity;
    markdown_core_concrete_record records[];
} markdown_core_concrete_records;

/** Appends one record to `*slot`, allocating or growing the vector
 * through `mem` as needed. Returns false — leaving `*slot` valid and
 * releasable — when the allocator does, or when doubling would push the
 * byte request past SIZE_MAX; the caller owns turning either into its
 * failure discipline. The ceiling is unreachable through parsing on a
 * 64-bit target, where memory exhausts long before capacity approaches
 * the wrap point, but a 32-bit size_t meets it at 2^27 records and the
 * refusal must not depend on which address space it runs in. */
bool markdown_core_concrete_records_append(
    markdown_core_mem *mem,
    markdown_core_concrete_records **slot,
    uint8_t kind,
    uint32_t line,
    uint32_t column,
    uint32_t length
);

/** Releases a vector; tolerates NULL so owners free unconditionally. */
void markdown_core_concrete_records_free(markdown_core_mem *mem, markdown_core_concrete_records *records);

/** The records of `node`'s own region, in capture order. Returns NULL
 * and sets `*count` to 0 for a node with none. */
const markdown_core_concrete_record *markdown_core_node_concrete_records(const markdown_core_node *node, size_t *count);

/** Inline concrete token records (incremental-canonical-ast.md 0, 11.1).
 *
 * The inline pass consumes or transforms source bytes the semantic tree
 * does not spell back: emphasis and extension delimiter runs, code-span
 * tick runs, escape backslashes, entity spellings, autolink brackets,
 * the bracket tokens and destination tail of a matched link, and the
 * smart-punctuation replacements. These records keep that material, under
 * the same doctrine as the block-phase marker records above: a byte run
 * whose spelling the projection preserves — plain text, trailing
 * whitespace, line breaks and the spacing that hardens them — is an
 * implicit gap with no record.
 *
 * Records live on the node that owns the inline sequence per 11.1 — the
 * leaf block, TableCell, or block DirectiveLabel whose reparse unit the
 * sequence is — in a vector separate from the block-phase `concrete`
 * vector because the two belong to different ownership domains. The
 * block records describe the node's own marker lines and stay put when
 * only the inline domain is rebuilt; the inline records describe the
 * content buffer and must travel with it: a dependent rebuild swaps
 * {content, children, inline records} as one domain, and the domain swap
 * stays self-inverse.
 *
 * `start` and `length` are byte extents in the owning node's content
 * buffer — the exact substrate the inline pass scans, which the node
 * retains. That coordinate is what the incremental seam relies on: a
 * valid seam prefix admits no byte that any capture site records (every
 * record-producing byte is a seam barrier, which the seam gate pins), so
 * a fast-forwarded reparse's vector is complete without transplanting a
 * single record.
 *
 * Whether delimiter material was consumed as markup is decided at reduce
 * time, not at capture time. `head` and `tail` count the bytes consumed
 * from each end of the token: a fully consumed token has head == length,
 * an untouched candidate 0/0, and a partially consumed emphasis run the
 * split the reduce loop actually took (`*a***b*` consumes its middle run
 * from both ends and leaves a literal byte between). head + tail never
 * exceeds length. `flags` is reserved (always 0) for 14.1.10; during
 * capture its top bit tombstones records the parse later reinterprets as
 * raw source (a formula or cross-reference body, a footnote label), and
 * handoff compacts them away. */

typedef enum markdown_core_inline_concrete_kind {
    /** A delimiter run the engine tracked: `*`/`_` emphasis runs, smart
     * quotes, and every extension delimiter (strikethrough, formula,
     * directive, cross-link/embed) — one record per push, patched at
     * reduce. An unpaired run keeps head == tail == 0 and its marker
     * text stays in the tree. */
    MARKDOWN_CORE_INLINE_CONCRETE_DELIMITER_RUN = 1,
    /** A `'` or `"` under SMART: the source byte is replaced by its curly
     * glyph at scan time, so it is fully consumed whether or not it later
     * pairs. */
    MARKDOWN_CORE_INLINE_CONCRETE_SMART_QUOTE,
    /** A hyphen run of two or more under SMART, replaced by dashes. A
     * lone `-` stays itself and gets no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_SMART_DASH,
    /** A `...` under SMART, replaced by an ellipsis. `.` and `..` stay
     * themselves and get no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_SMART_ELLIPSIS,
    /** One tick run of a matched code span — two records per span. An
     * unmatched run stays literal text and gets none. */
    MARKDOWN_CORE_INLINE_CONCRETE_CODE_TICKS,
    /** One markup backslash: a punctuation escape, one per pair of an
     * all-backslash run, or the backslash of a hard break. A backslash
     * that escapes nothing stays literal and gets no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_ESCAPE,
    /** A full entity spelling, `&` through `;`, decoded into the text.
     * An `&` that opens no entity stays literal and gets no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_ENTITY,
    /** A whole `<...>` URI or email autolink. Raw HTML keeps its exact
     * source bytes as its literal and gets no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_AUTOLINK,
    /** A `[` or `![` bracket opener, captured when pushed; consumed only
     * when its bracket matches. */
    MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_OPEN,
    /** The `]` of a matched link or image. An unmatched `]` stays
     * literal and gets no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_BRACKET_CLOSE,
    /** The consumed tail after a matched close bracket: `(dest "title")`
     * of an inline link, `[label]` of a full reference, `[]` of a
     * collapsed one. A shortcut reference consumes no tail and gets no
     * record. */
    MARKDOWN_CORE_INLINE_CONCRETE_LINK_TAIL,
    /** The `[^` of a defined footnote reference; its `]` is a
     * BRACKET_CLOSE. The label between them keeps its spelling on the
     * node. */
    MARKDOWN_CORE_INLINE_CONCRETE_FOOTNOTE_OPEN,
    /** An inline directive's `:name` spelling, colon included, consumed
     * at scan time outside the delimiter engine (the engine records only
     * the label's bracket delimiters). Fully consumed: the name lives on
     * as the node's scalar, never as text. */
    MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_NAME,
    /** An inline directive's `{...}` attribute block, braces included,
     * when it parses and is consumed in the same scan as the name (the
     * labeled form's attributes ride the closer's DELIMITER_RUN instead).
     * Braces that fail to parse stay literal text and get no record. */
    MARKDOWN_CORE_INLINE_CONCRETE_DIRECTIVE_ATTRIBUTES
} markdown_core_inline_concrete_kind;

typedef struct markdown_core_inline_concrete_record {
    /** Byte offset of the token in the owning node's content buffer. */
    uint32_t start;
    /** Token length in bytes; never 0. */
    uint32_t length;
    /** Bytes consumed as markup from the token's front. */
    uint32_t head;
    /** Bytes consumed as markup from the token's back. */
    uint32_t tail;
    /** A markdown_core_inline_concrete_kind. */
    uint8_t kind;
    /** Reserved for 14.1.10 recovery material; always 0 once handed to a
     * node. */
    uint8_t flags;
} markdown_core_inline_concrete_record;

/** A node's inline record vector; same single-allocation ownership as the
 * block vector above. */
typedef struct markdown_core_inline_concrete_records {
    size_t count;
    size_t capacity;
    markdown_core_inline_concrete_record records[];
} markdown_core_inline_concrete_records;

/** A retraction a reducer deferred: records wholly inside [start, end)
 * are dropped at handoff. Deferred rather than applied, because nested
 * balanced formulas reduce inside out with ever-growing interiors —
 * re-walking each one is the Theta(N^2) the complexity gates measure,
 * while noting the span is O(1) and one handoff sweep retires them all. */
typedef struct markdown_core_concrete_retraction {
    uint32_t start;
    uint32_t end;
} markdown_core_concrete_retraction;

/** The capture a subject builds during one inline parse, then hands to
 * the parsed node. `mem` doubles as the engagement flag: a subject built
 * without a parser (reference parsing) never captures. */
typedef struct markdown_core_concrete_capture {
    markdown_core_mem *mem;
    markdown_core_inline_concrete_records *records;
    markdown_core_concrete_retraction *retractions;
    size_t retraction_count;
    size_t retraction_capacity;
    /* Some record carries the building-phase tombstone bit, so handoff
     * must compact. */
    bool dirty;
} markdown_core_concrete_capture;

/** Engages `capture` for one parse; `mem` may be NULL to leave it inert. */
void markdown_core_concrete_capture_init(markdown_core_concrete_capture *capture, markdown_core_mem *mem);

/** Appends one record, growing under the same ceiling discipline as the
 * block append. Returns false on allocator failure or at the doubling
 * wrap point, leaving the vector valid; the caller owns folding that
 * loss into the parse's failure. Must not be called on an inert
 * capture. */
bool markdown_core_concrete_capture_append(
    markdown_core_concrete_capture *capture,
    uint8_t kind,
    uint32_t start,
    uint32_t length,
    uint32_t head,
    uint32_t tail
);

/** Number of records appended so far, tombstoned or not. Indexes below
 * this stay stable until handoff — the engine and the bracket stack key
 * their patches on them. */
size_t markdown_core_concrete_capture_count(const markdown_core_concrete_capture *capture);

/** Adds reduce-time consumption to record `index`: emphasis openers
 * consume from the tail, closers from the head. */
void markdown_core_concrete_capture_consume(
    markdown_core_concrete_capture *capture,
    size_t index,
    uint32_t head,
    uint32_t tail
);

/** Marks record `index` fully consumed (head = length, tail = 0);
 * idempotent, for tokens consumed whole at scan or reduce time. */
void markdown_core_concrete_capture_consume_all(markdown_core_concrete_capture *capture, size_t index);

/** Rewrites record `index`'s kind: the smart-quote patch after a generic
 * delimiter push. */
void markdown_core_concrete_capture_set_kind(markdown_core_concrete_capture *capture, size_t index, uint8_t kind);

/** Tombstones every record from `index` on: the bracket retraction when
 * `[^...]` reinterprets its interior as one atomic label. The run it
 * walks is the bracket's own records — nothing later exists yet — so the
 * walk is the bracket's span, not the vector. */
void markdown_core_concrete_capture_tombstone_from(markdown_core_concrete_capture *capture, size_t index);

/** Defers the retraction a reducer owes when it discards parsed interior
 * structure and re-borrows the raw bytes (formula, cross-link/embed):
 * every record wholly inside [start, end) is dropped at handoff. Returns
 * false when noting the span loses an allocation; the caller folds that
 * into the parse's failure like any lost record. */
bool markdown_core_concrete_capture_retract_span(markdown_core_concrete_capture *capture, uint32_t start, uint32_t end);

/** Compacts tombstones away and hands the vector over (NULL when nothing
 * survives, the buffer freed). The capture is reset either way. */
markdown_core_inline_concrete_records *markdown_core_concrete_capture_take(markdown_core_concrete_capture *capture);

/** Frees a building vector without handoff — the failed-parse path. */
void markdown_core_concrete_capture_abandon(markdown_core_concrete_capture *capture);

/** Releases a node-owned vector; tolerates NULL. */
void markdown_core_inline_concrete_records_free(markdown_core_mem *mem, markdown_core_inline_concrete_records *records);

/** The inline records of `node`'s own region, ascending by start with no
 * overlap. Returns NULL and sets `*count` to 0 for a node with none. */
const markdown_core_inline_concrete_record *markdown_core_node_inline_concrete_records(
    const markdown_core_node *node,
    size_t *count
);

#ifdef __cplusplus
}
#endif

#endif
