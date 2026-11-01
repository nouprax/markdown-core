import MarkdownCore
import Testing

@Suite("session") struct SessionSuite {
    @Test("each document equals a fresh parse of the session's text", arguments: [TextUnit.utf8, .utf16])
    func freshParse(unit: TextUnit) throws {
        let source = "# Title\n\nfirst *one*\n\n- item\n"
        let session = try MarkdownSession(source, unit: unit)
        #expect(session.unit == unit)
        #expect(session.text == source)
        // Before any append the ids are a parse's own, so the values are equal.
        #expect(try session.document == Document.parse(source, unit: unit))
        for chunk in ["- sec", "ond *item*\n", "\n> quote\n", "text\n"] {
            let returned = try session.append(chunk)
            #expect(returned == session.document)
            #expect(returned.unit == unit)
            let text = session.text
            #expect(try returned.dump(in: text) == Document.parse(text, unit: unit).dump(in: text))
        }
        #expect(session.text == "# Title\n\nfirst *one*\n\n- item\n- second *item*\n\n> quote\ntext\n")
    }

    @Test("an unchanged node keeps its value and id, and an extended paragraph continues its id")
    func identity() throws {
        let session = try MarkdownSession("first\n\nsecond")
        let before = session.document
        let first = try #require(before.content[0] as? Paragraph)
        let second = try #require(before.content[1] as? Paragraph)
        let after = try session.append(" paragraph\n")
        #expect(session.text == "first\n\nsecond paragraph\n")
        let unchanged = try #require(after.content[0] as? Paragraph)
        #expect(unchanged == first)
        let extended = try #require(after.content[1] as? Paragraph)
        #expect(extended.id == second.id)
        #expect(extended != second)
        #expect((extended.content.first as? Text)?.literal == "second paragraph")
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

    @Test("flush returns the final document and ends the session", arguments: [TextUnit.utf8, .utf16])
    func flush(unit: TextUnit) throws {
        let session = try MarkdownSession("[a]\n\n", unit: unit)
        let snapshot = try session.append("[a]: /url\n")
        // A snapshot's lookup sees only the definitions written before it.
        #expect(try snapshot.dump(in: session.text) != Document.parse(session.text, unit: unit).dump(in: session.text))
        let final = try session.flush()
        let text = session.text
        #expect(final == session.document)
        #expect(try final.dump(in: text) == Document.parse(text, unit: unit).dump(in: text))
        #expect(flushed { try session.append("more") })
        #expect(flushed { try session.flush() })
        #expect(session.text == text)
        #expect(session.document == final)
    }
}

/// The text every edit sweep starts from: ASCII, so a column is a byte.
private let sweepSource = "# Title [a]\n\nfirst *one* `code`\n\n- item\n- second\n\n> quote\n\n[a]: /url\n"
/// What each sweep writes at a cursor, and so removes again.
private let sweepWrites = ["*", "\n", "- ", "#", "`", "> ", "[a]", "x"]

/// Every cursor of an ASCII `text`: each line's columns, its end included.
private func cursors(of text: String) -> [Position] {
    text.split(separator: "\n", omittingEmptySubsequences: false).enumerated().flatMap { line, content in
        (1...content.utf8.count + 1).map { Position(line: Int32(line + 1), column: Int32($0)) }
    }
}

/// The cursor after ASCII `written` is written at `cursor`.
private func cursor(after written: String, at cursor: Position) -> Position {
    let lines = written.split(separator: "\n", omittingEmptySubsequences: false)
    guard let last = lines.last, lines.count > 1 else {
        return Position(line: cursor.line, column: cursor.column + Int32(written.utf8.count))
    }
    return Position(line: cursor.line + Int32(lines.count - 1), column: Int32(last.utf8.count + 1))
}

/// Whether `document` dumps as a fresh parse of `text` in `unit` does.
private func parses(_ document: Document, _ text: String, _ unit: TextUnit) throws -> Bool {
    try document.dump(in: text) == Document.parse(text, unit: unit).dump(in: text)
}

@Suite("session edits") struct SessionEditSuite {
    @Test("an edit at every cursor, and its undoing, parse as the new text", arguments: [TextUnit.utf8, .utf16])
    func sweep(unit: TextUnit) throws {
        let session = try MarkdownSession(sweepSource, unit: unit)
        try session.flush()
        for (index, place) in cursors(of: sweepSource).enumerated() {
            let written = sweepWrites[index % sweepWrites.count]
            let document = try session.edit(from: place, to: place, with: written)
            #expect(document == session.document)
            #expect(try parses(document, session.text, unit), "\(written) at \(place)")
            try session.edit(from: place, to: cursor(after: written, at: place), with: "")
            #expect(session.text == sweepSource)
            #expect(try parses(session.document, sweepSource, unit), "\(written) undone at \(place)")
        }
    }

    @Test("an edit's cursors count columns in the session's unit", arguments: [TextUnit.utf8, .utf16])
    func units(unit: TextUnit) throws {
        let session = try MarkdownSession("🚀 one\n\ntwo\n", unit: unit)
        try session.flush()
        // 🚀 is four UTF-8 bytes and two UTF-16 units.
        let after = Int32(unit == .utf8 ? 5 : 3)
        let document = try session.edit(
            from: Position(line: 1, column: after),
            to: Position(line: 1, column: after + 4),
            with: "*won*"
        )
        #expect(session.text == "🚀*won*\n\ntwo\n")
        #expect(try parses(document, session.text, unit))
    }

    @Test("a node the edit did not reach is the old node, and one it read again keeps its id")
    func nodes() throws {
        let session = try MarkdownSession("# Title\n\nfirst\n\nsecond\n")
        let before = try session.flush()
        let end = Position(line: 5, column: 7)
        let after = try session.edit(from: end, to: end, with: " one")
        #expect(session.text == "# Title\n\nfirst\n\nsecond one\n")
        let title = try #require(before.content[0] as? Heading)
        let first = try #require(before.content[1] as? Paragraph)
        #expect(after.content[0] as? Heading == title)
        #expect(after.content[1] as? Paragraph == first)
        let old = try #require(before.content[2] as? Paragraph)
        let new = try #require(after.content[2] as? Paragraph)
        #expect(new.id == old.id)
        #expect(new != old)
    }

    @Test("an edit before flush, out of bounds or backwards throws and changes nothing")
    func refusals() throws {
        let session = try MarkdownSession("- text\n")
        let start = Position(line: 1, column: 1)
        let inside = Position(line: 1, column: 4)
        let nowhere = Position(line: 9, column: 1)
        #expect(code { try session.edit(from: start, to: start, with: "x") } == .sessionStreaming)
        try session.append("more\n")
        let final = try session.flush()
        try session.locate(inside)
        let open = session.context.blocks
        #expect(!open.isEmpty)
        #expect(code { try session.edit(from: inside, to: start, with: "") } == .outOfBounds)
        #expect(code { try session.edit(from: nowhere, to: nowhere, with: "") } == .outOfBounds)
        #expect(code { try session.edit(from: Position(line: 0, column: 1), to: start, with: "") } == .outOfBounds)
        #expect(session.text == "- text\nmore\n")
        #expect(session.document == final)
        #expect(session.context.blocks == open)
        // An edit that runs empties the context, since it changes the text.
        try session.edit(from: start, to: start, with: "a ")
        #expect(session.context.blocks.isEmpty)
    }
}

/// The code of the library's error `call` throws, or `nil` when it returns.
private func code(_ call: () throws -> Document) -> ErrorCode? {
    do {
        _ = try call()
        return nil
    } catch {
        return (error as? MarkdownCoreError)?.code
    }
}

/// Whether `call` throws the library's error with ``ErrorCode/sessionFlushed``.
private func flushed(_ call: () throws -> Document) -> Bool {
    code(call) == .sessionFlushed
}
