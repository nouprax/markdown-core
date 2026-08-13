import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Diagnostic } from "./diagnostic.js";
import type { Commit } from "./commit.js";
import type { ParseOptions } from "../parse-options.js";

/**
 * One parsed Markdown document: the root of the canonical value tree and the
 * owner of the native parse it came from.
 *
 * The root is a plain value like every node under it. `kind`, `id`,
 * `revision`, `scope`, and `content` are its only ENUMERABLE properties, so
 * spreading or structured-cloning a document yields the tree alone; the
 * options, the diagnostics, and the four mediators below do not come along.
 */
export interface Document extends MarkupBase<"document"> {
    /** The document's top-level blocks in source order. */
    readonly content: readonly Markup[];
    /** The options this document and its whole series were parsed under. */
    readonly options: Readonly<Required<ParseOptions>>;
    /** Everything an editor should underline, in source order.
     *
     * Empty for almost every document; see {@link DiagnosticCode}. */
    readonly diagnostics: readonly Diagnostic[];
    /**
     * This document's node for `id`, or null when no node has that identity
     * here.
     *
     * An identity from another parse is null, not a throw: a caller holding
     * an id from a superseded revision is asking exactly this.
     */
    readonly node: (id: Markup["id"]) => Markup | null;
    /**
     * Hands this document new text and returns the document that text
     * describes, together with what changed.
     *
     * Reads the receiver and takes nothing from it: this document stays
     * usable and may be edited again. Editing it twice gives two lines of
     * descent, told apart by their revisions — and, like nodes from two
     * separate parses, nodes from two lines are not comparable.
     */
    readonly edit: (markdown: string) => Commit;
    /**
     * Releases the native parse.
     *
     * Idempotent, and needed after `edit` as much as before it: an edit takes
     * nothing away, so a chain of edits leaves one parse per link for its
     * holder to close. Every value this document already produced stays usable
     * afterwards, because none of them borrow from the parse.
     */
    readonly close: () => void;
    /** Returns the canonical diagnostic dump for this document. */
    readonly dump: () => string;
}
