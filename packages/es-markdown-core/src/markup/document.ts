import type { Metadata } from "./metadata.js";
import type { MarkupBase } from "./base.js";
import type { Specimen } from "./specimen.js";
import type { Footnote } from "./footnote.js";
import type { Heading } from "./heading.js";
import type { Reference } from "./reference.js";
import type { Markup } from "./markup.js";
import type { Position, Scope, TextUnit } from "./values.js";

/**
 * The immutable semantic root returned by a parse.
 *
 * Its contract fields are enumerable like every node's. The text unit, the
 * definition tables and the queries are not: they are the document's, not
 * the tree's, and `markupEquals` does not compare them.
 */
export interface Document extends MarkupBase<"document"> {
    readonly metadata: Metadata | null;
    readonly content: readonly Markup[];
    /** The unit every column a scope query takes or returns counts in. */
    readonly unit: TextUnit;
    /** Every `Footnote` of the document, inline notes included, in source order. */
    readonly footnotes: readonly Footnote[];
    /** Every `Specimen` of the document, in source order. */
    readonly specimens: readonly Specimen[];
    /** Every `Reference` of the document, in source order. */
    readonly references: readonly Reference[];
    /** The first footnote whose label equals `label`, or null. An inline note
     * has no label and never matches. */
    readonly footnote: (label: string) => Footnote | null;
    /** The first specimen whose label equals `label`, or null. */
    readonly specimen: (label: string) => Specimen | null;
    /**
     * The node the normalized `label` of a `reference` destination names: the
     * first `Reference` whose label equals it, else the first `Heading` whose
     * text declares it, else null.
     */
    readonly reference: (label: string) => Reference | Heading | null;
    /**
     * The scopes of `node`, one per source range in source order, computed
     * from the runs and `source`, the text the document was parsed from, with
     * columns in the document's unit. A node's source ranges are its runs.
     * `node` is a node of this document. Throws `MarkdownCoreError` `outOfBounds` when
     * `source` ends before the node does.
     */
    readonly scope: (node: Markup, source: string) => readonly Scope[];
    /**
     * The last node in canonical walk order one of whose source ranges holds
     * the byte at `position` of `source`, the text the document was parsed
     * from, or null when no node holds it or the position names no byte of
     * `source` or does not fall on a scalar boundary. Throws
     * `MarkdownCoreError` `outOfBounds` unless the line and column are
     * integers of at least 1.
     */
    readonly nodeAt: (position: Position, source: string) => Markup | null;
    /**
     * The canonical debug dump of the document, or of `node`, a node of this
     * document, with scopes computed from `source` in UTF-8 columns whatever
     * the document's unit. Throws `MarkdownCoreError` `outOfBounds` when
     * `source` ends before a node of the dumped tree does.
     */
    readonly dump: {
        (source: string): string;
        (node: Markup, source: string): string;
    };
}
