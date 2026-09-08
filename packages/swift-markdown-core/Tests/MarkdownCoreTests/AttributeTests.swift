import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("Properties keep recognized fields and literal prose after native release")
    func propertiesContent() throws {
        let source =
            "---\r\nname: 9007199254740993\r\nnot YAML\r\n...\r\nunknown: ignored\r\n"
            + "comment: *x\r\nname: duplicate\r\nabstract: |\r\n  first\r\n\r\n  second\r\n"
            + "comment: |\r\n  # prose\r\n---\r\nbody\r\n"
        let document = try Document.parse(source)
        let metadata = try #require(document.metadata)
        #expect(
            [metadata.name, metadata.abstract, metadata.comment] == [
                .scalar(.number("9007199254740993")), .scalar(.text("first\n\nsecond\n")), .scalar(.text("# prose\n")),
            ]
        )
        #expect(metadata.scope.end.line == 14)
        #expect(document.content[0].scope.start.line == 15)
        let empty = try #require(Document.parse("---\nunknown: 1\nfree text\n---").metadata)
        #expect(empty == Metadata(scope: empty.scope))
        #expect(try Document.parse("---\nname: 1\n").metadata == nil)
    }

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
            name: values[0],
            title: values[1],
            subtitle: values[2],
            time: values[3],
            date: values[4],
            authors: values[5],
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
        #expect(
            [
                document.metadata?.name, document.metadata?.title, document.metadata?.subtitle,
                document.metadata?.time, document.metadata?.date, document.metadata?.authors,
            ]
                == values
        )
        #expect(document.dump().contains("subtitle=scalar(number(\"9007199254740993\"))"))
        #expect(document.dump().contains("date=list([])"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(!visitor.events.contains { $0.contains("Metadata") })
    }
}
