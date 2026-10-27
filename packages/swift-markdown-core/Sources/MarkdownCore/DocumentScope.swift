import MarkdownCoreC

extension Document {
    /// The editor scopes of `node`, one per run in source order, computed
    /// from its runs and `source`, with columns in the document's ``unit``;
    /// extents take no part.
    ///
    /// A node's source ranges are its runs. Its first run leads from where
    /// the source of the node before it in its relation ends, so each call
    /// walks the document once to place the node and reads the source for
    /// its lines; nothing is cached.
    ///
    /// - Parameters:
    ///   - node: a node of this document.
    ///   - source: the source the document was parsed from.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before `node` does.
    public func scope(of node: some Markup, in source: String) throws -> [Scope] {
        let places = self.places(of: MarkupRecord.of(node))
        var text = source
        return try text.withUTF8 { bytes in
            guard (places.last?.end ?? 0) <= bytes.count else { throw MarkdownCoreError(code: .outOfBounds) }
            let lines = SourceLines(bytes)
            return places.map { lines.scope(from: $0.start, to: $0.end, in: bytes, unit: unit) }
        }
    }

    /// The last node in canonical walk order one of whose source ranges holds
    /// the byte at `position`, with the column counted in the document's
    /// ``unit``: the last node of ``path(at:in:)``.
    ///
    /// - Parameters:
    ///   - position: a line and column of `source`.
    ///   - source: the source the document was parsed from.
    /// - Returns: the node, or `nil` when no node holds the byte or the
    ///   position names no byte of the source at a scalar boundary.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when the
    ///   position's line or column is below 1.
    public func node(at position: Position, in source: String) throws -> (any Markup)? {
        try path(at: position, in: source).last
    }

    /// Every node one of whose source ranges holds the byte at `position`, in
    /// canonical walk order, so the root first and ``node(at:in:)``'s node
    /// last, with the column counted in the document's ``unit``.
    ///
    /// - Parameters:
    ///   - position: a line and column of `source`.
    ///   - source: the source the document was parsed from.
    /// - Returns: the nodes, none when no node holds the byte or the position
    ///   names no byte of the source at a scalar boundary.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when the
    ///   position's line or column is below 1.
    public func path(at position: Position, in source: String) throws -> [any Markup] {
        guard position.line >= 1, position.column >= 1 else { throw MarkdownCoreError(code: .outOfBounds) }
        var text = source
        return text.withUTF8 { bytes in
            guard let offset = SourceLines(bytes).offset(of: position, in: bytes, unit: unit) else { return [] }
            return holders(of: offset).map(\.markup)
        }
    }

    /// The source ranges of `node`'s fields of slot kind `field`, one per run
    /// of each, in source order: the bytes of each value as written, escapes
    /// included, without the quotes, the angle brackets or the sigil around
    /// it. `nil` asks for the fields no slot kind names, such as a title or a
    /// literal. A field the parser makes rather than reads, such as a
    /// heading's computed anchor, has none.
    ///
    /// A node's fields and its ``markers(of:in:)`` divide its own source: its
    /// runs less the runs of the nodes it holds. Each query reads the node's
    /// own source again with the scanners its element read it with, from a
    /// native parse of `source`, since the document holds none.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before `node` does or is not the document's source, and
    ///   ``ErrorCode/allocationFailed`` when an allocation fails.
    public func scope(of node: some Markup, field: Slot.Kind?, in source: String) throws -> [Scope] {
        try parts(of: node, in: source) { document, native, bytes, scopes, count in
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
    /// line endings between them; see ``scope(of:field:in:)``.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before `node` does or is not the document's source, and
    ///   ``ErrorCode/allocationFailed`` when an allocation fails.
    public func markers(of node: some Markup, in source: String) throws -> [Scope] {
        try parts(of: node, in: source) { document, native, bytes, scopes, count in
            markdown_core_document_markers(document, native, bytes.baseAddress, bytes.count, &scopes, &count)
        }
    }

    /// The canonical dump of the document, with scopes computed from `source`,
    /// the source the document was parsed from, in UTF-8 columns.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before a node of the document does.
    public func dump(in source: String) throws -> String {
        try dump(self, in: source)
    }

    /// The canonical dump of `node` and everything under it, with scopes
    /// computed from `source`, the source the document was parsed from, in
    /// UTF-8 columns.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before a node of the tree does.
    public func dump(_ node: some Markup, in source: String) throws -> String {
        let top = MarkupRecord.of(node)
        var text = source
        return try text.withUTF8 { bytes in
            // A node's first run leads from where the source of the node
            // before it ends, so the walk starts at the document's root.
            guard let output = MarkupDumper.render(top, in: record, bytes: bytes, lines: SourceLines(bytes)) else {
                throw MarkdownCoreError(code: .outOfBounds)
            }
            return output
        }
    }

    /// The records one of whose source ranges holds the byte at `offset`, in
    /// canonical walk order, found by one walk.
    private func holders(of offset: Int) -> [MarkupRecord] {
        var walk = CanonicalWalk(root: record)
        var places: [(start: Int, end: Int)] = []
        var found: [MarkupRecord] = []
        while let item = walk.next() {
            guard let node = item.record else { continue }
            walk.places(of: item, into: &places)
            if places.contains(where: { $0.start <= offset && offset < $0.end }) { found.append(node) }
        }
        return found
    }

    /// A field or marker query of `node` against a native parse of `source`.
    ///
    /// The native node is found where the native path query finds it: the
    /// path at the first byte `node` holds lists the same nodes in the same
    /// order in both trees, so `node`'s place in this document's path is its
    /// place in the native one. A node that holds no byte owns no source.
    private func parts(
        of node: some Markup,
        in source: String,
        _ query: (
            OpaquePointer,
            OpaquePointer,
            UnsafeBufferPointer<UInt8>,
            inout UnsafeMutablePointer<markdown_core_scope>?,
            inout Int
        ) -> markdown_core_status
    ) throws -> [Scope] {
        let target = MarkupRecord.of(node)
        guard let first = places(of: target).first(where: { $0.start < $0.end })?.start else { return [] }
        let rank = holders(of: first).firstIndex { $0 === target } ?? 0
        var text = source
        return try text.withUTF8 { bytes in
            guard first < bytes.count else { throw MarkdownCoreError(code: .outOfBounds) }
            var document: OpaquePointer?
            var status = markdown_core_document_parse_in(bytes.baseAddress, bytes.count, unit.native, &document)
            guard status == MARKDOWN_CORE_OK, let document else { throw MarkdownCoreError(status) }
            defer { markdown_core_document_free(document) }
            let position = SourceLines(bytes).scope(from: first, to: first + 1, in: bytes, unit: unit).start
            var nodes: UnsafeMutablePointer<OpaquePointer?>?
            var count = 0
            status = markdown_core_document_path(
                document,
                position.native,
                bytes.baseAddress,
                bytes.count,
                &nodes,
                &count
            )
            guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
            defer { markdown_core_path_free(nodes) }
            guard rank < count, let native = nodes?[rank], Self.same(target, native) else {
                throw MarkdownCoreError(code: .outOfBounds)
            }
            var scopes: UnsafeMutablePointer<markdown_core_scope>?
            var found = 0
            status = query(document, native, bytes, &scopes, &found)
            guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
            defer { markdown_core_scopes_free(scopes) }
            return UnsafeBufferPointer(start: scopes, count: found).map { Scope($0) }
        }
    }

    /// Whether `native` has `record`'s runs, as the node a parse of another
    /// source would not.
    private static func same(_ record: MarkupRecord, _ native: OpaquePointer) -> Bool {
        var count = 0
        let runs = markdown_core_node_runs(native, &count)
        return UnsafeBufferPointer(start: runs, count: count).map { Run($0) } == record.runs
    }

    /// The source ranges of `node`, found by one canonical walk.
    private func places(of node: MarkupRecord) -> [(start: Int, end: Int)] {
        var walk = CanonicalWalk(root: record)
        var places: [(start: Int, end: Int)] = []
        while let item = walk.next() {
            if item.record === node {
                walk.places(of: item, into: &places)
                break
            }
        }
        return places
    }
}

/// The lines of a source: the offset each begins at. A line ends after LF,
/// after CR, or after CRLF, which is one terminator.
struct SourceLines {
    let starts: [Int]

    init(_ bytes: UnsafeBufferPointer<UInt8>) {
        var starts = [0]
        for index in bytes.indices {
            let byte = bytes[index]
            if byte == 0x0A || (byte == 0x0D && !(index + 1 < bytes.count && bytes[index + 1] == 0x0A)) {
                starts.append(index + 1)
            }
        }
        self.starts = starts
    }

    /// The index of the line holding `offset`: the last line starting at or
    /// before it.
    func line(of offset: Int) -> Int {
        var low = 0
        var high = starts.count
        while high - low > 1 {
            let middle = low + (high - low) / 2
            if starts[middle] <= offset {
                low = middle
            } else {
                high = middle
            }
        }
        return low
    }

    /// A byte range as editor coordinates: the start is the position of its
    /// first byte, and the end the line holding its exclusive end and the
    /// columns from that line's start to it.
    func scope(from start: Int, to end: Int, in bytes: UnsafeBufferPointer<UInt8>, unit: TextUnit) -> Scope {
        let startLine = line(of: start)
        let endLine = line(of: end)
        return Scope(
            start: Position(
                line: Int32(startLine + 1),
                column: Int32(Self.columns(bytes, from: starts[startLine], to: start, unit: unit) + 1)
            ),
            end: Position(
                line: Int32(endLine + 1),
                column: Int32(Self.columns(bytes, from: starts[endLine], to: end, unit: unit))
            )
        )
    }

    /// The offset of the byte at `position`, whose line and column are at
    /// least 1, stepping over its line's scalars up to the column; `nil`
    /// unless the position lands on a byte of the line at a scalar boundary.
    func offset(of position: Position, in bytes: UnsafeBufferPointer<UInt8>, unit: TextUnit) -> Int? {
        let line = Int(position.line) - 1
        guard line < starts.count else { return nil }
        var offset = starts[line]
        let end = line + 1 < starts.count ? starts[line + 1] : bytes.count
        var column = 1
        while column < Int(position.column) {
            guard offset < end else { return nil }
            var next = offset + 1
            while next < end && (bytes[next] & 0xC0) == 0x80 { next += 1 }
            column += Self.columns(bytes, from: offset, to: next, unit: unit)
            offset = next
        }
        guard column == Int(position.column), offset < end else { return nil }
        return offset
    }

    /// The columns between two offsets of one line, in `unit`. A UTF-8 byte
    /// that begins a four-byte scalar is two UTF-16 units; a continuation byte
    /// is none.
    static func columns(_ bytes: UnsafeBufferPointer<UInt8>, from start: Int, to end: Int, unit: TextUnit) -> Int {
        switch unit {
        case .utf8:
            return end - start
        case .utf16:
            var units = 0
            for index in start..<end {
                let byte = bytes[index]
                units += (byte & 0xC0) == 0x80 ? 0 : byte >= 0xF0 ? 2 : 1
            }
            return units
        }
    }
}
