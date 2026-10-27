import MarkdownCoreC

extension MarkdownSession {
    /// Every node of ``document`` one of whose source ranges holds the byte at
    /// `position` of ``text``, in canonical walk order, so the root first and
    /// the node ``Document/node(at:in:)`` finds last, with the column counted
    /// in the session's ``unit``.
    ///
    /// - Returns: the nodes, none when no node holds the byte or the position
    ///   names no byte of the text at a scalar boundary.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when the
    ///   position's line or column is below 1.
    public func path(at position: Position) throws -> [any Markup] {
        guard position.line >= 1, position.column >= 1 else { throw MarkdownCoreError(code: .outOfBounds) }
        return withBytes { bytes in
            guard let offset = SourceLines(bytes).offset(of: position, in: bytes, unit: unit) else { return [] }
            return document.holders(of: offset).map(\.markup)
        }
    }

    /// The source ranges of `node`'s fields of slot kind `field`, one per run
    /// of each, in source order: the bytes of each value as written, escapes
    /// included, without the quotes, the angle brackets or the sigil around
    /// it. `nil` asks for the fields no slot kind names, such as a title or a
    /// literal. A field the parser makes rather than reads, such as a
    /// heading's computed anchor, has none.
    ///
    /// A node's fields and its ``markers(of:)`` divide its own source: its
    /// runs less the runs of the nodes it holds. Each query reads the node's
    /// own source with the scanners its element read it with, from the native
    /// parse the session holds.
    ///
    /// `node` is a node of the current ``document``, or of an earlier one when
    /// the current document holds it unchanged: a node an append left alone
    /// keeps its id and equals its predecessor.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   the current document does not hold `node`, and
    ///   ``ErrorCode/allocationFailed`` when an allocation fails.
    public func scope(of node: some Markup, field: Slot.Kind?) throws -> [Scope] {
        try parts(of: node) { document, native, bytes, scopes, count in
            markdown_core_document_field(
                document,
                native,
                field?.native ?? MARKDOWN_CORE_SLOT_NONE,
                bytes.baseAddress,
                bytes.count,
                &scopes,
                &count
            )
        }
    }

    /// The source ranges of `node`'s markers: the rest of its own source, the
    /// syntax no field holds, such as the `**` of a strong emphasis, a
    /// heading's `#` run, a list item's marker or a fence, and the spaces and
    /// line endings between them; see ``scope(of:field:)``.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   the current document does not hold `node`, and
    ///   ``ErrorCode/allocationFailed`` when an allocation fails.
    public func markers(of node: some Markup) throws -> [Scope] {
        try parts(of: node) { document, native, bytes, scopes, count in
            markdown_core_document_markers(document, native, bytes.baseAddress, bytes.count, &scopes, &count)
        }
    }

    /// The session's text as UTF-8, handed to `body`.
    func withBytes<Result>(_ body: (UnsafeBufferPointer<UInt8>) throws -> Result) rethrows -> Result {
        var bytes = [UInt8](repeating: 0, count: markdown_core_session_text_size(session))
        if !bytes.isEmpty {
            bytes.withUnsafeMutableBufferPointer { markdown_core_session_text(session, $0.baseAddress) }
        }
        return try bytes.withUnsafeBufferPointer(body)
    }

    /// A field or marker query of `node` against the session's native
    /// document, which the current ``document`` copies: the native node is the
    /// one with `node`'s id on the native path at the first byte `node` holds.
    /// A node that holds no byte owns no source.
    private func parts(
        of node: some Markup,
        _ query: (
            OpaquePointer,
            OpaquePointer,
            UnsafeBufferPointer<UInt8>,
            inout UnsafeMutablePointer<markdown_core_scope>?,
            inout Int
        ) -> markdown_core_status
    ) throws -> [Scope] {
        guard let places = document.places(holding: MarkupRecord.of(node)) else {
            throw MarkdownCoreError(code: .outOfBounds)
        }
        guard let first = places.first(where: { $0.start < $0.end })?.start else { return [] }
        let id = node.id
        let native: OpaquePointer = markdown_core_session_document(session)
        return try withBytes { bytes in
            let position = SourceLines(bytes).scope(from: first, to: first + 1, in: bytes, unit: unit).start
            var nodes: UnsafeMutablePointer<OpaquePointer?>?
            var count = 0
            var status = markdown_core_document_path(
                native,
                position.native,
                bytes.baseAddress,
                bytes.count,
                &nodes,
                &count
            )
            guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
            defer { markdown_core_path_free(nodes) }
            // The native document is the one the current document copies, so
            // the node is on the path with the same id.
            let path = UnsafeBufferPointer(start: nodes, count: count).compactMap { $0 }
            guard let target = path.first(where: { MarkupID(markdown_core_node_id($0)) == id }) else {
                preconditionFailure("the session's native document lacks a node its copy holds")
            }
            var scopes: UnsafeMutablePointer<markdown_core_scope>?
            var size = 0
            status = query(native, target, bytes, &scopes, &size)
            guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
            defer { markdown_core_scopes_free(scopes) }
            return UnsafeBufferPointer(start: scopes, count: size).map { Scope($0) }
        }
    }
}

extension Document {
    /// The source ranges of the node of this document that is `node` or, with
    /// its id, equals it, found by one canonical walk; `nil` when the document
    /// holds no such node.
    func places(holding node: MarkupRecord) -> [(start: Int, end: Int)]? {
        var walk = CanonicalWalk(root: record)
        var places: [(start: Int, end: Int)] = []
        while let item = walk.next() {
            guard let found = item.record, found.id == node.id else { continue }
            guard found === node || found == node else { return nil }
            walk.places(of: item, into: &places)
            return places
        }
        return nil
    }
}
