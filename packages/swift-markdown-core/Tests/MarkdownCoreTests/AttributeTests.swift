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

extension APISuite {
    @Test("P2 inheritance retains native values and occurrence scopes")
    func attributeSites() throws {
        let document = try Document.parse(
            "# T ## {#heading}\n\n`x`{.code} [x][r]{#own .same k=2} "
                + "![alt|20x30][r]{width=50% height=2in}\n\n[r]: /u {#definition .same k=1 k=1}\n"
        )
        #expect(document.content[0].anchor == "heading")
        let paragraph = try #require(document.content[1] as? Paragraph)
        let code = try #require(paragraph.content[0] as? Code)
        let link = try #require(paragraph.content[2] as? Link)
        let image = try #require(paragraph.content[4] as? Media)
        #expect(code.literal == "x" && code.attributes.classes == ["code"])
        #expect(code.scope.end.column == 10)
        #expect(link.anchor == "own")
        #expect(link.attributes.classes == ["same", "same"])
        #expect(link.attributes.records.map(\.value) == ["1", "1", "2"])
        #expect(image.anchor == "definition")
        #expect(image.dimensions == Dimensions(width: 20, height: 30))
        #expect(image.attributes.records.suffix(2).map(\.value) == ["50%", "2in"])
        #expect(link.scope.end.line == 3 && image.scope.end.line == 3)
    }
}

extension ErrorsSuite {
    @Test("every occurrence of one reference definition materializes one resource")
    func sharedResource() throws {
        // M2: the C tree shares one resource across every occurrence of a
        // definition, and the Swift tree decodes it once. The destination is
        // long enough to live in heap storage, so two Strings that share it
        // report one buffer and two independent decodes would report two.
        let destination = "/" + String(repeating: "u", count: 1024)
        let count = 5_000
        let anchor = String(repeating: "a", count: 1024)
        let classes = String(repeating: " .c", count: 1024)
        let document = try Document.parse(
            "[a]: \(destination) {#\(anchor)\(classes) k=\(destination)}\n\n"
                + String(repeating: "[a]\n\n", count: count)
        )
        let links = try document.content.map { try #require(($0 as? Paragraph)?.content.first as? Link) }
        #expect(links.count == count)
        #expect(links[0].anchor == anchor)
        #expect(links[0].attributes.classes.count == 1024)
        let classStorage = links[0].attributes.classes.withUnsafeBufferPointer { $0.baseAddress }
        for link in links {
            #expect(link.attributes.classes.withUnsafeBufferPointer { $0.baseAddress } == classStorage)
        }
        var editable = links[0].attributes.classes
        editable[0] = "edited"
        #expect(links[1].attributes.classes[0] == "c")
        guard case .url(var first) = links[0].dest else {
            Issue.record("a resolved reference is the url branch")
            return
        }
        #expect(first == destination)
        let storage = first.withUTF8 { UnsafeRawPointer($0.baseAddress!) }
        for link in links.dropFirst() {
            guard case .url(var url) = link.dest else {
                Issue.record("a resolved reference is the url branch")
                return
            }
            #expect(url.withUTF8 { UnsafeRawPointer($0.baseAddress!) } == storage)
        }
    }

}
