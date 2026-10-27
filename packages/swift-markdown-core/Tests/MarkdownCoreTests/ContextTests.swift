import MarkdownCore
import Testing

@Suite("context") struct ContextSuite {
    private func place(_ start: (Int32, Int32), _ end: (Int32, Int32)) -> Scope {
        Scope(start: Position(line: start.0, column: start.1), end: Position(line: end.0, column: end.1))
    }

    @Test("an open cross link's target is a dest slot, which spells a value with the closer after the caret")
    func openSlot() throws {
        let session = try MarkdownSession("[[No\n")
        try session.locate(Position(line: 1, column: 5))
        let context = session.context
        #expect(context.blocks.map(\.kind) == [NodeKind.paragraph])
        let slot = try #require(context.slots.first)
        #expect(context.slots.count == 1)
        #expect(slot.kind == .dest && slot.owner == .crossLink && !slot.closed && slot.prefix.isEmpty)
        #expect(slot.before == place((1, 3), (1, 4)))
        #expect(slot.after == place((1, 5), (1, 4)))
        let spelled = try #require(try session.spell("x", in: slot))
        #expect(spelled.text == "x]]" && spelled.cursor == 1)
        // A `|` would end the target, and the field holds it in no spelling.
        #expect(try session.spell("a|b", in: slot) == nil)
        #expect(try session.spell(["x", "a|b"], in: slot) == [spelled, nil])
    }

    @Test("a spelling's caret counts columns in the session's unit")
    func spellingUnits() throws {
        for (unit, cursor) in [(TextUnit.utf8, 6), (TextUnit.utf16, 3)] {
            let session = try MarkdownSession("[[No\n", unit: unit)
            try session.locate(Position(line: 1, column: 5))
            let slot = try #require(session.context.slots.first)
            #expect(try session.spell("\u{E9}\u{1F680}", in: slot)?.cursor == cursor)
        }
    }

    @Test("a closed construct's slot reads its field on past the cursor")
    func closedSlot() throws {
        let session = try MarkdownSession("[[Note]] after\n")
        try session.locate(Position(line: 1, column: 5))
        let slot = try #require(session.context.slots.first)
        #expect(slot.closed)
        #expect(slot.after == place((1, 5), (1, 6)))
        let spelled = try #require(try session.spell("x", in: slot))
        #expect(spelled.text == "x" && spelled.cursor == 1)
    }

    @Test("the context is the session's state: a locate replaces it, and an append clears it")
    func contextState() throws {
        let session = try MarkdownSession("[[No")
        #expect(session.context.blocks.isEmpty && session.context.slots.isEmpty)
        try session.locate(Position(line: 1, column: 5))
        let slot = try #require(session.context.slots.first)
        try session.locate(Position(line: 1, column: 1))
        #expect(session.context.slots.isEmpty)
        #expect(outOfBounds { try session.spell("x", in: slot) })
        try session.locate(Position(line: 1, column: 5))
        let next = try #require(session.context.slots.first)
        try session.append("te]]\n")
        #expect(session.context.slots.isEmpty)
        #expect(outOfBounds { try session.spell("x", in: next) })
        try session.locate(Position(line: 1, column: 5))
        let last = try #require(session.context.slots.first)
        #expect(last.closed)
        #expect(outOfBounds { try session.locate(Position(line: 9, column: 1)) })
        #expect(try session.spell("x", in: last)?.text == "x")
        // Another session at the same turn holds a slot at the same place.
        let other = try MarkdownSession("[[No")
        for _ in 0..<3 { try other.locate(Position(line: 1, column: 1)) }
        try other.append("te]]\n")
        try other.locate(Position(line: 1, column: 5))
        #expect(other.context.slots.count == 1)
        #expect(outOfBounds { try other.spell("x", in: last) })
    }

    @Test("a cursor outside the text, or below 1, is out of bounds and keeps the context")
    func cursorBounds() throws {
        let session = try MarkdownSession("a\n")
        try session.locate(Position(line: 1, column: 2))
        for position in [Position(line: 0, column: 1), Position(line: 1, column: 0), Position(line: 3, column: 1)] {
            #expect(outOfBounds { try session.locate(position) })
            #expect(session.context.blocks.map(\.kind) == [NodeKind.paragraph])
        }
        try session.locate(Position(line: 2, column: 1))
        #expect(session.context.slots.isEmpty)
    }

    @Test("the path holds every node over a byte, root first, and ends at the node there")
    func path() throws {
        let source = "> a *b*\n"
        let session = try MarkdownSession(source)
        let document = try session.finalize()
        for column in Int32(1)...8 {
            let position = Position(line: 1, column: column)
            let path = try session.path(at: position)
            let node = try document.node(at: position, in: source)
            #expect(path.last?.id == node?.id)
            #expect(path.isEmpty || path.first?.id == document.id)
        }
        #expect(try session.path(at: Position(line: 1, column: 6)).count == 5)
        #expect(try session.path(at: Position(line: 2, column: 1)).isEmpty)
    }

    @Test("a link's fields and markers divide its own source")
    func fields() throws {
        let session = try MarkdownSession("[a](doc.md#part)\n")
        let document = try session.finalize()
        let link = try #require((document.content.first as? Paragraph)?.content.first as? Link)
        #expect(try session.scope(of: link, field: .dest) == [place((1, 5), (1, 10))])
        #expect(try session.scope(of: link, field: .anchor) == [place((1, 12), (1, 15))])
        #expect(try session.scope(of: link, field: .label).isEmpty)
        let markers = try session.markers(of: link)
        #expect(markers.first == place((1, 1), (1, 1)) && markers.last == place((1, 16), (1, 16)))
        // The text the link holds is its child's, not the link's own.
        #expect(!markers.contains(place((1, 2), (1, 2))))
    }

    @Test("a node the session's document does not hold is out of bounds")
    func foreignNode() throws {
        let other = try Document.parse("[a](doc.md#part)\n")
        let link = try #require((other.content.first as? Paragraph)?.content.first as? Link)
        let session = try MarkdownSession("x\n\n[a](other.md#part)\n")
        #expect(outOfBounds { try session.scope(of: link, field: .dest) })
        #expect(outOfBounds { try session.markers(of: link) })
    }

    @Test("an append keeps answering for the nodes it leaves unchanged, and not for those it changes")
    func nodesAcrossAppends() throws {
        let session = try MarkdownSession("[a](doc.md)\n\nb")
        let before = session.document
        let link = try #require((before.content.first as? Paragraph)?.content.first as? Link)
        let text = try #require((before.content.last as? Paragraph)?.content.first as? Text)
        try session.append("c\n")
        #expect(try session.scope(of: link, field: .dest) == [place((1, 5), (1, 10))])
        #expect(outOfBounds { try session.markers(of: text) })
    }

    @Test("a session's snapshot sees no definition after the asking root")
    func snapshot() throws {
        let session = try MarkdownSession("[a]\n\n")
        let document = try session.append("[a]: /url\n")
        let text = try #require((document.content.first as? Paragraph)?.content.first as? Text)
        #expect(try session.path(at: Position(line: 1, column: 1)).last?.id == text.id)
        #expect(try session.markers(of: text).isEmpty)
        #expect(try session.scope(of: text, field: .label).isEmpty)
        try session.locate(Position(line: 1, column: 2))
        #expect(session.context.blocks.map(\.kind) == [.paragraph])
        let final = try session.finalize()
        let link = try #require((final.content.first as? Paragraph)?.content.first as? Link)
        #expect(try !session.markers(of: link).isEmpty)
    }

    @Test("the lexicons are the dialect's")
    func lexicons() {
        #expect(
            Metadata.fields == [
                "name", "title", "subtitle", "time", "date", "authors", "keywords", "abstract", "state", "comment",
            ]
        )
        #expect(Slot.cues.contains("[") && Slot.cues.contains("\n") && !Slot.cues.contains(">"))
        #expect(zip(Slot.cues, Slot.cues.dropFirst()).allSatisfy { $0 < $1 })
        #expect(NodeKind.crossLink.description == "CrossLink" && Slot.Kind.`class`.description == "class")
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
