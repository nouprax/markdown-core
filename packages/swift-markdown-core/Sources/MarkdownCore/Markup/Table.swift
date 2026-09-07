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

/// One logical column; no width is authored by a pipe table.
public struct TableColumn: Sendable {
    /// Alignment of this logical column.
    public let alignment: TableAlignment
    /// Positive finite authored width share, or nil when no width was authored.
    public let relative: Double?
}

/// One table model for every table syntax. Rows belong to their named group.
public struct Table: Markup {
    /// The non-empty logical column grid.
    public let columns: [TableColumn]
    /// Header rows in stored order.
    public let head: [TableRow]
    /// Body rows in stored order.
    public let content: [TableRow]
    /// Footer rows in stored order.
    public let foot: [TableRow]
    /// Authored source extent. See ``Scope``.
    public let scope: Scope

    /// Dispatches to this node kind's visitor callback.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Table {
    init(from node: OpaquePointer, children: [any Markup]) {
        var count = 0
        var headCount = 0
        var contentCount = 0
        var footCount = 0
        precondition(markdown_core_node_table_properties(node, &count, &headCount, &contentCount, &footCount))
        let columns = (0..<count).map { index in
            var column = markdown_core_table_column()
            precondition(markdown_core_node_table_column_at(node, index, &column))
            return TableColumn(
                alignment: TableAlignment(from: column.alignment),
                relative: column.relative.has_value ? column.relative.value : nil
            )
        }
        let rows: [TableRow] = Self.typedChildren(children)
        precondition(headCount + contentCount + footCount == rows.count)
        self.init(
            columns: columns,
            head: Array(rows[..<headCount]),
            content: Array(rows[headCount..<(headCount + contentCount)]),
            foot: Array(rows[(headCount + contentCount)...]),
            scope: Self.scope(from: node)
        )
    }
}

/// Cells whose upper-left coordinate starts in this row, in logical order.
public struct TableRow: Markup {
    /// Cells starting in this row, in logical column order.
    public let cells: [TableCell]
    /// Authored source extent. See ``Scope``.
    public let scope: Scope

    /// Dispatches to this node kind's visitor callback.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableRow {
    init(from node: OpaquePointer, children: [any Markup]) {
        self.init(cells: Self.typedChildren(children), scope: Self.scope(from: node))
    }
}

/// One cell; its spans are positive and cannot cross a row-group boundary.
public struct TableCell: Markup {
    /// Number of rows occupied within this row group.
    public let rowspan: Int
    /// Number of logical columns occupied.
    public let colspan: Int
    /// Inline or block content as parsed, without paragraph normalization.
    public let content: [any Markup]
    /// Authored source extent. See ``Scope``.
    public let scope: Scope

    /// Dispatches to this node kind's visitor callback.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableCell {
    init(from node: OpaquePointer, content: [any Markup]) {
        var rowspan: Int64 = 0
        var colspan: Int64 = 0
        precondition(markdown_core_node_table_cell_spans(node, &rowspan, &colspan))
        self.init(rowspan: Int(rowspan), colspan: Int(colspan), content: content, scope: Self.scope(from: node))
    }
}
