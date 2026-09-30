import MarkdownCore
import Testing

extension APISuite {
    @Test("insertions retain typed content and walk both phases after native release")
    func insertions() throws {
        let source = "++a *b*++"
        let document = try Document.parse(source)
        let paragraph = try #require(document.content.first as? Paragraph)
        let insertion = try #require(paragraph.content.first as? Insertion)
        var visitor = RecordingWalkingVisitor()
        insertion.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Insertion", "enter:Text", "exit:Text", "enter:Emphasis",
                "enter:Text", "exit:Text", "exit:Emphasis", "exit:Insertion",
            ]
        )
        #expect(insertion.content.count == 2)
        #expect(((insertion.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(
            scope(of: insertion, in: document, source: source)
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9))
        )
    }
}
