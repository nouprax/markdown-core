import MarkdownCoreC

/// One column's alignment, as its delimiter row spelled it.
public enum TableAlignment: String, Sendable {
    /// The delimiter row carried no colon for this column.
    case none
    /// `:---`
    case left
    /// `:---:`
    case center
    /// `---:`
    case right
}

/// A GFM table. Requires the `tables` extension.
public struct Table: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// One entry per column, from the delimiter row. A row may hold fewer
    /// cells than this; the trailing columns are simply absent from it.
    public let alignments: [TableAlignment]
    /// The header row. A table cannot exist without one — the delimiter row is
    /// what makes the line above it a header rather than a paragraph.
    public let header: TableRow
    /// The body rows, header excluded. Empty is a valid table.
    public let rows: [TableRow]
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope

    /// Dispatches to the visitor's `Table` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Table {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var count = 0
        markdown_core_node_table_column_count(node, &count)
        let alignments = (0..<count).map { index in
            var alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
            markdown_core_node_table_alignment_at(node, index, &alignment)
            return TableAlignment(from: alignment)
        }
        let rows: [TableRow] = Self.typedChildren(from: node)
        let headers = rows.filter(\.isHeader)
        precondition(headers.count == 1, "table must contain exactly one header row")
        self.init(
            id: id,
            alignments: alignments,
            header: headers[0],
            rows: rows.filter { !$0.isHeader },
            scope: Self.scope(from: node)
        )
    }
}

/// One row of a ``Table``.
public struct TableRow: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// True only for the row reached through ``Table/header``, and false for
    /// every entry in ``Table/rows``.
    public let isHeader: Bool
    /// The row's cells, in source order. A row may be short; it is not padded.
    public let cells: [TableCell]
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope

    /// Dispatches to the visitor's `TableRow` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableRow {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        var header = false
        markdown_core_node_table_row_is_header(node, &header)
        let cells: [TableCell] = Self.typedChildren(from: node)
        self.init(
            id: id,
            isHeader: header,
            cells: cells,
            scope: Self.scope(from: node)
        )
    }
}

/// One cell of a ``TableRow``.
public struct TableCell: Markup {
    /// The node's identity: the name a consumer tracks this element by across
    /// a stream's feeds — the render key. See ``Identity``.
    public let id: Identity
    /// The cell's inline content.
    public let content: [any Markup]
    /// Where it is. A cell the parser completed to fill a short row has a
    /// scope but no source behind it. See ``Scope``.
    public let scope: Scope

    /// Dispatches to the visitor's `TableCell` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableCell {
    init(from node: OpaquePointer) {
        let id = Self.identity(from: node)
        self.init(id: id, content: Self.children(from: node), scope: Self.scope(from: node))
    }
}
