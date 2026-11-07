import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("tables retain groups and columns")
    func tableValues() throws {
        let texts: [MarkupRecord] = [
            TextRecord(fields(5), literal: "h"),
            TextRecord(fields(8), literal: "b"),
            TextRecord(fields(11), literal: "c"),
        ]
        let rows = texts.map { text in
            let id = text.id.value
            let cell = TableCellRecord(fields(id - 1), children: [text])
            return TableRowRecord(fields(id - 2), children: [cell])
        }
        let columns = [TableColumn(flow: .left), TableColumn(flow: .none)]
        let record = TableRecord(fields(2), columns: columns, head: [rows[0]], body: [rows[1], rows[2]])
        let root = document(holding: [record])
        let table = try #require(root.content.first as? Table)
        #expect((table.head[0].cells[0].content[0] as? Text)?.literal == "h")
        #expect((table.content[1].cells[0].content[0] as? Text)?.literal == "c")
        var visitor = RecordingWalkingVisitor()
        table.walk(with: &visitor)
        let kinds = visitor.events.filter { ["enter:Table", "enter:TableRow"].contains($0) }
        #expect(kinds == ["enter:Table", "enter:TableRow", "enter:TableRow", "enter:TableRow"])
        let dump = try root.dump(table, in: "")
        #expect(dump.contains("columns=[left,none] children=3"))
        #expect(dump.contains("TableBody children=2"))
        let empty = TableRecord(fields(2), columns: columns, head: [], body: [])
        let bare = try document(holding: [empty]).dump(in: "")
        #expect(bare.contains("TableHead children=0\n"))
        #expect(bare.contains("TableBody children=0\n"))
    }
}

/// A hand-built document holding `content`, every node an empty run at 0.
func document(holding content: [MarkupRecord]) -> Document {
    Document(
        record: DocumentRecord(
            fields(1),
            unit: .utf16,
            metadata: nil,
            content: content,
            footnotes: [],
            specimens: [],
            references: [],
            referenceLabels: [:]
        )
    )
}
