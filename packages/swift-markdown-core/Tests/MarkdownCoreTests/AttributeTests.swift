import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("universal attributes retain ordered values after native document release")
    func universalAttributes() throws {
        let parsed = try Document.parse(":n{#id .a class=\"a b}c\" k=1 k=2}")
        let paragraph = try #require(parsed.content.first as? Paragraph)
        let directive = try #require(paragraph.content.first as? Directive)
        #expect(directive.anchor == "id")
        #expect(directive.attributes.classes == ["a", "a", "b}c"])
        #expect(directive.attributes.records == [Record(name: "k", value: "1"), Record(name: "k", value: "2")])
        #expect(directive.dump().contains("attributes={.a .a .\"b}c\" k=\"1\" k=\"2\"}"))
        #expect(parsed.anchor == nil && parsed.attributes == .empty && parsed.metadata == nil)
    }

    @Test("metadata values preserve decimal text and stay outside Markup walking")
    func metadataValues() throws {
        let parsed = try Document.parse("body")
        let values: [MetadataValue] = [
            .scalar(.null), .scalar(.bool(true)), .scalar(.number("9007199254740993")),
            .scalar(.text("中文\nquoted")), .list([]), .list([.number("1.25"), .text("")]),
        ]
        let metadata = Metadata(
            records: values.map { MetadataRecord(name: "key", value: $0, scope: parsed.scope) },
            scope: parsed.scope
        )
        let document = Document(
            scope: parsed.scope,
            anchor: nil,
            attributes: .empty,
            content: parsed.content,
            metadata: metadata,
            footnotes: [],
            specimens: []
        )
        #expect(document.metadata?.records.map(\.value) == values)
        #expect(document.dump().contains("value=scalar(number(\"9007199254740993\"))"))
        #expect(document.dump().contains("value=list([])"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(!visitor.events.contains { $0.contains("Metadata") })
    }
}
