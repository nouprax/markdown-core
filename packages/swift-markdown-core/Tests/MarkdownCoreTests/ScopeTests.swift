import MarkdownCore
import Testing

@Suite("scope") struct ScopeSuite {
    @Test("scopes count columns in the document's unit, across CRLF")
    func units() throws {
        let source = "é🚀\r\nx"
        let wide = try Document.parse(source, unit: .utf8)
        let narrow = try Document.parse(source)
        #expect(wide.unit == .utf8 && narrow.unit == .utf16)
        // The unit is how positions are counted, not what was parsed.
        #expect(wide == narrow)
        // The first text's start and end columns, then the soft break's.
        let cases: [(Document, [Int32])] = [(wide, [1, 6, 7, 7]), (narrow, [1, 3, 4, 4])]
        for (document, columns) in cases {
            let paragraph = try #require(document.content.first as? Paragraph)
            #expect(
                try scope(of: paragraph.content[0], in: document, source: source)
                    == Scope(start: Position(line: 1, column: columns[0]), end: Position(line: 1, column: columns[1]))
            )
            #expect(
                try scope(of: paragraph.content[1], in: document, source: source)
                    == Scope(start: Position(line: 1, column: columns[2]), end: Position(line: 1, column: columns[3]))
            )
            #expect(
                try scope(of: paragraph.content[2], in: document, source: source)
                    == Scope(start: Position(line: 2, column: 1), end: Position(line: 2, column: 1))
            )
            #expect(
                try scope(of: document, in: document, source: source)
                    == Scope(start: Position(line: 1, column: 1), end: Position(line: 2, column: 1))
            )
        }
    }

    @Test("a position names a byte of its line at a scalar boundary, and finds the last node holding it")
    func hitTesting() throws {
        let source = "é🚀\r\nx"
        // Ids: 1 document, 2 paragraph, 3 "é🚀", 4 the soft break (the CR),
        // 5 "x". Zero is no node.
        let expected: [(TextUnit, [UInt64])] = [
            (.utf8, [3, 0, 3, 0, 0, 0, 4, 2, 0]),
            (.utf16, [3, 3, 0, 4, 2, 0, 0, 0, 0]),
        ]
        for (unit, ids) in expected {
            let document = try Document.parse(source, unit: unit)
            let found = (1...Int32(ids.count)).map { column in
                document.node(at: Position(line: 1, column: column), in: source)?.id.value ?? 0
            }
            #expect(found == ids, "\(unit)")
            #expect(document.node(at: Position(line: 2, column: 1), in: source)?.id.value == 5)
            #expect(document.node(at: Position(line: 2, column: 2), in: source) == nil)
            #expect(document.node(at: Position(line: 3, column: 1), in: source) == nil)
            #expect(document.node(at: Position(line: 0, column: 1), in: source) == nil)
            #expect(document.node(at: Position(line: 1, column: 0), in: source) == nil)
        }
    }

    @Test("an empty document and a lone line terminator are 1:1..1:0 and hold no byte", arguments: ["", "\n"])
    func empty(source: String) throws {
        for unit in [TextUnit.utf8, .utf16] {
            let document = try Document.parse(source, unit: unit)
            #expect(
                try scope(of: document, in: document, source: source)
                    == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 0))
            )
            #expect(document.node(at: Position(line: 1, column: 1), in: source) == nil)
        }
    }

    @Test("a node of another document or a source shorter than the document has no scope")
    func foreign() throws {
        let document = try Document.parse("one\n")
        let other = try #require(Document.parse("two\n").content.first)
        #expect(document.scope(of: other, in: "one\n") == nil)
        #expect(document.scope(of: document, in: "on") == nil)
        #expect(document.dump(in: "on") == nil)
        #expect(document.dump(other, in: "one\n") == nil)
    }
}
