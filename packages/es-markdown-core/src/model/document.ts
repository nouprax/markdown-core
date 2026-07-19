import type { MarkupBase } from "./base.js";
import type { Markup } from "./markup.js";
import type { Scope } from "../values.js";

export interface Document extends MarkupBase<"document"> {
    readonly content: readonly Markup[];
    /**
     * Resolves the absolute scope of `node` within this snapshot, O(1) after
     * the snapshot's one-time materialization.
     *
     * A one-shot `Document.parse` result always answers. A session snapshot
     * materializes its scopes on first use (of `scope`, a `MarkupWalker` walk, or
     * `dump`) while it is the session's current snapshot and is
     * self-contained afterwards — including after the session advances or is
     * closed. Requesting a scope from a snapshot that was superseded before
     * any of those ran is a programmer error, as is passing a node that does
     * not belong to this snapshot: one whose id this snapshot does not
     * contain, or a stale value whose revision this snapshot has superseded.
     * (An unchanged value shared across snapshots resolves against any of
     * them — equal nodes may sit at different absolute positions in
     * different snapshots.)
     */
    readonly scope: (node: Markup) => Scope;
    /** Returns the canonical diagnostic dump for this document. */
    readonly dump: () => string;
}
