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
 * underlines, thematic breaks, `[^label]:` openers — and until now kept
 * only what the canonical AST needs (a level, a fence length clamped to
 * 255, an int the leading zeros of an ordinal collapse into). These
 * records keep the marker material itself: for each marker the grammar
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
 *   3-byte U+FFFD, invalid UTF-8 replaced under
 *   MARKDOWN_CORE_OPT_VALIDATE_UTF8, EOL excluded). A normalized-line
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
    MARKDOWN_CORE_CONCRETE_FOOTNOTE_OPENER
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

#ifdef __cplusplus
}
#endif

#endif
