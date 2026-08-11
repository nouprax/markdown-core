import MarkdownCoreC

/// A table column's declared alignment.
public enum TableAlignment: String, Sendable {
    case none
    case left
    case center
    case right
}

/// A pipe table (the tables extension) with one header row.
public struct Table: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// Per-column alignments, one entry per column.
    public let alignments: [TableAlignment]
    /// The single header row.
    public let header: TableRow
    /// The body rows, header excluded.
    public let rows: [TableRow]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Table {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var count = 0
        markdown_core_node_table_column_count(node, &count)
        let alignments = (0..<count).map { index in
            var alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
            markdown_core_node_table_alignment_at(node, index, &alignment)
            return TableAlignment(from: alignment)
        }
        let rows = builder.children(node).map { child -> TableRow in
            guard let row = child as? TableRow else {
                preconditionFailure("table contains a non-row node")
            }
            return row
        }
        let headers = rows.filter(\.isHeader)
        precondition(headers.count == 1, "table must contain exactly one header row")
        self.init(
            id: track.id,
            revision: track.revision,
            scope: track.scope,
            alignments: alignments,
            header: headers[0],
            rows: rows.filter { !$0.isHeader }
        )
    }
}

/// One row of a `Table`.
public struct TableRow: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// Whether this is the table's header row.
    public let isHeader: Bool
    /// The row's cells in column order.
    public let cells: [TableCell]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableRow {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        var header = false
        markdown_core_node_table_row_is_header(node, &header)
        let cells = builder.children(node).map { child -> TableCell in
            guard let cell = child as? TableCell else {
                preconditionFailure("table row contains a non-cell node")
            }
            return cell
        }
        self.init(id: track.id, revision: track.revision, scope: track.scope, isHeader: header, cells: cells)
    }
}

/// One cell of a `TableRow`.
public struct TableCell: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The node's absolute source extent, both bounds inclusive of the
    /// construct's own markers.
    ///
    /// A property OF the node, not of a lookup: a document is an immutable
    /// projection of one text, so a node in it does not move. It is
    /// deliberately absent from `==` — position is not content — so an edit
    /// above this node leaves every reactive comparison below it untouched.
    public let scope: Scope
    /// The cell's inline content.
    public let content: [any Markup]

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableCell {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let track = builder.track(of: node)
        self.init(id: track.id, revision: track.revision, scope: track.scope, content: builder.children(node))
    }
}
