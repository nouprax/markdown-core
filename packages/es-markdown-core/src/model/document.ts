import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Diagnostic } from "./diagnostic.js";
import type { Commit } from "./commit.js";
import type { ParseOptions } from "../parse-options.js";
import type { Scope } from "../values.js";

export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
    /** The options this document and its whole lineage were parsed under. */
    readonly options: Readonly<Required<ParseOptions>>;
    /** Everything an editor should underline, in source order. Empty for
     * almost every document; see `DiagnosticCode`. */
    readonly diagnostics: readonly Diagnostic[];
    /**
     * The absolute source extent of `node` in this document, O(1).
     *
     * Throws on a node from another parse, and on a stale value whose
     * revision this document has superseded — equal nodes may sit at
     * different absolute positions in different revisions, so pairing old
     * fields with new positions is never right.
     */
    readonly scope: (node: Markup) => Scope;
    /**
     * This document's node for `id`, or null when no node has that identity
     * here. An identity from another parse is null, not a throw: a caller
     * holding an id from a superseded revision is asking exactly this.
     */
    readonly node: (id: Markup["id"]) => Markup | null;
    /**
     * Hands this document new text and returns the document that text
     * describes, together with what changed.
     *
     * SUPERSEDES the receiver: the native parse moves to the successor, so
     * this document must not be edited again. Its already-extracted values,
     * scopes, and diagnostics stay valid forever, because they are values.
     */
    readonly edit: (markdown: string) => Commit;
    /**
     * Releases the native parse. Idempotent, and unnecessary after `edit`,
     * which hands the parse to the successor. Every value this document
     * already produced stays usable afterwards, because none of them borrow
     * from the parse.
     */
    readonly close: () => void;
    /** Returns the canonical diagnostic dump for this document. */
    readonly dump: () => string;
}
