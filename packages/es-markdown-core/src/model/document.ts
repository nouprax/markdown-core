import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Diagnostic } from "./diagnostic.js";
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
     * describes — the whole-text mutation.
     *
     * What changed is asked of the new tree itself: a node's id names the
     * same thing across the mutation, and its revision says when its own
     * fields, child list, or any descendant last changed — equal
     * (id, revision) within a series means identical content.
     *
     * A document is the live head of a CHAIN, and a mutation advances the
     * chain, old handles die, decoded values live forever: on success this
     * receiver is SUPERSEDED — everything it already produced stays a plain
     * value and `close` still works, but mutating it again is a
     * deterministic error, so history is linear and there is no forking. A
     * failed edit supersedes nothing: the receiver stays the head.
     */
    readonly edit: (markdown: string) => Document;
    /**
     * Appends `chunk` to the end of this document's text and returns the
     * document all bytes so far describe — the trailing mutation.
     *
     * Same tree, dump, and identity rules as an `edit` of the concatenated
     * text; the difference is cost. Appended bytes never move settled
     * content, so the successor re-decodes only what changed, and a node the
     * append did not reach is the predecessor's very value object — same
     * `id`, same `revision`, same `scope`, `===`. Any byte split is legal:
     * mid-UTF-8, mid-CRLF, mid-line.
     *
     * Supersedes the receiver exactly as `edit` does. A failure past the
     * argument checks poisons the chain — every further mutation fails
     * deterministically, only `close` remains, and recovery is a new
     * document from text the caller still holds.
     */
    readonly append: (chunk: string) => Document;
    /**
     * Releases the native parse.
     *
     * Idempotent, O(1), and needed for a superseded document as much as for
     * a live head: a mutation takes nothing away, so a chain leaves one
     * handle per link for its holder to close. Every value this document
     * already produced stays usable afterwards, because none of them borrow
     * from the parse.
     */
    readonly close: () => void;
    /** Returns the canonical diagnostic dump for this document. */
    readonly dump: () => string;
}
