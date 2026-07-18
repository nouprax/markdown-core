import type { CSession } from "../runtime/c-session.js";
import type { Scope } from "../values.js";

export interface ScopeEntry {
    readonly revision: number;
    readonly scope: Scope;
}

type ResolverState =
    | { readonly phase: "pending"; readonly session: CSession }
    | { readonly phase: "materialized"; readonly table: ReadonlyMap<number, ScopeEntry> }
    | { readonly phase: "detached" };

/**
 * Resolves absolute scopes for one snapshot.
 *
 * Session snapshots do not store positions on node values: deltas
 * deliberately omit pure positional shifts, so a snapshot resolves every
 * scope against the session's native tree the first time one is requested
 * (one walk, cached) and is self-contained from then on. The owning session
 * detaches the resolver before the tree changes; a resolver that was
 * detached before it ever materialized can no longer answer.
 *
 * Each cached entry keeps the node's revision at this snapshot, so a stale
 * value — same id, superseded revision — is rejected instead of silently
 * pairing old fields with this snapshot's position.
 */
export class ScopeResolver {
    private state: ResolverState;

    private constructor(state: ResolverState) {
        this.state = state;
    }

    static live(session: CSession): ScopeResolver {
        return new ScopeResolver({ phase: "pending", session });
    }

    static materialized(table: ReadonlyMap<number, ScopeEntry>): ScopeResolver {
        return new ScopeResolver({ phase: "materialized", table });
    }

    /** Called by the owning session before the native tree is replaced or
     * freed. A materialized resolver keeps answering from its cache. */
    detach(): void {
        if (this.state.phase === "pending") this.state = { phase: "detached" };
    }

    /** Undoes `detach` after a transactionally failed commit: the native
     * tree is unchanged, so the snapshot is current again. A resolver that
     * materialized before the detach keeps its (still valid) cache. */
    reattach(session: CSession): void {
        if (this.state.phase === "detached") this.state = { phase: "pending", session };
    }

    /** Forces the one-time materialization now; the caller guarantees the
     * snapshot is current. (0 is never a valid id, so this only builds the
     * table.) */
    materialize(): void {
        this.entry(0);
    }

    /** Returns undefined when the id has no node in this snapshot. Throws
     * when the snapshot was superseded before any scope was resolved. */
    entry(rawValue: number): ScopeEntry | undefined {
        if (this.state.phase === "pending") {
            this.state = { phase: "materialized", table: this.state.session.scopeTable() };
        }
        if (this.state.phase === "detached") {
            throw new Error(
                "scope requested from a superseded snapshot that never resolved scopes while it was current"
            );
        }
        return this.state.table.get(rawValue);
    }
}
