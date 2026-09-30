import MarkdownCore
import Testing

extension APISuite {
    @Test("inline notes are owned by their referents and listed with the definitions")
    func inlineFootnotes() throws {
        let document = try Document.parse("^[^[x]]\n\n[^inline-1]: authored\n")
        #expect(document.footnotes.map(\.label) == [nil, nil, "inline-1"])
        let outer = try #require((document.content.first as? Paragraph)?.content.first as? Cite)
        guard case .footnote(.note(let note)) = outer.citations.first?.referent else {
            Issue.record("an inline note is the note branch")
            return
        }
        #expect(note == document.footnotes[0])
        let inner = try #require(note.content.first as? Cite)
        guard case .footnote(.note(let nested)) = inner.citations.first?.referent else {
            Issue.record("a nested inline note is the note branch")
            return
        }
        #expect(nested == document.footnotes[1])
        #expect((nested.content.first as? Text)?.literal == "x")
        #expect(document.footnotes[2].content.first is Paragraph)
        #expect(document.footnote(for: "inline-1") == document.footnotes[2])
        #expect(document.content.count == 2)
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Document", "enter:Paragraph", "enter:Cite", "enter:Citation", "enter:Footnote",
                "enter:Cite", "enter:Citation", "enter:Footnote", "enter:Text", "exit:Text", "exit:Footnote",
                "exit:Citation", "exit:Cite", "exit:Footnote", "exit:Citation", "exit:Cite", "exit:Paragraph",
                "enter:Footnote", "enter:Paragraph", "enter:Text", "exit:Text", "exit:Paragraph",
                "exit:Footnote", "exit:Document",
            ]
        )
    }
}

extension APISuite {
    @Test("owned scoped elements dispatch as Markup after the document is released")
    func ownedMarkupNodes() throws {
        let nodes: [any Markup] = try {
            let document = try Document.parse(
                "---\ntitle: Example\n---\n[^Label]\n\n[^label]: self [^LABEL]\n\n(@sample) Body\n"
            )
            let citation = try #require((document.content.first as? Paragraph)?.content.first as? Cite).citations[0]
            return [try #require(document.metadata), citation, document.footnotes[0], document.specimens[0]]
        }()
        for node in nodes {
            #expect(node.anchor == nil && node.attributes.classes.isEmpty && node.attributes.records.isEmpty)
            var walker = RecordingWalkingVisitor()
            node.walk(with: &walker)
            #expect(walker.entered == walker.exited && walker.entered > 0)
            #expect(walker.events.first == "enter:\(kindName(node))")
            #expect(walker.events.last == "exit:\(kindName(node))")
        }
        #expect((nodes[1] as? Citation)?.referent == .footnote(target: .label(value: "label")))
        #expect((nodes[2] as? Footnote)?.label == "label")
    }
}

extension APISuite {
    @Test("walker controls callback phases for document and leaf roots")
    func unifiedVisitor() throws {
        let document = try Document.parse("text")
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Document", "enter:Paragraph", "enter:Text",
                "exit:Text", "exit:Paragraph", "exit:Document",
            ]
        )
        let text = try #require((document.content.first as? Paragraph)?.content.first)
        visitor = RecordingWalkingVisitor()
        text.walk(with: &visitor)
        #expect(visitor.events == ["enter:Text", "exit:Text"])
    }
}

extension APISuite {
    @Test("the definition tables list every footnote and specimen in source order and find a label's first")
    func definitionTables() throws {
        let source = "(@a) one\n\n(@) two\n\n(@a) three\n\nSee (@a) and [^n] and ^[inline].\n\n[^n]: note\n"
        let document = try Document.parse(source)
        #expect(document.specimens.map(\.label) == ["a", nil, "a"])
        #expect(document.specimens.map(\.id) == document.content.prefix(3).map(\.id))
        let first = try #require(document.specimen(for: "a"))
        #expect(first == document.specimens[0])
        #expect(((first.content.first as? Paragraph)?.content.first as? Text)?.literal == "one")
        #expect(document.specimen(for: "b") == nil)

        let paragraph = try #require(document.content[3] as? Paragraph)
        let referents = paragraph.content.compactMap { ($0 as? Cite)?.citations.first?.referent }
        #expect(referents.count == 3)
        #expect(referents[0] == .specimen(label: "a"))
        #expect(referents[1] == .footnote(target: .label(value: "n")))
        // An inline note is listed where its call is, and has no label to find.
        #expect(referents[2] == .footnote(target: .note(footnote: document.footnotes[0])))
        #expect(document.footnotes.map(\.label) == [nil, "n"])
        #expect(document.footnote(for: "n") == document.footnotes[1])
        #expect(document.footnote(for: "n")?.id == document.content[4].id)
        #expect(document.footnote(for: "") == nil)
    }
}
