import MarkdownCoreC

/// The single mutable owner of one Markdown text and its living AST.
///
/// Queue edits (`append` is an edit at end-of-text), then `commit()`: the
/// session reparses incrementally, keeps node identity wherever content is
/// unchanged, and returns the new snapshot with the exact delta. After
/// any sequence of edits and commits the document is semantically identical
/// to a one-shot `Document.parse` of the same final text.
///
/// Calls on one session must be externally synchronized (one writer at a
/// time); snapshots are immutable `Sendable` values that survive the
/// session. Commits are transactional: on failure the session stays valid at
/// its previous revision and the commit may be retried (applied edits are
/// retained — the text advances, the tree does not).
public final class MarkupSession {
    let session: OpaquePointer
    var mirror: [UInt64: any Markup] = [:]
    private var currentResolver: ScopeResolver?

    public let options: ParseOptions

    /// Per-session random salt; nodes from different sessions never compare
    /// equal even when their raw ids collide numerically.
    public let lineage: UInt64

    /// The last committed snapshot; the empty document at revision 0 until
    /// the first commit.
    public private(set) var document: Document

    public init(options: ParseOptions = .init()) throws {
        var nativeOptions = options.nativeValue
        var nativeError: OpaquePointer?
        guard let session = markdown_core_session_open(&nativeOptions, &nativeError) else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
        self.session = session
        self.options = options
        lineage = markdown_core_session_lineage(session)
        guard let view = markdown_core_session_document(session),
            let root = markdown_core_document_root(view)
        else {
            markdown_core_session_free(session)
            throw ParseError(
                code: .internal,
                message: "session opened without a document root",
                scope: nil
            )
        }
        // The revision-0 root is always an empty document.
        var document = Document(
            id: MarkupID(lineage: lineage, rawValue: markdown_core_node_get_id(root)),
            revision: markdown_core_node_get_revision(root),
            children: [],
            resolver: ScopeResolver.unresolvable
        )
        let resolver = ScopeResolver(session: session)
        document.resolver = resolver
        currentResolver = resolver
        mirror[document.id.rawValue] = document
        self.document = document
    }

    deinit {
        currentResolver?.detach()
        markdown_core_session_free(session)
    }

    /// The revision of the last committed snapshot; 0 before the first
    /// commit.
    public var revision: UInt64 { markdown_core_session_revision(session) }

    /// The byte length of the stored text, including uncommitted edits.
    public var length: Int { markdown_core_session_length(session) }

    /// Queues an append of `text`'s UTF-8 bytes at the end of the stored
    /// text. Nothing is parsed until `commit()`.
    public func append(_ text: String) throws {
        try replace(length..<length, with: text)
    }

    /// Queues a replacement of the byte range `range` of the stored text
    /// with `text`'s UTF-8 bytes. An empty range inserts; empty `text`
    /// deletes. Offsets refer to the stored text as previously edited;
    /// nothing is parsed until `commit()`.
    public func replace(_ range: Range<Int>, with text: String) throws {
        var nativeError: OpaquePointer?
        let bytes = Array(text.utf8)
        let applied = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_session_edit(
                session,
                range.lowerBound,
                range.upperBound,
                buffer.baseAddress,
                buffer.count,
                &nativeError
            )
        }
        guard applied else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
    }

    /// Reparses the pending text incrementally and returns the new snapshot
    /// with its delta. The snapshot shares every unchanged node value
    /// with the previous snapshot; the work is proportional to the delta,
    /// not the document.
    public func commit() throws -> Commit {
        var nativeChanges: OpaquePointer?
        var nativeError: OpaquePointer?
        // The previous snapshot's currency ends when the commit starts:
        // detach its resolver before the native tree is replaced, so a
        // not-yet-materialized snapshot can never cache the new revision's
        // positions as its own — a racing reader either materialized from
        // the still-unchanged tree or takes the documented
        // superseded-snapshot trap.
        let previousResolver = currentResolver
        previousResolver?.detach()
        guard markdown_core_session_commit(session, &nativeChanges, &nativeError) else {
            defer { markdown_core_error_free(nativeError) }
            // The native commit failed transactionally: the tree is
            // unchanged at the previous revision, the previous snapshot
            // becomes current again, and the commit may be retried.
            previousResolver?.reattach(session: session)
            throw ParseError(from: nativeError)
        }
        guard let nativeChanges else {
            preconditionFailure("session commit succeeded without a delta")
        }
        defer { markdown_core_delta_free(nativeChanges) }
        let changes = Delta(from: nativeChanges, lineage: lineage)

        guard let view = markdown_core_session_document(session),
            let root = markdown_core_document_root(view)
        else {
            preconditionFailure("session committed without a document root")
        }
        if changes.beforeRevision == 0 {
            // First commit: every node is fresh, so a direct recursive build
            // skips the by-id lookups and depth sort of the delta path. This
            // is also what keeps the one-shot `Document.parse` sugar on the
            // v1 performance budget.
            mirror[markdown_core_node_get_id(root)] = bulkBuild(root)
        } else {
            for id in changes.removed {
                mirror.removeValue(forKey: id.rawValue)
            }
            let builder = MarkupBuilder(lineage: lineage) { [self] node in childValues(of: node) }
            for rebuild in rebuildsByDepth(changes) {
                mirror[rebuild.rawID] = builder.markup(from: rebuild.node)
            }
        }

        return Commit(document: adoptSnapshot(root: root), changes: changes)
    }

    private func adoptSnapshot(root: OpaquePointer) -> Document {
        guard var document = mirror[markdown_core_node_get_id(root)] as? Document else {
            preconditionFailure("session committed without a document root")
        }
        let resolver = ScopeResolver(session: session)
        document.resolver = resolver
        currentResolver = resolver
        mirror[document.id.rawValue] = document
        self.document = document
        return document
    }

    /// One-shot support: the first commit without materializing a delta (the
    /// C out-parameter stays NULL, exactly the C-consumer knob). The bulk
    /// rebuild needs no delta and `Document.parse` discards it.
    func commitBulk() throws -> Document {
        precondition(revision == 0, "commitBulk is only the first commit")
        var nativeError: OpaquePointer?
        // Same detach-before-commit ordering as `commit()`; the intermediate
        // snapshot is never exposed by `Document.parse`, so this only keeps
        // the two commit paths on one contract.
        let previousResolver = currentResolver
        previousResolver?.detach()
        guard markdown_core_session_commit(session, nil, &nativeError) else {
            defer { markdown_core_error_free(nativeError) }
            previousResolver?.reattach(session: session)
            throw ParseError(from: nativeError)
        }
        guard let view = markdown_core_session_document(session),
            let root = markdown_core_document_root(view)
        else {
            preconditionFailure("session committed without a document root")
        }
        mirror[markdown_core_node_get_id(root)] = bulkBuild(root)
        return adoptSnapshot(root: root)
    }

    /// The committed snapshot's current value for `id`; nil when no node
    /// with that identity exists at the committed revision.
    public func node(for id: MarkupID) -> (any Markup)? {
        id.lineage == lineage ? mirror[id.rawValue] : nil
    }

    private func bulkBuild(_ root: OpaquePointer) -> any Markup {
        // Post-order over an explicit stack (adversarial nesting must not
        // exhaust native recursion). Child arrays assemble in sibling frames
        // so the bulk build costs one mirror write per node and no reads.
        var frames: [[any Markup]] = [[]]
        var completed: [any Markup] = []
        let builder = MarkupBuilder(lineage: lineage) { _ in completed }
        var stack: [(node: OpaquePointer, ready: Bool)] = [(root, false)]
        while let (node, ready) = stack.popLast() {
            if ready {
                completed = frames.removeLast()
                let value = builder.markup(from: node)
                frames[frames.count - 1].append(value)
                mirror[markdown_core_node_get_id(node)] = value
                continue
            }
            stack.append((node, true))
            frames.append([])
            // Reversed so pops build children in source order.
            var children: [OpaquePointer] = []
            var child = markdown_core_node_get_first_child(node)
            while let current = child {
                children.append(current)
                child = markdown_core_node_get_next_sibling(current)
            }
            for current in children.reversed() {
                stack.append((current, false))
            }
        }
        guard let value = frames.first?.first else {
            preconditionFailure("bulk build did not reach the root")
        }
        return value
    }

    private struct Rebuild {
        let rawID: UInt64
        let node: OpaquePointer
        let depth: Int
    }

    private func rebuildsByDepth(_ changes: Delta) -> [Rebuild] {
        var rebuilds: [Rebuild] = []
        rebuilds.reserveCapacity(
            changes.added.count + changes.changed.count + changes.bubbled.count
        )
        for id in [changes.added, changes.changed, changes.bubbled].joined() {
            guard let node = markdown_core_session_node_by_id(session, id.rawValue) else {
                preconditionFailure("delta names a node the session cannot resolve")
            }
            var depth = 0
            var parent = markdown_core_node_get_parent(node)
            while let current = parent {
                depth += 1
                parent = markdown_core_node_get_parent(current)
            }
            rebuilds.append(Rebuild(rawID: id.rawValue, node: node, depth: depth))
        }
        // Children before parents: a rebuilt parent assembles its child
        // values from the mirror.
        return rebuilds.sorted { $0.depth > $1.depth }
    }

    private func childValues(of node: OpaquePointer) -> [any Markup] {
        var result: [any Markup] = []
        result.reserveCapacity(markdown_core_node_child_count(node))
        var child = markdown_core_node_get_first_child(node)
        while let current = child {
            guard let value = mirror[markdown_core_node_get_id(current)] else {
                preconditionFailure("delta did not cover a rebuilt parent's child")
            }
            result.append(value)
            child = markdown_core_node_get_next_sibling(current)
        }
        return result
    }
}

extension MarkupSession {
    /// Async sugar over the streaming hot path: appends each token from
    /// `tokens` and commits, yielding one `Commit` per token. Iterate on the
    /// isolation that owns the session; coalescing tokens before feeding
    /// them trades latency for throughput exactly as manual `append` +
    /// `commit` does.
    public func updates<Tokens: AsyncSequence>(
        feeding tokens: Tokens
    ) -> Updates<Tokens> where Tokens.Element == String {
        Updates(session: self, iterator: tokens.makeAsyncIterator())
    }

    public struct Updates<Tokens: AsyncSequence>: AsyncSequence, AsyncIteratorProtocol
    where Tokens.Element == String {
        let session: MarkupSession
        var iterator: Tokens.AsyncIterator

        public func makeAsyncIterator() -> Updates<Tokens> { self }

        public mutating func next() async throws -> Commit? {
            guard let token = try await iterator.next() else { return nil }
            try session.append(token)
            return try session.commit()
        }
    }
}
