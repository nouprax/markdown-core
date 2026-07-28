package com.nouprax.markdown.core

import kotlin.concurrent.atomics.AtomicReference
import kotlin.concurrent.atomics.ExperimentalAtomicApi

internal class ScopeEntry(
    val revision: ULong,
    val scope: Scope,
)

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
internal class ScopeResolver private constructor(
    initial: Any,
) {
    private object Detached

    private object Materializing

    // CSession (pending) | Materializing (one reader owns the native
    // scopes() call) | Map<ULong, ScopeEntry> (materialized) | Detached.
    // The Materializing state is what makes the native call atomic with the
    // owning session's detach: a writer that wants to commit or close spins
    // in detach() until the in-flight reader publishes its table, so the
    // native session can never be mutated or freed under a reader.
    private val state = AtomicReference(initial)

    /**
     * Called by the owning session before the native tree is replaced or
     * freed. Waits for an in-flight materialization to publish, then leaves
     * a materialized resolver answering from its cache.
     */
    fun detach() {
        while (true) {
            when (val current = state.load()) {
                is CSession -> {
                    if (state.compareAndSet(current, Detached)) {
                        return
                    }
                }

                Materializing -> {
                    materializeWaitHint()
                }

                else -> {
                    return
                }
            }
        }
    }

    /**
     * Undoes [detach] after a transactionally failed commit: the native tree
     * is unchanged, so the snapshot is current again. A resolver that
     * materialized before the detach keeps its (still valid) cache.
     */
    fun reattach(session: CSession) {
        state.compareAndSet(Detached, session)
    }

    /** Forces the one-time materialization now; the caller guarantees the
     * snapshot is current. (0 is never a valid id, so this only builds the
     * table.) */
    fun materialize() {
        entry(0UL)
    }

    /** Returns null when the id has no node in this snapshot. Fails when the
     * snapshot was superseded before any scope was resolved. */
    fun entry(rawValue: ULong): ScopeEntry? {
        while (true) {
            when (val current = state.load()) {
                is CSession -> {
                    if (state.compareAndSet(current, Materializing)) {
                        materializeProbe?.invoke()
                        val table =
                            try {
                                WireDecoder.decodeScopes(current.scopes())
                            } catch (failure: Throwable) {
                                // The snapshot is still current: hand the
                                // session back so another reader may retry,
                                // and let a spinning detach reclaim it.
                                state.store(current)
                                throw failure
                            }
                        state.store(table)
                        return table[rawValue]
                    }
                }

                Materializing -> {
                    materializeWaitHint()
                }

                Detached -> {
                    error(
                        "scope requested from a superseded snapshot that never resolved scopes while it was current",
                    )
                }

                else -> {
                    @Suppress("UNCHECKED_CAST")
                    return (current as Map<ULong, ScopeEntry>)[rawValue]
                }
            }
        }
    }

    companion object {
        /** Placeholder carried by mirror-internal [Document] values; every
         * exposed snapshot swaps in a live or materialized resolver. */
        val unresolvable = ScopeResolver(Detached)

        /** Test seam: runs on the materializing reader between winning the
         * state and issuing the native call, so interleaving tests can hold
         * the reader exactly inside the window detach must respect. */
        var materializeProbe: (() -> Unit)? = null

        fun live(session: CSession): ScopeResolver = ScopeResolver(session)

        fun materialized(table: Map<ULong, ScopeEntry>): ScopeResolver = ScopeResolver(table)
    }
}
