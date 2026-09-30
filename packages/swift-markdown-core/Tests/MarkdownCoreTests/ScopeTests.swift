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
            let found = try (1...Int32(ids.count)).map { column in
                try document.node(at: Position(line: 1, column: column), in: source)?.id.value ?? 0
            }
            #expect(found == ids, "\(unit)")
            #expect(try document.node(at: Position(line: 2, column: 1), in: source)?.id.value == 5)
            #expect(try document.node(at: Position(line: 2, column: 2), in: source) == nil)
            #expect(try document.node(at: Position(line: 3, column: 1), in: source) == nil)
        }
    }

    @Test(
        "a line or column below 1 is out of bounds",
        arguments: [(0, 1), (1, 0), (-1, 1), (1, -1), (Int32.min, Int32.min)] as [(Int32, Int32)]
    )
    func positionBelowOne(line: Int32, column: Int32) throws {
        let source = "a\n"
        let document = try Document.parse(source)
        #expect(outOfBounds { try document.node(at: Position(line: line, column: column), in: source) })
    }

    @Test("a source that ends before the node does is out of bounds for its scope and dump")
    func shortSource() throws {
        let source = "a\n\nb\u{E9}\n"
        let document = try Document.parse(source)
        let last = try #require(document.content.last as? Paragraph)
        // The last paragraph ends after "é", byte 6; the document does too,
        // since its range ends at its last content.
        for short in ["a\n\nb", "a\n\n", ""] {
            #expect(outOfBounds { try document.scope(of: last, in: short) })
            #expect(outOfBounds { try document.dump(last, in: short) })
            #expect(outOfBounds { try document.dump(in: short) })
        }
        // A source that covers the node answers, whatever follows it.
        let covering = "a\n\nb\u{E9}"
        #expect(
            try document.scope(of: last, in: covering)
                == Scope(start: Position(line: 3, column: 1), end: Position(line: 3, column: 2))
        )
        #expect(try document.dump(in: covering) == document.dump(in: source))
    }

    @Test("an empty document and a lone line terminator are 1:1..1:0 and hold no byte", arguments: ["", "\n"])
    func empty(source: String) throws {
        for unit in [TextUnit.utf8, .utf16] {
            let document = try Document.parse(source, unit: unit)
            #expect(
                try scope(of: document, in: document, source: source)
                    == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 0))
            )
            #expect(try document.node(at: Position(line: 1, column: 1), in: source) == nil)
        }
    }
}

/// Whether `call` throws the library's error with ``ErrorCode/outOfBounds``.
private func outOfBounds(_ call: () throws -> Any?) -> Bool {
    do {
        _ = try call()
        return false
    } catch {
        return (error as? MarkdownCoreError)?.code == .outOfBounds
    }
}
