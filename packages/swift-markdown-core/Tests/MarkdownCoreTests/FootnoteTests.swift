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
                "entering:Document", "entering:Paragraph", "entering:Cite", "entering:Citation", "exiting:Citation",
                "exiting:Cite", "exiting:Paragraph", "entering:Footnote", "entering:Cite", "entering:Citation",
                "exiting:Citation", "exiting:Cite", "exiting:Footnote", "entering:Footnote", "entering:Text",
                "exiting:Text", "exiting:Footnote", "entering:Footnote", "entering:Paragraph", "entering:Text",
                "exiting:Text", "exiting:Paragraph", "exiting:Footnote", "exiting:Document",
            ]
        )
    }
}
