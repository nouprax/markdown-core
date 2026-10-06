import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("table widths use canonical decimal notation")
    func tableColumnWidths() throws {
        let widths: [(Double, String)] = [
            (0.1, "0.1"), (1e-6, "0.000001"), (1e-7, "1e-7"),
            (1e20, "100000000000000000000"), (1e21, "1e+21"), (.leastNonzeroMagnitude, "5e-324"),
            (1.2345678901234567, "1.2345678901234567"),
        ]
        for (width, expected) in widths {
            let table = TableRecord(
                fields(2),
                columns: [TableColumn(flow: .none, relative: width)],
                caption: nil,
                head: [],
                body: [],
                foot: []
            )
            let dump = try document(holding: [table]).dump(in: "")
            #expect(dump.contains("columns=[none:\(expected)]"))
        }
    }

    @Test("tables retain groups, spans and direct block content")
    func tableValues() throws {
        let blocks: [MarkupRecord] = [
            HeadingRecord(fields(5), level: 1, content: []),
            ParagraphRecord(fields(8), children: []),
            ThematicBreakRecord(fields(11), children: []),
        ]
        let rows = blocks.map { block in
            let id = block.id.value
            let cell = TableCellRecord(fields(id - 1), rowspan: 1, colspan: 2, content: [block])
            return TableRowRecord(fields(id - 2), children: [cell])
        }
        let columns = [TableColumn(flow: .left, relative: 0.1), TableColumn(flow: .none, relative: nil)]
        let record = TableRecord(
            fields(2),
            columns: columns,
            caption: nil,
            head: [rows[0]],
            body: [rows[1]],
            foot: [rows[2]]
        )
        let root = document(holding: [record])
        let table = try #require(root.content.first as? Table)
        #expect(table.head[0].cells[0].content[0] is Heading)
        #expect(table.foot[0].cells[0].content[0] is ThematicBreak)
        #expect(table.content[0].cells[0].colspan == 2)
        var visitor = RecordingWalkingVisitor()
        table.walk(with: &visitor)
        let kinds = visitor.events.filter {
            ["enter:Heading", "enter:Paragraph", "enter:ThematicBreak"].contains($0)
        }
        #expect(kinds == ["enter:Heading", "enter:Paragraph", "enter:ThematicBreak"])
        let dump = try root.dump(table, in: "")
        #expect(dump.contains("columns=[left:0.1,none:null] children=3"))
        #expect(dump.contains("TableFoot children=1"))
        let empty = TableRecord(fields(2), columns: columns, caption: nil, head: [], body: [], foot: [])
        let bare = try document(holding: [empty]).dump(in: "")
        #expect(bare.contains("TableHead children=0\n"))
        #expect(bare.contains("TableFoot children=0\n"))
    }
}

/// A hand-built document holding `content`, with no source range.
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
