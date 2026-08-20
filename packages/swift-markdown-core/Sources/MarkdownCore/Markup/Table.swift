import MarkdownCoreC

public enum TableAlignment: String, Sendable {
    case none
    case left
    case center
    case right
}

public struct Table: Markup {
    public let alignments: [TableAlignment]
    public let header: TableRow
    public let rows: [TableRow]
    public let scope: Scope

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Table {
    init(from node: OpaquePointer) {
        var count = 0
        markdown_core_node_table_column_count(node, &count)
        let alignments = (0..<count).map { index in
            var alignment = MARKDOWN_CORE_TABLE_ALIGNMENT_NONE
            markdown_core_node_table_alignment_at(node, index, &alignment)
            return TableAlignment(from: alignment)
        }
        let rows = Self.children(from: node).map { child -> TableRow in
            guard let row = child as? TableRow else {
                preconditionFailure("table contains a non-row node")
            }
            return row
        }
        let headers = rows.filter(\.isHeader)
        precondition(headers.count == 1, "table must contain exactly one header row")
        self.init(
            alignments: alignments,
            header: headers[0],
            rows: rows.filter { !$0.isHeader },
            scope: Self.scope(from: node)
        )
    }
}

public struct TableRow: Markup {
    public let isHeader: Bool
    public let cells: [TableCell]
    public let scope: Scope

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableRow {
    init(from node: OpaquePointer) {
        var header = false
        markdown_core_node_table_row_is_header(node, &header)
        let cells = Self.children(from: node).map { child -> TableCell in
            guard let cell = child as? TableCell else {
                preconditionFailure("table row contains a non-cell node")
            }
            return cell
        }
        self.init(
            isHeader: header,
            cells: cells,
            scope: Self.scope(from: node)
        )
    }
}

public struct TableCell: Markup {
    public let content: [any Markup]
    public let scope: Scope

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension TableCell {
    init(from node: OpaquePointer) {
        self.init(content: Self.children(from: node), scope: Self.scope(from: node))
    }
}
