import MarkdownCore
import Testing

@Suite("ast") struct CalloutSuite {
    @Test("authored callout title walks before body after native release")
    func calloutMetadata() throws {
        let callout = try #require(Document.parse("> [!CuStOm]- **T**\n> body\n").content.first as? Callout)
        #expect(callout.variant == "CuStOm")
        #expect(callout.collapsed == true)
        let title = try #require(callout.title)
        #expect(title.map { String(describing: type(of: $0)) } == ["Strong"])
        #expect(callout.content.map { String(describing: type(of: $0)) } == ["Paragraph"])
        let strong = try #require(title.first as? Strong)
        #expect((strong.content.first as? Text)?.literal == "T")
        var visitor = RecordingWalkingVisitor()
        callout.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Callout", "entering:Strong", "entering:Text", "exiting:Text", "exiting:Strong",
                "entering:Paragraph", "entering:Text", "exiting:Text", "exiting:Paragraph", "exiting:Callout",
            ]
        )
        let values = try Document.parse("> [!note]+ %%t%%\n\n> [!note]\n").content
        let comment = try #require(values[0] as? Callout)
        #expect(comment.collapsed == false)
        #expect((comment.title?.first as? MarkdownCore.Comment)?.literal == "t")
        #expect(comment.content.isEmpty)
        let empty = try #require(values[1] as? Callout)
        #expect(empty.title == nil)
        #expect(empty.collapsed == nil)
    }

}
