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
        // The first text's start and end columns, then the soft break's
        // start: it reads its CR LF whole, so it ends where line 2 starts.
        let cases: [(Document, [Int32])] = [(wide, [1, 6, 7]), (narrow, [1, 3, 4])]
        for (document, columns) in cases {
            let paragraph = try #require(document.content.first as? Paragraph)
            #expect(
                try scope(of: paragraph.content[0], in: document, source: source)
                    == Scope(start: Position(line: 1, column: columns[0]), end: Position(line: 1, column: columns[1]))
            )
            #expect(
                try scope(of: paragraph.content[1], in: document, source: source)
                    == Scope(start: Position(line: 1, column: columns[2]), end: Position(line: 2, column: 0))
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
        // Ids, in completion order: 1 paragraph, 2 "é🚀", 3 the soft break
        // (the CR LF, one decoded run it reads whole), 4 "x", 5 document.
        // Zero is no node.
        let expected: [(TextUnit, [UInt64])] = [
            (.utf8, [2, 0, 2, 0, 0, 0, 3, 3, 0]),
            (.utf16, [2, 2, 0, 3, 3, 0, 0, 0, 0]),
        ]
        for (unit, ids) in expected {
            let document = try Document.parse(source, unit: unit)
            let found = try (1...Int32(ids.count)).map { column in
                try document.node(at: Position(line: 1, column: column), in: source)?.id.value ?? 0
            }
            #expect(found == ids, "\(unit)")
            #expect(try document.node(at: Position(line: 2, column: 1), in: source)?.id.value == 4)
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
                == [Scope(start: Position(line: 3, column: 1), end: Position(line: 3, column: 2))]
        )
        #expect(try document.dump(in: covering) == document.dump(in: source))
    }

    @Test("a node in an inline root's content has a scope per source range its content was read from")
    func contentSources() throws {
        let source = "> a *b\n> c* d\n"
        let document = try Document.parse(source)
        let callout = try #require(document.content.first as? Callout)
        let block = try #require(callout.content.first as? Paragraph)
        let emphasis = try #require(block.content[1] as? Emphasis)
        func place(_ start: (Int32, Int32), _ end: (Int32, Int32)) -> Scope {
            Scope(start: Position(line: start.0, column: start.1), end: Position(line: end.0, column: end.1))
        }
        // The paragraph reads its own bytes, one line each, in two copied runs;
        // the second quote marker between them is not its own.
        #expect(
            block.runs == [
                Run(source: Extent(lead: 0, span: 5), decoded: 5),
                Run(source: Extent(lead: 2, span: 4), decoded: 4),
            ]
        )
        #expect(try document.scope(of: block, in: source) == [place((1, 3), (2, 0)), place((2, 3), (2, 6))])
        // The emphasis is at offset 2 of the content "a *b\nc* d", and its
        // source skips the marker too.
        #expect(emphasis.extent == Extent(lead: 0, span: 5))
        #expect(emphasis.runs.isEmpty)
        #expect(try document.scope(of: emphasis, in: source) == [place((1, 5), (2, 0)), place((2, 3), (2, 4))])
        #expect(try document.node(at: Position(line: 2, column: 1), in: source)?.isEqual(callout) == true)
        #expect(try document.node(at: Position(line: 2, column: 4), in: source)?.isEqual(emphasis) == true)
        // A subtree's dump places it in its root's content and draws it from
        // its own level.
        #expect(
            try document.dump(emphasis, in: source)
                == "Emphasis scope=1:5..2:0,2:3..2:4 anchor=null attributes={} children=3\n"
                + "├── Text scope=1:6..1:6 anchor=null attributes={} literal=\"b\" children=0\n"
                + "├── SoftBreak scope=1:7..2:0 anchor=null attributes={} children=0\n"
                + "└── Text scope=2:3..2:3 anchor=null attributes={} literal=\"c\" children=0\n"
        )
        // A continuation indent moves the paragraph's runs, never what its
        // content holds: the stripped space is its own source without content.
        let wider = try Document.parse("> a *b\n>  c* d\n")
        let moved = try #require((wider.content.first as? Callout)?.content.first as? Paragraph)
        let runs = [
            Run(source: Extent(lead: 0, span: 5), decoded: 5),
            Run(source: Extent(lead: 2, span: 1), decoded: 0),
            Run(source: Extent(lead: 0, span: 4), decoded: 4),
        ]
        #expect(moved.runs == runs)
        #expect(moved != block)
        #expect(moved.content[1].isEqual(emphasis))
    }

    @Test("a block whose runs read no content is no inline root, and its runs cut its range")
    func sourceWithoutContent() throws {
        let source = "> ```\n> x\n> ```\n"
        let document = try Document.parse(source)
        let callout = try #require(document.content.first as? Callout)
        let code = try #require(callout.content.first as? CodeBlock)
        func place(_ start: (Int32, Int32), _ end: (Int32, Int32)) -> Scope {
            Scope(start: Position(line: start.0, column: start.1), end: Position(line: end.0, column: end.1))
        }
        // The code block reads its lines without content, so the quote
        // markers between them are not its own.
        #expect(code.literal == "x\n")
        #expect(
            code.runs == [
                Run(source: Extent(lead: 0, span: 4), decoded: 0),
                Run(source: Extent(lead: 2, span: 2), decoded: 0),
                Run(source: Extent(lead: 2, span: 3), decoded: 0),
            ]
        )
        #expect(
            try document.scope(of: code, in: source)
                == [place((1, 3), (2, 0)), place((2, 3), (3, 0)), place((3, 3), (3, 5))]
        )
        #expect(try document.node(at: Position(line: 2, column: 1), in: source)?.isEqual(callout) == true)
        #expect(try document.node(at: Position(line: 2, column: 3), in: source)?.isEqual(code) == true)
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
