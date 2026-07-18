import MarkdownCoreC

public enum TableAlignment: String, Sendable {
    case none
    case left
    case center
    case right
}

public struct Table: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let alignments: [TableAlignment]
    public let header: TableRow
    public let rows: [TableRow]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Table {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
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
            id: id,
            revision: revision,
            alignments: alignments,
            header: headers[0],
            rows: rows.filter { !$0.isHeader }
        )
    }
}

public struct TableRow: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let isHeader: Bool
    public let cells: [TableCell]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableRow {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var header = false
        markdown_core_node_table_row_is_header(node, &header)
        let cells = builder.children(node).map { child -> TableCell in
            guard let cell = child as? TableCell else {
                preconditionFailure("table row contains a non-cell node")
            }
            return cell
        }
        self.init(id: id, revision: revision, isHeader: header, cells: cells)
    }
}

public struct TableCell: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let content: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableCell {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        self.init(id: id, revision: revision, content: builder.children(node))
    }
}
