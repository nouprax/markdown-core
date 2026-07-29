package com.nouprax.markdown.core

import kotlin.jvm.JvmOverloads
import kotlin.jvm.JvmStatic

public class Document internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val content: kotlin.collections.List<Markup>,
    internal val resolver: ScopeResolver,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)

    /**
     * Resolves the absolute scope of [node] within this snapshot, O(1) after
     * the snapshot's one-time materialization.
     *
     * A one-shot [parse] result always answers. A session snapshot
     * materializes its scopes on first use (of [scope], a [MarkupWalker] walk, or
     * [dump]) while it is the session's current snapshot and is
     * self-contained afterwards — including after the session advances or is
     * closed. Requesting a scope from a snapshot that was superseded before
     * any of those ran is a programmer error, as is passing a node that does
     * not belong to this snapshot: one whose id this snapshot does not
     * contain, or a stale value whose revision this snapshot has superseded.
     * (An unchanged value shared across snapshots resolves against any of
     * them — equal nodes may sit at different absolute positions in
     * different snapshots.)
     */
    public fun scope(node: Markup): Scope {
        require(node.id.lineage == id.lineage) { "node belongs to a different session or parse" }
        val entry =
            requireNotNull(resolver.entry(node.id.rawValue)) { "node does not belong to this snapshot" }
        check(entry.revision == node.revision) {
            "node value is from a different revision of this snapshot's session"
        }
        return entry.scope
    }

    /**
     * Resolves and caches every scope of this snapshot now, making the
     * retained value self-contained regardless of later commits or session
     * close — the explicit form of the materialization that [scope], a
     * walk, or [dump] would perform implicitly on first use. Call while the
     * snapshot is current (before the owning session's next successful
     * commit). Idempotent; a one-shot [parse] result is always materialized.
     */
    public fun materialize() {
        resolver.materialize()
    }

    /** Returns the canonical diagnostic dump for this document. */
    public fun dump(): String = MarkupDumper.dump(this)

    /** Returns the canonical diagnostic dump for the subtree rooted at
     * [node], with the subtree as scope origin. */
    public fun dump(node: Markup): String = MarkupDumper.dump(this, node)

    public companion object {
        /** Parses [source] in one shot into a self-contained snapshot;
         * statically callable from Java as `Document.parse(...)`. */
        @JvmStatic
        @JvmOverloads
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document = WireDecoder.decodeDocument(cParse(source.encodeToByteArray(), options))
    }
}
