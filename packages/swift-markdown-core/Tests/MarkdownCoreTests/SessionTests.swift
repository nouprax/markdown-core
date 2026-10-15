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
}
