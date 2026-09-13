import MarkdownCore
import Testing

extension APISuite {
    @Test("inline footnotes keep direct content, source ids and finite visitation")
    func inlineFootnotes() throws {
        let document = try Document.parse("^[^[x]]\n\n[^inline-1]: authored\n")
        #expect(document.footnotes.map(\.id) == ["inline-1-1", "inline-2", "inline-1"])
        let outer = try #require((document.content.first as? Paragraph)?.content.first as? Cite)
        #expect(outer.citations.first?.referent == .footnote(id: "inline-1-1"))
        let inner = try #require(document.footnotes[0].content.first as? Cite)
        #expect(inner.citations.first?.referent == .footnote(id: "inline-2"))
        #expect((document.footnotes[1].content.first as? Text)?.literal == "x")
        #expect(document.footnotes[2].content.first is Paragraph)
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Document", "enter:Paragraph", "enter:Cite", "enter:Citation", "exit:Citation",
                "exit:Cite", "exit:Paragraph", "enter:Footnote", "enter:Cite", "enter:Citation",
                "exit:Citation", "exit:Cite", "exit:Footnote", "enter:Footnote", "enter:Text",
                "exit:Text", "exit:Footnote", "enter:Footnote", "enter:Paragraph", "enter:Text",
                "exit:Text", "exit:Paragraph", "exit:Footnote", "exit:Document",
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
        #expect((nodes[1] as? Citation)?.referent == .footnote(id: "label"))
        #expect((nodes[2] as? Footnote)?.id == "label")
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
