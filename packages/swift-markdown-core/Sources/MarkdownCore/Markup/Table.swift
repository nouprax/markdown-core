import MarkdownCoreC

/// One logical column.
public struct TableColumn: Sendable, Hashable {
    /// Alignment of this logical column.
    public let flow: Flow
}

/// A pipe table: one head row, then the body rows.
public struct Table: Markup {
    let record: TableRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The non-empty logical column grid.
    public var columns: [TableColumn] { record.columns }
    /// Header rows in stored order.
    public var head: MarkupCollection<TableRow> { record.collection(record.head) }
    /// Body rows in stored order.
    public var content: MarkupCollection<TableRow> { record.collection(record.body) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

/// Children: the head rows, then the body rows. The rows, in that order, are
/// one relation the dump draws as two groups: the body continues the head.
final class TableRecord: MarkupRecord, @unchecked Sendable {
    let columns: [TableColumn]
    let headCount: Int

    init(_ fields: InheritedFields, columns: [TableColumn], head: [MarkupRecord], body: [MarkupRecord]) {
        self.columns = columns
        headCount = head.count
        super.init(fields, children: head + body)
    }

    var head: Range<Int> { 0..<headCount }
    var body: Range<Int> { headCount..<children.count }

    override var markup: any Markup { Table(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: TableRecord.self)
        return columns == other.columns && headCount == other.headCount
    }

    override func relation(at step: Int) -> Relation? {
        switch step {
        case 0: Relation(name: "TableHead", indices: head)
        case 1: Relation(name: "TableBody", indices: body, continues: true)
        default: nil
        }
    }
}

extension TableRecord {
    convenience init(from node: OpaquePointer, rows: [MarkupRecord]) {
        var count = 0
        var headCount = 0
        var bodyCount = 0
        answered(markdown_core_node_table_properties(node, &count, &headCount, &bodyCount))
        let columns = (0..<count).map { index in
            let column = answer(markdown_core_table_column()) { markdown_core_node_table_column_at(node, index, $0) }
            return TableColumn(flow: Flow(from: column.flow))
        }
        self.init(
            InheritedFields(from: node),
            columns: columns,
            head: Array(rows[..<headCount]),
            body: Array(rows[headCount...])
        )
    }
}

extension Table: RecordBacked {
    var base: MarkupRecord { record }
}

/// One row's cells, in column order.
public struct TableRow: Markup {
    let record: TableRowRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The row's cells, in column order.
    public var cells: MarkupCollection<TableCell> { record.collection(record.children.indices) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class TableRowRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { TableRow(record: self) }
}

extension TableRowRecord {
    convenience init(from node: OpaquePointer, cells: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: cells)
    }
}

extension TableRow: RecordBacked {
    var base: MarkupRecord { record }
}

/// One cell, with inline content.
public struct TableCell: Markup {
    let record: TableCellRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Inline content as parsed.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// Its own source ranges, in source order. See ``Run``.
    public var runs: [Run] { record.runs }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class TableCellRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { TableCell(record: self) }
}

extension TableCellRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension TableCell: RecordBacked {
    var base: MarkupRecord { record }
}
