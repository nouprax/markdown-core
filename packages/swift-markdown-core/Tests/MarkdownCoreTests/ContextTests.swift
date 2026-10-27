import MarkdownCore
import Testing

@Suite("context") struct ContextSuite {
    private func place(_ start: (Int32, Int32), _ end: (Int32, Int32)) -> Scope {
        Scope(start: Position(line: start.0, column: start.1), end: Position(line: end.0, column: end.1))
    }

    @Test("an open cross link's target is a dest slot, which spells a value with the closer after the caret")
    func openSlot() throws {
        let source = "[[No\n"
        let document = try Document.parse(source)
        let context = try document.context(at: Position(line: 1, column: 5), in: source)
        #expect(context.blocks.map(\.kind) == [NodeKind.paragraph])
        let slot = try #require(context.slots.first)
        #expect(context.slots.count == 1)
        #expect(slot.kind == .dest && slot.owner == .crossLink && !slot.closed && slot.prefix.isEmpty)
        #expect(slot.before == place((1, 3), (1, 4)))
        #expect(slot.after == place((1, 5), (1, 4)))
        let spelled = try #require(try slot.spell("x"))
        #expect(spelled.text == "x]]" && spelled.cursor == 1)
        // A `|` would end the target, and the field holds it in no spelling.
        #expect(try slot.spell("a|b") == nil)
        #expect(try slot.spell(["x", "a|b"]) == [spelled, nil])
    }

    @Test("a spelling's caret counts columns in the document's unit")
    func spellingUnits() throws {
        let source = "[[No\n"
        for (unit, cursor) in [(TextUnit.utf8, 6), (TextUnit.utf16, 3)] {
            let document = try Document.parse(source, unit: unit)
            let slot = try #require(try document.context(at: Position(line: 1, column: 5), in: source).slots.first)
            #expect(try slot.spell("\u{E9}\u{1F680}")?.cursor == cursor)
        }
    }

    @Test("a closed construct's slot reads its field on past the cursor")
    func closedSlot() throws {
        let source = "[[Note]] after\n"
        let document = try Document.parse(source)
        let slot = try #require(try document.context(at: Position(line: 1, column: 5), in: source).slots.first)
        #expect(slot.closed)
        #expect(slot.after == place((1, 5), (1, 6)))
        let spelled = try #require(try slot.spell("x"))
        #expect(spelled.text == "x" && spelled.cursor == 1)
    }

    @Test("a cursor outside the source, or below 1, is out of bounds")
    func cursorBounds() throws {
        let source = "a\n"
        let document = try Document.parse(source)
        for position in [Position(line: 0, column: 1), Position(line: 1, column: 0), Position(line: 3, column: 1)] {
            #expect(outOfBounds { try document.context(at: position, in: source) })
        }
        #expect(try document.context(at: Position(line: 2, column: 1), in: source).slots.isEmpty)
    }

    @Test("the path holds every node over a byte, root first, and ends at the node there")
    func path() throws {
        let source = "> a *b*\n"
        let document = try Document.parse(source)
        for column in Int32(1)...8 {
            let position = Position(line: 1, column: column)
            let path = try document.path(at: position, in: source)
            let node = try document.node(at: position, in: source)
            #expect(path.last?.id == node?.id)
            #expect(path.isEmpty || path.first?.id == document.id)
        }
        #expect(try document.path(at: Position(line: 1, column: 6), in: source).count == 5)
        #expect(try document.path(at: Position(line: 2, column: 1), in: source).isEmpty)
    }

    @Test("a link's fields and markers divide its own source")
    func fields() throws {
        let source = "[a](doc.md#part)\n"
        let document = try Document.parse(source)
        let link = try #require((document.content.first as? Paragraph)?.content.first as? Link)
        #expect(try document.scope(of: link, field: .dest, in: source) == [place((1, 5), (1, 10))])
        #expect(try document.scope(of: link, field: .anchor, in: source) == [place((1, 12), (1, 15))])
        #expect(try document.scope(of: link, field: .label, in: source).isEmpty)
        let markers = try document.markers(of: link, in: source)
        #expect(markers.first == place((1, 1), (1, 1)) && markers.last == place((1, 16), (1, 16)))
        // The text the link holds is its child's, not the link's own.
        #expect(!markers.contains(place((1, 2), (1, 2))))
    }

    @Test("a field query against another source is out of bounds")
    func fieldsOfAnotherSource() throws {
        let document = try Document.parse("[a](doc.md#part)\n")
        let link = try #require((document.content.first as? Paragraph)?.content.first as? Link)
        #expect(outOfBounds { try document.scope(of: link, field: .dest, in: "[a](other.md#part)\n") })
        #expect(outOfBounds { try document.markers(of: link, in: "[a](do") })
    }

    @Test("the vocabularies are the dialect's")
    func vocabularies() {
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
