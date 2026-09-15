import MarkdownCoreC

/// One logical column; no width is authored by a pipe table.
public struct TableColumn: Sendable {
    /// Alignment of this logical column.
    public let flow: Flow
    /// Positive finite authored width share, or nil when no width was authored.
    public let relative: Double?
}

/// One table model for every table syntax. Rows belong to their named group.
public struct Table: Markup {
    struct Fields: Sendable {
        let caption: MarkupReference<TableCaption>?
        let columns: [TableColumn]
        let head: MarkupReferences<TableRow>
        let content: MarkupReferences<TableRow>
        let foot: MarkupReferences<TableRow>
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
    }

    let fields: Stored<Fields>

    /// The independently owned inline caption, visited before all rows.
    public var caption: TableCaption? { fields.caption }
    /// The non-empty logical column grid.
    public var columns: [TableColumn] { fields.columns }
    /// Header rows in stored order.
    public var head: MarkupCollection<TableRow> { fields.head }
    /// Body rows in stored order.
    public var content: MarkupCollection<TableRow> { fields.content }
    /// Footer rows in stored order.
    public var foot: MarkupCollection<TableRow> { fields.foot }
    /// Authored source extent. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }

    /// Dispatches to this node kind's visitor callback.
}

extension Table.Fields {
    init(from node: OpaquePointer, caption: Int?, children: [Int]) {
        var count = 0
        var headCount = 0
        var contentCount = 0
        var footCount = 0
        precondition(markdown_core_node_table_properties(node, &count, &headCount, &contentCount, &footCount))
        let columns = (0..<count).map { index in
            var column = markdown_core_table_column()
            precondition(markdown_core_node_table_column_at(node, index, &column))
            return TableColumn(
                flow: Flow(from: column.flow),
                relative: column.relative.has_value ? column.relative.value : nil
            )
        }
        let rows: [Int] = children
        precondition(headCount + contentCount + footCount == rows.count)
        self.init(
            caption: caption.map { .init(index: $0) },
            columns: columns,
            head: .init(indices: Array(rows[..<headCount])),
            content: .init(indices: Array(rows[headCount..<(headCount + contentCount)])),
            foot: .init(indices: Array(rows[(headCount + contentCount)...])),
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}

/// Cells whose upper-left coordinate starts in this row, in logical order.
public struct TableRow: Markup {
    struct Fields: Sendable {
        let cells: MarkupReferences<TableCell>
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
    }

    let fields: Stored<Fields>

    /// Cells starting in this row, in logical column order.
    public var cells: MarkupCollection<TableCell> { fields.cells }
    /// Authored source extent. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }

    /// Dispatches to this node kind's visitor callback.
}

extension TableRow.Fields {
    init(from node: OpaquePointer, children: [Int]) {
        self.init(
            cells: .init(indices: children),
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}

/// One cell; its spans are positive and cannot cross a row-group boundary.
public struct TableCell: Markup {
    struct Fields: Sendable {
        let rowspan: Int
        let colspan: Int
        let content: MarkupReferences<any Markup>
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
    }

    let fields: Stored<Fields>

    /// Number of rows occupied within this row group.
    public var rowspan: Int { fields.rowspan }
    /// Number of logical columns occupied.
    public var colspan: Int { fields.colspan }
    /// Inline or block content as parsed, without paragraph normalization.
    public var content: MarkupCollection<any Markup> { fields.content }
    /// Authored source extent. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }

    /// Dispatches to this node kind's visitor callback.
}

extension TableCell.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        var rowspan: Int64 = 0
        var colspan: Int64 = 0
        precondition(markdown_core_node_table_cell_spans(node, &rowspan, &colspan))
        self.init(
            rowspan: Int(rowspan),
            colspan: Int(colspan),
            content: .init(indices: content),
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}

/// A table's authored caption, with ordinary inline content.
public struct TableCaption: Markup {
    struct Fields: Sendable {
        let content: MarkupReferences<any Markup>
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
    }

    let fields: Stored<Fields>

    /// Inline content after removing the caption marker.
    public var content: MarkupCollection<any Markup> { fields.content }
    /// Authored source extent, including the caption marker.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }

    /// Dispatches to this node kind's visitor callback.
}

extension TableCaption.Fields {
    init(from node: OpaquePointer, content: [Int]) {
        self.init(
            content: .init(indices: content),
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node)
        )
    }
}
