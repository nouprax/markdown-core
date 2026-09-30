import MarkdownCore
import Testing

extension APISuite {
    @Test("spans retain typed content and walk both phases after native release")
    func spans() throws {
        let source = "[a *b*]{}"
        let document = try Document.parse(source)
        let paragraph = try #require(document.content.first as? Paragraph)
        let span = try #require(paragraph.content.first as? Span)
        var visitor = RecordingWalkingVisitor()
        span.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Span", "enter:Text", "exit:Text", "enter:Emphasis",
                "enter:Text", "exit:Text", "exit:Emphasis", "exit:Span",
            ]
        )
        #expect(span.content.count == 2)
        #expect(((span.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(
            scope(of: span, in: document, source: source)
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9))
        )
    }
    @Test("superscripts retain typed content and walk both phases after native release")
    func superscripts() throws {
        let source = "^a*b*^"
        let document = try Document.parse(source)
        let paragraph = try #require(document.content.first as? Paragraph)
        let superscript = try #require(paragraph.content.first as? Superscript)
        var visitor = RecordingWalkingVisitor()
        superscript.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Superscript", "enter:Text", "exit:Text", "enter:Emphasis",
                "enter:Text", "exit:Text", "exit:Emphasis", "exit:Superscript",
            ]
        )
        #expect(superscript.content.count == 2)
        #expect(((superscript.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(
            scope(of: superscript, in: document, source: source)
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 6))
        )
    }
    @Test("subscripts retain typed content and walk both phases after native release")
    func subscripts() throws {
        let source = "~a*b*~"
        let document = try Document.parse(source)
        let paragraph = try #require(document.content.first as? Paragraph)
        let script = try #require(paragraph.content.first as? Subscript)
        var visitor = RecordingWalkingVisitor()
        script.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Subscript", "enter:Text", "exit:Text", "enter:Emphasis",
                "enter:Text", "exit:Text", "exit:Emphasis", "exit:Subscript",
            ]
        )
        #expect(script.content.count == 2)
        #expect(((script.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(
            scope(of: script, in: document, source: source)
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 6))
        )
    }
}
