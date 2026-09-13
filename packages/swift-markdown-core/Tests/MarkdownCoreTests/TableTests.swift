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
            let table = ValueTree(records: [
                .table(
                    .init(
                        caption: nil,
                        columns: [TableColumn(alignment: .none, relative: width)],
                        head: [],
                        content: [],
                        foot: [],
                        scope: scope,
                        anchor: nil,
                        attributes: .empty
                    )
                )
            ]).value(at: 0, as: Table.self)
            #expect(table.dump().contains("columns=[none:\(expected)]"))
        }
    }

    @Test("tables retain groups, spans and direct block content")
    func tableValues() throws {
        let table = try groupedTable()
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
        let empty = ValueTree(records: [
            .table(
                .init(
                    caption: nil,
                    columns: table.columns,
                    head: [],
                    content: [],
                    foot: [],
                    scope: table.scope,
                    anchor: nil,
                    attributes: .empty
                )
            )
        ]).value(at: 0, as: Table.self)
        #expect(empty.dump().contains("TableHead children=0\n"))
        #expect(empty.dump().contains("TableFoot children=0\n"))
    }

}

private func groupedTable() throws -> Table {
    let parsed = try Document.parse("# head\n\nbody\n\n---\n")
    var records = parsed.tree.records
    var rows: [Int] = []
    for index in parsed.content.recordIndices {
        let cell = records.count
        records.append(
            .tableCell(
                .init(
                    rowspan: 1,
                    colspan: 2,
                    content: [index],
                    scope: parsed.scope,
                    anchor: nil,
                    attributes: .empty
                )
            )
        )
        rows.append(records.count)
        records.append(.tableRow(.init(cells: [cell], scope: parsed.scope, anchor: nil, attributes: .empty)))
    }
    let index = records.count
    records.append(
        .table(
            .init(
                caption: nil,
                columns: [
                    TableColumn(alignment: .left, relative: 0.1), TableColumn(alignment: .none, relative: nil),
                ],
                head: [rows[0]],
                content: [rows[1]],
                foot: [rows[2]],
                scope: parsed.scope,
                anchor: nil,
                attributes: .empty
            )
        )
    )
    return ValueTree(records: records).value(at: index, as: Table.self)
}
