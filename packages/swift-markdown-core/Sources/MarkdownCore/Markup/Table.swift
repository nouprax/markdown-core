import MarkdownCoreC

/// One logical column; no width is authored by a pipe table.
public struct TableColumn: Sendable, Hashable {
    /// Alignment of this logical column.
    public let flow: Flow
    /// Positive finite authored width share, or nil when no width was authored.
    public let relative: Double?
}

/// One table model for every table syntax. Rows belong to their named group.
public struct Table: Markup {
    let record: TableRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The independently owned inline caption, visited before all rows.
    public var caption: TableCaption? {
        record.captionCount == 0
            ? nil : TableCaption(record: unsafeDowncast(record.children[0], to: TableCaptionRecord.self))
    }
    /// The non-empty logical column grid.
    public var columns: [TableColumn] { record.columns }
    /// Header rows in stored order.
    public var head: MarkupCollection<TableRow> { record.collection(record.head) }
    /// Body rows in stored order.
    public var content: MarkupCollection<TableRow> { record.collection(record.body) }
    /// Footer rows in stored order.
    public var foot: MarkupCollection<TableRow> { record.collection(record.foot) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

/// Children: the caption, when authored, then the head, body and foot rows.
final class TableRecord: MarkupRecord, @unchecked Sendable {
    let columns: [TableColumn]
    let captionCount: Int
    let headCount: Int
    let bodyCount: Int

    init(
        _ fields: InheritedFields,
        columns: [TableColumn],
        caption: TableCaptionRecord?,
        head: [MarkupRecord],
        body: [MarkupRecord],
        foot: [MarkupRecord]
    ) {
        let captions: [MarkupRecord] = caption.map { [$0] } ?? []
        self.columns = columns
        captionCount = captions.count
        headCount = head.count
        bodyCount = body.count
        super.init(fields, children: captions + head + body + foot)
    }

    var head: Range<Int> { captionCount..<(captionCount + headCount) }
    var body: Range<Int> { head.upperBound..<(head.upperBound + bodyCount) }
    var foot: Range<Int> { body.upperBound..<children.count }

    override var markup: any Markup { Table(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: TableRecord.self)
        return columns == other.columns && captionCount == other.captionCount && headCount == other.headCount
            && bodyCount == other.bodyCount
    }

    override func relation(at step: Int) -> Relation? {
        switch step {
        case 0: Relation(name: nil, indices: 0..<captionCount)
        case 1: Relation(name: "TableHead", indices: head)
        case 2: Relation(name: "TableBody", indices: body)
        case 3: Relation(name: "TableFoot", indices: foot)
        default: nil
        }
    }
}

extension TableRecord {
    convenience init(from node: OpaquePointer, caption: TableCaptionRecord?, rows: [MarkupRecord]) {
        var count = 0
        var headCount = 0
        var bodyCount = 0
        var footCount = 0
        precondition(markdown_core_node_table_properties(node, &count, &headCount, &bodyCount, &footCount))
        let columns = (0..<count).map { index in
            var column = markdown_core_table_column()
            precondition(markdown_core_node_table_column_at(node, index, &column))
            return TableColumn(
                flow: Flow(from: column.flow),
                relative: column.relative.has_value ? column.relative.value : nil
            )
        }
        precondition(headCount + bodyCount + footCount == rows.count)
        self.init(
            InheritedFields(from: node),
            columns: columns,
            caption: caption,
            head: Array(rows[..<headCount]),
            body: Array(rows[headCount..<(headCount + bodyCount)]),
            foot: Array(rows[(headCount + bodyCount)...])
        )
    }
}

extension Table: RecordBacked {
    var base: MarkupRecord { record }
}

/// Cells whose upper-left coordinate starts in this row, in logical order.
public struct TableRow: Markup {
    let record: TableRowRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Cells starting in this row, in logical column order.
    public var cells: MarkupCollection<TableCell> { record.collection(record.children.indices) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
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

/// One cell; its spans are positive and cannot cross a row-group boundary.
public struct TableCell: Markup {
    let record: TableCellRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Number of rows occupied within this row group.
    public var rowspan: Int { record.rowspan }
    /// Number of logical columns occupied.
    public var colspan: Int { record.colspan }
    /// Inline or block content as parsed, without paragraph normalization.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Authored source extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class TableCellRecord: MarkupRecord, @unchecked Sendable {
    let rowspan: Int
    let colspan: Int

    init(_ fields: InheritedFields, rowspan: Int, colspan: Int, content: [MarkupRecord]) {
        self.rowspan = rowspan
        self.colspan = colspan
        super.init(fields, children: content)
    }

    override var markup: any Markup { TableCell(record: self) }

    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        let other = unsafeDowncast(other, to: TableCellRecord.self)
        return rowspan == other.rowspan && colspan == other.colspan
    }
}

extension TableCellRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        var rowspan: Int64 = 0
        var colspan: Int64 = 0
        precondition(markdown_core_node_table_cell_spans(node, &rowspan, &colspan))
        self.init(InheritedFields(from: node), rowspan: Int(rowspan), colspan: Int(colspan), content: content)
    }
}

extension TableCell: RecordBacked {
    var base: MarkupRecord { record }
}

/// A table's authored caption, with ordinary inline content.
public struct TableCaption: Markup {
    let record: TableCaptionRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// Inline content after removing the caption marker.
    public var content: MarkupCollection<any Markup> { record.collection(record.children.indices) }
    /// Authored source extent, including the caption marker.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
}

final class TableCaptionRecord: MarkupRecord, @unchecked Sendable {
    override var markup: any Markup { TableCaption(record: self) }
}

extension TableCaptionRecord {
    convenience init(from node: OpaquePointer, content: [MarkupRecord]) {
        self.init(InheritedFields(from: node), children: content)
    }
}

extension TableCaption: RecordBacked {
    var base: MarkupRecord { record }
}
