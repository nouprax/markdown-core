@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.concurrent.atomics.AtomicReference
import kotlin.concurrent.atomics.ExperimentalAtomicApi
import kotlin.jvm.JvmOverloads
import kotlin.jvm.JvmStatic
import kotlin.jvm.JvmSynthetic

private class ScopeEntry(
    val revision: ULong,
    val scope: Scope,
)

private sealed interface ResolverState

private class LiveResolver(
    val session: CSessionHandle,
) : ResolverState

private class MaterializedResolver(
    val entries: Map<ULong, ScopeEntry>,
) : ResolverState

private object DetachedResolver : ResolverState

private object MaterializingResolver : ResolverState

@get:JvmSynthetic
@set:JvmSynthetic
internal var scopeMaterializeProbe: (() -> Unit)? = null

/**
 * Resolves absolute scopes for one snapshot.
 *
 * Session snapshots do not store positions on node values: deltas
 * deliberately omit pure positional shifts, so a snapshot resolves every
 * scope against the session's native tree the first time one is requested
 * (one payload, cached) and is self-contained from then on. The owning
 * session detaches the resolver before the tree changes; a resolver that was
 * detached before it ever materialized can no longer answer.
 *
 * Each cached entry keeps the node's revision at this snapshot, so a stale
 * value — same id, superseded revision — is rejected instead of silently
 * pairing old fields with this snapshot's position.
 */
@OptIn(ExperimentalAtomicApi::class)
private class ScopeResolver(
    initial: ResolverState,
) {
    // LiveResolver (pending) | MaterializingResolver (one reader owns the
    // native scopes() call) | MaterializedResolver | DetachedResolver.
    // The materializing state makes the native call atomic with the owning
    // document's detach: a writer that wants to commit or close spins until
    // the reader publishes its table, so the native session can never be
    // mutated or freed under a reader.
    private val state = AtomicReference(initial)

    fun detach() {
        while (true) {
            when (val current = state.load()) {
                is LiveResolver -> {
                    if (state.compareAndSet(current, DetachedResolver)) {
                        return
                    }
                }

                MaterializingResolver -> {
                    materializeWaitHint()
                }

                DetachedResolver, is MaterializedResolver -> {
                    return
                }
            }
        }
    }

    fun reattach(session: CSessionHandle) {
        state.compareAndSet(DetachedResolver, LiveResolver(session))
    }

    /** Forces the one-time materialization now. Zero is never a valid id, so
     * this only builds the table. */
    fun materialize() {
        entry(0UL)
    }

    fun entry(rawValue: ULong): ScopeEntry? {
        while (true) {
            when (val current = state.load()) {
                is LiveResolver -> {
                    if (state.compareAndSet(current, MaterializingResolver)) {
                        val entries =
                            try {
                                scopeMaterializeProbe?.invoke()
                                decodeWireScopeMap(current.session.scopes()) { revision, scope ->
                                    ScopeEntry(revision, scope)
                                }
                            } catch (failure: Throwable) {
                                // The snapshot is still current: hand the
                                // session back so another reader may retry,
                                // and let a spinning detach reclaim it.
                                state.store(current)
                                throw failure
                            }
                        state.store(MaterializedResolver(entries))
                        return entries[rawValue]
                    }
                }

                MaterializingResolver -> {
                    materializeWaitHint()
                }

                DetachedResolver -> {
                    error(
                        "scope requested from a superseded snapshot that never resolved scopes while it was current",
                    )
                }

                is MaterializedResolver -> {
                    return current.entries[rawValue]
                }
            }
        }
    }
}

public class Document private constructor(
    override val id: MarkupID,
    override val revision: ULong,
    public val content: kotlin.collections.List<Markup>,
    private val resolver: ScopeResolver,
) : Markup {
    override fun <Result> accept(visitor: MarkupVisitor<Result>): Result = visitor.visit(this)

    override fun equals(other: Any?): Boolean = markupEquals(this, other)

    override fun hashCode(): Int = markupHashCode(this)

    /**
     * Resolves the absolute scope of [node] within this snapshot, O(1) after
     * the snapshot's one-time materialization.
     *
     * A one-shot [parse] result always answers. A session snapshot
     * materializes its scopes on first use (of [scope], a scopeful
     * [MarkupWalker] event walk, or [dump]) while it is the session's current
     * snapshot and is self-contained afterwards — including after the
     * session advances or is closed. Requesting a scope from a snapshot that
     * was superseded before any of those ran is a programmer error, as is
     * passing a node that does not belong to this snapshot: one whose id this
     * snapshot does not contain, or a stale value whose revision this
     * snapshot has superseded.
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
     * scopeful event walk, or [dump] would perform implicitly on first use.
     * Call while the snapshot is current (before the owning session's next
     * successful commit). Idempotent; a one-shot [parse] result is always
     * materialized.
     */
    public fun materialize() {
        resolver.materialize()
    }

    /** Returns the canonical diagnostic dump for this document. */
    public fun dump(): String = MarkupDumper.dump(this)

    /** Returns the canonical diagnostic dump for the subtree rooted at
     * [node], with the subtree as scope origin. */
    public fun dump(node: Markup): String = MarkupDumper.dump(this, node)

    @JvmSynthetic
    internal fun detachResolver() {
        resolver.detach()
    }

    @JvmSynthetic
    internal fun reattachResolver(session: CSessionHandle) {
        resolver.reattach(session)
    }

    public companion object {
        @JvmSynthetic
        internal fun unresolved(
            id: MarkupID,
            revision: ULong,
            content: kotlin.collections.List<Markup>,
        ): Document = Document(id, revision, content, ScopeResolver(DetachedResolver))

        @JvmSynthetic
        internal fun live(
            id: MarkupID,
            revision: ULong,
            content: kotlin.collections.List<Markup>,
            session: CSessionHandle,
        ): Document = Document(id, revision, content, ScopeResolver(LiveResolver(session)))

        /** Parses [source] in one shot into a self-contained snapshot;
         * statically callable from Java as `Document.parse(...)`. */
        @JvmStatic
        @JvmOverloads
        public fun parse(
            source: String,
            options: ParseOptions = ParseOptions(),
        ): Document =
            decodeWireDocument(
                cParse(source.encodeToByteArray(), options),
                ::ScopeEntry,
            ) { id, revision, content, entries ->
                Document(id, revision, content, ScopeResolver(MaterializedResolver(entries)))
            }
    }
}
