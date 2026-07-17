import MarkdownCoreC
import Synchronization

/// Resolves absolute scopes for one snapshot.
///
/// Session snapshots do not store positions on node values: deltas
/// deliberately omit pure positional shifts, so a snapshot resolves every
/// scope against the session's native tree the first time one is requested
/// (one walk, cached) and is self-contained from then on. The owning session
/// detaches the resolver before the tree changes; a resolver that was
/// detached before it ever materialized can no longer answer.
///
/// Each cached entry keeps the node's revision at this snapshot, so a stale
/// value — same id, superseded revision — is rejected instead of silently
/// pairing old fields with this snapshot's position.
final class ScopeResolver: Sendable {
    struct Entry {
        let revision: UInt64
        let scope: Scope
    }

    // The native session pointer is only touched under the mutex, and reads
    // between the owning session's mutating calls are safe by the C
    // contract; the pointer is what keeps the struct from being Sendable by
    // synthesis.
    private struct State: @unchecked Sendable {
        var table: [UInt64: Entry]?
        var session: OpaquePointer?
    }

    private let state: Mutex<State>

    /// Placeholder carried by mirror-internal `Document` values; every
    /// exposed snapshot swaps in a live or materialized resolver.
    static let unresolvable = ScopeResolver()

    private init() {
        state = Mutex(State(table: nil, session: nil))
    }

    init(session: OpaquePointer) {
        state = Mutex(State(table: nil, session: session))
    }

    /// Called by the owning session before the native tree is replaced or
    /// freed. A materialized resolver keeps answering from its cache.
    func detach() {
        state.withLock { $0.session = nil }
    }

    /// Forces the one-time materialization now; the caller guarantees the
    /// snapshot is current. (0 is never a valid id, so this only builds the
    /// table.)
    func materialize() {
        _ = entry(of: 0)
    }

    /// Returns nil when the id has no node in this snapshot. Traps when the
    /// snapshot was superseded before any scope was resolved.
    func entry(of rawID: UInt64) -> Entry? {
        state.withLock { state in
            if state.table == nil {
                guard let session = state.session else {
                    preconditionFailure(
                        "scope requested from a superseded snapshot that never resolved scopes while it was current"
                    )
                }
                guard let view = markdown_core_session_document(session),
                    let root = markdown_core_document_root(view)
                else {
                    state.table = [:]
                    state.session = nil
                    return nil
                }
                state.table = Self.materialize(root: root)
                state.session = nil
            }
            return state.table?[rawID]
        }
    }

    static func materialize(root: OpaquePointer) -> [UInt64: Entry] {
        var result: [UInt64: Entry] = [:]
        var stack: [OpaquePointer] = [root]
        while let node = stack.popLast() {
            result[markdown_core_node_get_id(node)] = Entry(
                revision: markdown_core_node_get_revision(node),
                scope: Scope(from: markdown_core_node_scope(node))
            )
            var child = markdown_core_node_get_first_child(node)
            while let current = child {
                stack.append(current)
                child = markdown_core_node_get_next_sibling(current)
            }
        }
        return result
    }
}

extension Document {
    /// Resolves the absolute scope of `node` within this snapshot, O(1)
    /// after the snapshot's one-time materialization.
    ///
    /// A one-shot `Document.parse` result always answers. A session snapshot
    /// materializes its scopes on first use (of `scope(of:)`, a `Walker`
    /// walk, or `dump()`) while it is the session's current snapshot and is
    /// self-contained afterwards — including after the session advances or
    /// is deinitialized. Requesting a scope from a snapshot that was
    /// superseded before any of those ran is a programmer error and traps,
    /// as is passing a node that does not belong to this snapshot: one whose
    /// id this snapshot does not contain, or a stale value whose revision
    /// this snapshot has superseded. (An unchanged value shared across
    /// snapshots resolves against any of them — equal nodes may sit at
    /// different absolute positions in different snapshots.)
    public func scope(of node: some Markup) -> Scope {
        precondition(
            node.id.lineage == id.lineage,
            "node belongs to a different session or parse"
        )
        guard let entry = resolver.entry(of: node.id.rawValue) else {
            preconditionFailure("node does not belong to this snapshot")
        }
        precondition(
            entry.revision == node.revision,
            "node value is from a different revision of this snapshot's session"
        )
        return entry.scope
    }
}
