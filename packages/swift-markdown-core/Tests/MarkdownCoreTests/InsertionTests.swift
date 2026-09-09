import MarkdownCore
import Testing

extension APISuite {
    @Test("insertions retain typed content and walk both phases after native release")
    func insertions() throws {
        let paragraph = try #require(Document.parse("++a *b*++").content.first as? Paragraph)
        let insertion = try #require(paragraph.content.first as? Insertion)
        var visitor = RecordingWalkingVisitor()
        insertion.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Insertion", "entering:Text", "exiting:Text", "entering:Emphasis",
                "entering:Text", "exiting:Text", "exiting:Emphasis", "exiting:Insertion",
            ]
        )
        #expect(insertion.content.count == 2)
        #expect(((insertion.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(insertion.scope == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9)))
    }
}
