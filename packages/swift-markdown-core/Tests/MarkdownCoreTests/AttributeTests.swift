import Testing

@testable import MarkdownCore

extension APISuite {
    @Test("Properties preserve mixed data and source while recovering subsequent records")
    func propertiesContent() throws {
        let source =
            "---\r\na: &x 9007199254740993\r\nnot YAML\r\n...\r\nbad: &x [true]\r\n"
            + "b: *x\r\na: duplicate\r\n# note\r\n---\r\nbody\r\n"
        let document = try Document.parse(source)
        let metadata = try #require(document.metadata)
        let data = metadata.content.compactMap { content -> MetadataRecord? in
            if case .data(let record) = content { return record }
            return nil
        }
        #expect(data.map(\.name) == ["a", "b"])
        #expect(data.map(\.value) == [.scalar(.number("9007199254740993")), .scalar(.number("9007199254740993"))])
        #expect(metadata.content[1] == .comment("not YAML"))
        #expect(metadata.content[2] == .comment("..."))
        #expect(metadata.content[3] == .comment("bad: &x [true]"))
        #expect(metadata.content[5] == .comment("a: duplicate"))
        #expect(metadata.scope.end.line == 9)
        #expect(document.content[0].scope.start.line == 10)
        #expect(try Document.parse("---\n---").metadata?.content == [])
        #expect(try Document.parse("---\na: 1\n").metadata == nil)
        #expect(document.dump().contains("MetadataContent value=comment(\"...\")"))
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
            content: values.map { .data(MetadataRecord(name: "key", value: $0, scope: parsed.scope)) },
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
            document.metadata?.content.compactMap { if case .data(let record) = $0 { record.value } else { nil } }
                == values
        )
        #expect(document.dump().contains("value=scalar(number(\"9007199254740993\"))"))
        #expect(document.dump().contains("value=list([])"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(!visitor.events.contains { $0.contains("Metadata") })
    }
}
