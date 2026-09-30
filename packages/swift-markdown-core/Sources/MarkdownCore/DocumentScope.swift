extension Document {
    /// The editor scope of `node`, computed from the extents and `source`, with
    /// columns in the document's ``unit``.
    ///
    /// Each call walks the document once to place the node and reads the
    /// source for its lines; nothing is cached.
    ///
    /// - Parameters:
    ///   - node: a node of this document.
    ///   - source: the source the document was parsed from.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before `node` does.
    public func scope(of node: some Markup, in source: String) throws -> Scope {
        let range = place(of: MarkupRecord.of(node))
        var text = source
        return try text.withUTF8 { bytes in
            guard range.end <= bytes.count else { throw MarkdownCoreError(code: .outOfBounds) }
            return SourceLines(bytes).scope(from: range.start, to: range.end, in: bytes, unit: unit)
        }
    }

    /// The last node in canonical walk order whose source range holds the
    /// byte at `position`, with the column counted in the document's ``unit``.
    ///
    /// - Parameters:
    ///   - position: a line and column of `source`.
    ///   - source: the source the document was parsed from.
    /// - Returns: the node, or `nil` when no node holds the byte or the
    ///   position names no byte of the source at a scalar boundary.
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when the
    ///   position's line or column is below 1.
    public func node(at position: Position, in source: String) throws -> (any Markup)? {
        guard position.line >= 1, position.column >= 1 else { throw MarkdownCoreError(code: .outOfBounds) }
        var text = source
        return text.withUTF8 { bytes in
            guard let offset = SourceLines(bytes).offset(of: position, in: bytes, unit: unit) else { return nil }
            var walk = CanonicalWalk(root: record, anchor: 0)
            var found: MarkupRecord?
            while let item = walk.next() {
                if let node = item.record, item.start <= offset, offset < item.end { found = node }
            }
            return found?.markup
        }
    }

    /// The canonical dump of the document, with scopes computed from `source`,
    /// the source the document was parsed from, in UTF-8 columns.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before the document does.
    public func dump(in source: String) throws -> String {
        try dump(self, in: source)
    }

    /// The canonical dump of `node` and everything under it, with scopes
    /// computed from `source`, the source the document was parsed from, in
    /// UTF-8 columns.
    ///
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/outOfBounds`` when
    ///   `source` ends before `node` does.
    public func dump(_ node: some Markup, in source: String) throws -> String {
        let target = MarkupRecord.of(node)
        let range = place(of: target)
        var text = source
        return try text.withUTF8 { bytes in
            guard range.end <= bytes.count else { throw MarkdownCoreError(code: .outOfBounds) }
            // A node's walk starts at its own extent, which is relative to the
            // anchor its relation had where it was written.
            return MarkupDumper.render(
                target,
                anchor: range.start - Int(target.extent.lead),
                bytes: bytes,
                lines: SourceLines(bytes)
            )
        }
    }

    /// The absolute byte range of `target`, found by one canonical walk.
    private func place(of target: MarkupRecord) -> (start: Int, end: Int) {
        var walk = CanonicalWalk(root: record, anchor: 0)
        var range = (start: 0, end: 0)
        while let item = walk.next() {
            if item.record === target {
                range = (item.start, item.end)
                break
            }
        }
        return range
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
