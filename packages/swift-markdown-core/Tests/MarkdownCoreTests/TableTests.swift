import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("table widths use canonical decimal notation")
    func tableColumnWidths() throws {
        let scope = try Document.parse("x").scope
        let widths: [(Double, String)] = [
            (0.1, "0.1"), (1e-6, "0.000001"), (1e-7, "1e-7"),
            (1e20, "100000000000000000000"), (1e21, "1e+21"), (.leastNonzeroMagnitude, "5e-324"),
            (1.2345678901234567, "1.2345678901234567"),
        ]
        for (width, expected) in widths {
            let table = Table(
                caption: nil,
                columns: [TableColumn(alignment: .none, relative: width)],
                head: [],
                content: [],
                foot: [],
                scope: scope,
                anchor: nil,
                attributes: .empty
            )
            #expect(table.dump().contains("columns=[none:\(expected)]"))
        }
    }

    @Test("tables retain groups, spans and direct block content")
    func tableValues() throws {
        let head = try Document.parse("# head")
        let body = try Document.parse("body")
        let foot = try Document.parse("---")
        let table = Table(
            caption: nil,
            columns: [TableColumn(alignment: .left, relative: 0.1), TableColumn(alignment: .none, relative: nil)],
            head: [row(head)],
            content: [row(body)],
            foot: [row(foot)],
            scope: body.scope,
            anchor: nil,
            attributes: .empty
        )
        #expect(table.head[0].cells[0].content[0] is Heading)
        #expect(table.foot[0].cells[0].content[0] is ThematicBreak)
        #expect(table.content[0].cells[0].colspan == 2)
        var visitor = RecordingWalkingVisitor()
        table.walk(with: &visitor)
        let kinds = visitor.events.filter {
            ["entering:Heading", "entering:Paragraph", "entering:ThematicBreak"].contains($0)
        }
        #expect(kinds == ["entering:Heading", "entering:Paragraph", "entering:ThematicBreak"])
        #expect(table.dump().contains("columns=[left:0.1,none:null] children=3"))
        #expect(table.dump().contains("TableFoot children=1"))
        let empty = Table(
            caption: nil,
            columns: table.columns,
            head: [],
            content: [],
            foot: [],
            scope: table.scope,
            anchor: nil,
            attributes: .empty
        )
        #expect(empty.dump().contains("TableHead children=0\n"))
        #expect(empty.dump().contains("TableFoot children=0\n"))
    }

}

private func row(_ document: Document) -> TableRow {
    TableRow(
        cells: [
            TableCell(
                rowspan: 1,
                colspan: 2,
                content: document.content,
                scope: document.scope,
                anchor: nil,
                attributes: .empty
            )
        ],
        scope: document.scope,
        anchor: nil,
        attributes: .empty
    )
}
