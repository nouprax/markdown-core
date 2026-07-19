package com.nouprax.markdown.core

public class Document internal constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val content: kotlin.collections.List<Markup>,
    internal val resolver: ScopeResolver,
) : Markup {
    override fun <Result> accept(visitor: Visitor<Result>): Result = visitor.visit(this)

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

    /** Returns the canonical diagnostic dump for this document. */
    public fun dump(): String = MarkupDumper.dump(this)

    public companion object {
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document = WireDecoder.decodeDocument(cParse(source.encodeToByteArray(), options))
    }
}
