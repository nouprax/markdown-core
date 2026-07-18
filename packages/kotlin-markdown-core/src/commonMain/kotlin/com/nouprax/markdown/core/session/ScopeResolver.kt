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

    // NativeSession (pending) | Map<ULong, ScopeEntry> (materialized) |
    // Detached. Reads between the owning session's mutating calls are safe
    // by the native contract; the atomic keeps concurrent readers and the
    // session's detach consistent with each other.
    private val state = AtomicReference(initial)

    /**
     * Called by the owning session before the native tree is replaced or
     * freed. A materialized resolver keeps answering from its cache.
     */
    fun detach() {
        while (true) {
            val current = state.load()
            if (current !is NativeSession) return
            if (state.compareAndSet(current, Detached)) return
        }
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
                is NativeSession -> {
                    state.compareAndSet(current, WireDecoder.decodeScopes(current.scopes()))
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

        fun live(session: NativeSession): ScopeResolver = ScopeResolver(session)

        fun materialized(table: Map<ULong, ScopeEntry>): ScopeResolver = ScopeResolver(table)
    }
}
