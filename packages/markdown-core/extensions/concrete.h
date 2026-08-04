#ifndef MARKDOWN_CORE_CONCRETE_H
#define MARKDOWN_CORE_CONCRETE_H

#include <stdbool.h>

#include <markdown-core.h>
#include <node.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ownership-region classification (incremental-canonical-ast.md 11.1, 0).
 *
 * The region is the unit a localized edit reparses, and it is defined
 * rather than left to taste because concrete offsets are region-relative:
 * a token or trivia record never becomes a leaf of a document-wide
 * structure, so an edit elsewhere can never touch one. Every bound in the
 * complexity contract is parameterized on this classification, which is
 * why it lands first and alone — the capture, extent, and canonical-text
 * milestones all consume it, and a misclassification would surface as a
 * broken bound three milestones later where it is expensive to trace back.
 *
 * The classes, verbatim from 11.1:
 *
 * - LEAF: a leaf block — Paragraph, Heading, CodeBlock, HTMLBlock,
 *   FormulaBlock, ThematicBreak, ReferenceDefinition — together with its
 *   complete inline child sequence, because inline syntax is non-local
 *   within a leaf and cannot be reparsed in fragments.
 * - INLINE_SEQUENCE: the inline sequence of a TableCell or a *block*
 *   DirectiveLabel, each of which owns the source backing that sequence.
 *   These are the only two members: FootnoteDefinition's content is block
 *   content, so it is a container, and only these two own an inline
 *   sequence without being leaf blocks.
 * - CONTAINER: the marker material of one BlockQuote, List, ListItem,
 *   Table, TableRow, DirectiveBlock, or FootnoteDefinition — its `>` runs,
 *   bullet or ordinal markers, delimiter row, fence lines, or `[^label]:`
 *   opener — which belongs to that container's own region and not to any
 *   child's. Document is the marker-less root of this class.
 * - NONE: inline containers are never regions. Emphasis, links, references,
 *   and an inline Directive's label are materialized during the
 *   surrounding leaf's parse and stay inside it rather than copying and
 *   reparsing the same bytes.
 *
 * A DirectiveLabel is the one kind whose class is contextual: the label of
 * a block directive owns its inline source (INLINE_SEQUENCE); the label of
 * an inline directive lives inside the enclosing leaf's region (NONE).
 * Classification therefore takes the node, not the type.
 */
typedef enum markdown_core_region_class {
    MARKDOWN_CORE_REGION_NONE,
    MARKDOWN_CORE_REGION_LEAF,
    MARKDOWN_CORE_REGION_INLINE_SEQUENCE,
    MARKDOWN_CORE_REGION_CONTAINER
} markdown_core_region_class;

/** Classifies `node` per 11.1. Pure: reads the node's type and, for a
 * directive label, its parent's type; allocates nothing. Unknown types are
 * a contract violation and classify as NONE, which the partition gate
 * refuses (every node of a committed tree must belong to a region). */
markdown_core_region_class markdown_core_region_classify(const markdown_core_node *node);

/** The region that owns `node`'s concrete material: the node itself when it
 * is a region, otherwise its nearest region ancestor. For every inline node
 * the result is the enclosing LEAF or INLINE_SEQUENCE region — never a
 * CONTAINER, because marker material owns no inline content. Returns NULL
 * only for a detached non-region node, which no committed tree contains. */
const markdown_core_node *markdown_core_region_of(const markdown_core_node *node);

#ifdef __cplusplus
}
#endif

#endif
