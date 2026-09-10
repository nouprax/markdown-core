import MarkdownCore
import Testing

extension APISuite {
    @Test("spans retain typed content and walk both phases after native release")
    func spans() throws {
        let paragraph = try #require(Document.parse("[a *b*]{}").content.first as? Paragraph)
        let span = try #require(paragraph.content.first as? Span)
        var visitor = RecordingWalkingVisitor()
        span.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Span", "entering:Text", "exiting:Text", "entering:Emphasis",
                "entering:Text", "exiting:Text", "exiting:Emphasis", "exiting:Span",
            ]
        )
        #expect(span.content.count == 2)
        #expect(((span.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(span.scope == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9)))
    }
    @Test("superscripts retain typed content and walk both phases after native release")
    func superscripts() throws {
        let paragraph = try #require(Document.parse("^a*b*^").content.first as? Paragraph)
        let superscript = try #require(paragraph.content.first as? Superscript)
        var visitor = RecordingWalkingVisitor()
        superscript.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Superscript", "entering:Text", "exiting:Text", "entering:Emphasis",
                "entering:Text", "exiting:Text", "exiting:Emphasis", "exiting:Superscript",
            ]
        )
        #expect(superscript.content.count == 2)
        #expect(((superscript.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(superscript.scope == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 6)))
    }
    @Test("subscripts retain typed content and walk both phases after native release")
    func subscripts() throws {
        let paragraph = try #require(Document.parse("~a*b*~").content.first as? Paragraph)
        let script = try #require(paragraph.content.first as? Subscript)
        var visitor = RecordingWalkingVisitor()
        script.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Subscript", "entering:Text", "exiting:Text", "entering:Emphasis",
                "entering:Text", "exiting:Text", "exiting:Emphasis", "exiting:Subscript",
            ]
        )
        #expect(script.content.count == 2)
        #expect(((script.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(script.scope == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 6)))
    }
}
