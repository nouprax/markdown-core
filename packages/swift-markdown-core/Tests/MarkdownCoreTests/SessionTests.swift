import MarkdownCore
import Testing

@Suite("session") struct SessionSuite {
    @Test("each document equals a fresh parse of the session's text", arguments: [TextUnit.utf8, .utf16])
    func freshParse(unit: TextUnit) throws {
        let source = "# Title\n\nfirst *one*\n\n- item\n"
        let session = try MarkdownSession(source, unit: unit)
        #expect(session.unit == unit)
        #expect(session.text == source)
        // Before any edit the ids are a parse's own, so the values are equal.
        #expect(try session.document == Document.parse(source, unit: unit))
        let steps: [(MarkdownSession) throws -> Document] = [
            { try $0.edit([TextEdit(9..<14, with: "second")]) },
            { try $0.append("\n> quote\n") },
            { try $0.edit([TextEdit(0..<2, with: "")]) },
            { try $0.edit([TextEdit(0..<0, with: "text\n")]) },
        ]
        for step in steps {
            let returned = try step(session)
            #expect(returned == session.document)
            #expect(returned.unit == unit)
            let text = session.text
            #expect(try returned.dump(in: text) == Document.parse(text, unit: unit).dump(in: text))
        }
        #expect(session.text == "text\nTitle\n\nsecond *one*\n\n- item\n\n> quote\n")
    }

    @Test("an unchanged node keeps its value and id, and an edited paragraph continues its id")
    func identity() throws {
        let session = try MarkdownSession("first\n\nsecond paragraph\n")
        let before = session.document
        let first = try #require(before.content[0] as? Paragraph)
        let second = try #require(before.content[1] as? Paragraph)
        // "second" is units 7..<13; the paragraph's " paragraph" survives the edit.
        let after = try session.edit([TextEdit(7..<13, with: "changed")])
        #expect(session.text == "first\n\nchanged paragraph\n")
        let unchanged = try #require(after.content[0] as? Paragraph)
        #expect(unchanged == first)
        let edited = try #require(after.content[1] as? Paragraph)
        #expect(edited.id == second.id)
        #expect(edited != second)
        #expect((edited.content.first as? Text)?.literal == "changed paragraph")
    }

    @Test("a batch is in coordinates of the text before it, listed in any order")
    func batch() throws {
        let session = try MarkdownSession("one\n\ntwo\n\nthree\n")
        try session.edit([TextEdit(10..<15, with: "THREE"), TextEdit(0..<3, with: "ONE")])
        #expect(session.text == "ONE\n\ntwo\n\nTHREE\n")
        // Two edits at one offset apply in the order listed.
        try session.edit([TextEdit(5..<5, with: "b"), TextEdit(5..<5, with: "a")])
        #expect(session.text == "ONE\n\nbatwo\n\nTHREE\n")
        let text = session.text
        #expect(try session.document.dump(in: text) == Document.parse(text).dump(in: text))
    }

    @Test("appending streams a text chunk by chunk", arguments: [TextUnit.utf8, .utf16])
    func append(unit: TextUnit) throws {
        let chunks = ["# He", "ading 🚀\n", "\npara", "graph\n", "- a\n- b"]
        let session = try MarkdownSession(unit: unit)
        #expect(session.text.isEmpty)
        for chunk in chunks {
            let document = try session.append(chunk)
            let text = session.text
            #expect(try document.dump(in: text) == Document.parse(text, unit: unit).dump(in: text))
        }
        #expect(session.text == chunks.joined())
    }

    @Test("offsets count in the session's unit after an emoji")
    func offsetsAfterEmoji() throws {
        // "🚀" is two UTF-16 units and four UTF-8 bytes. Read as bytes, the
        // UTF-16 offset 2 would fall inside the emoji.
        let cases: [(TextUnit, Range<Int>)] = [(.utf16, 2..<3), (.utf8, 4..<5)]
        for (unit, bounds) in cases {
            let session = try MarkdownSession("🚀a\n", unit: unit)
            try session.edit([TextEdit(bounds, with: "b")])
            #expect(session.text == "🚀b\n", "\(unit)")
        }
    }

    @Test("string indices carry no unit and count against the current text", arguments: [TextUnit.utf8, .utf16])
    func indices(unit: TextUnit) throws {
        let session = try MarkdownSession("🚀 é text\n", unit: unit)
        let text = session.text
        let start = try #require(text.firstIndex(of: "t"))
        let end = text.index(start, offsetBy: 4)
        try session.edit([TextEdit(start..<end, with: "word")])
        #expect(session.text == "🚀 é word\n")
        let current = session.text
        #expect(try session.document.dump(in: current) == Document.parse(current, unit: unit).dump(in: current))
    }

    @Test("out-of-bounds ranges and a split surrogate pair carry their own codes")
    func refusedRanges() throws {
        let emoji = try MarkdownSession("🚀\n")
        let letters = try MarkdownSession("abc\n")
        let rejected: [ErrorCode: [(MarkdownSession, [TextEdit])]] = [
            .outOfBounds: [
                // A negative start reaches the engine as a start after its end.
                (emoji, [TextEdit(-1..<0, with: "x")]),
                (emoji, [TextEdit(3..<4, with: "x")]),
                (letters, [TextEdit(0..<2, with: "x"), TextEdit(1..<3, with: "y")]),
            ],
            .insideScalar: [
                (emoji, [TextEdit(1..<1, with: "x")]),
                (emoji, [TextEdit(0..<1, with: "x")]),
            ],
        ]
        for (code, cases) in rejected {
            for (session, edits) in cases {
                let error = #expect(throws: MarkdownCoreError.self) { try session.edit(edits) }
                #expect(error?.code == code, "\(edits)")
            }
        }
    }
}
