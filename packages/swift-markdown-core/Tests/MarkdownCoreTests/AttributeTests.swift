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
        #expect(try scope(of: metadata, in: document, source: source).end.line == 14)
        #expect(try scope(of: document.content[0], in: document, source: source).start.line == 15)
        let empty = try #require(Document.parse("---\nunknown: 1\nfree text\n---").metadata)
        let bare = InheritedFields(id: empty.id, extent: empty.extent, anchor: nil, attributes: .empty)
        #expect(empty == Metadata(record: MetadataRecord(bare)))
        #expect(try Document.parse("---\nname: 1\n").metadata == nil)
    }

    @Test("universal attributes retain ordered values after native document release")
    func universalAttributes() throws {
        let source = ":n{#id .a class=\"a b}c\" k=1 k=2}"
        let parsed = try Document.parse(source)
        let paragraph = try #require(parsed.content.first as? Paragraph)
        let directive = try #require(paragraph.content.first as? Directive)
        #expect(directive.anchor == "id")
        #expect(directive.attributes.classes == ["a", "a", "b}c"])
        #expect(directive.attributes.records == [Record(name: "k", value: "1"), Record(name: "k", value: "2")])
        let dump = try parsed.dump(directive, in: source)
        #expect(dump.contains("attributes={.a .a .\"b}c\" k=\"1\" k=\"2\"}"))
        #expect(parsed.anchor == nil && parsed.attributes == .empty && parsed.metadata == nil)
    }

    @Test("metadata is a leaf Markup and preserves decimal text")
    func metadataValues() throws {
        let values: [MetadataValue] = [
            .scalar(.null), .scalar(.bool(true)), .scalar(.number("9007199254740993")),
            .scalar(.text("中文\nquoted")), .list([]), .list([.number("1.25"), .text("")]),
        ]
        let metadata = MetadataRecord(
            fields(2),
            name: values[0],
            title: values[1],
            subtitle: values[2],
            time: values[3],
            date: values[4],
            authors: values[5]
        )
        let document = Document(
            record: DocumentRecord(
                fields(1),
                unit: .utf16,
                metadata: metadata,
                content: [],
                footnotes: [],
                specimens: [],
                references: [],
                referenceLabels: [:]
            )
        )
        #expect(
            [
                document.metadata?.name, document.metadata?.title, document.metadata?.subtitle,
                document.metadata?.time, document.metadata?.date, document.metadata?.authors,
            ]
                == values
        )
        let dump = try document.dump(in: "")
        #expect(dump.contains("subtitle=scalar(number(\"9007199254740993\"))"))
        #expect(dump.contains("date=list([])"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(Array(visitor.events.prefix(3)) == ["enter:Document", "enter:Metadata", "exit:Metadata"])
    }
}

extension APISuite {
    @Test("each attribute site keeps its own values and occurrence scopes")
    func attributeSites() throws {
        let source =
            "# T ## {#heading}\n\n`x`{.code} [x][r]{#own .same k=2} "
            + "![alt|20x30][r]{width=50% height=2in}\n\n[r]: /u {#definition .same k=1 k=1}\n"
        let document = try Document.parse(source)
        #expect(document.content[0].anchor == "heading")
        let paragraph = try #require(document.content[1] as? Paragraph)
        let code = try #require(paragraph.content[0] as? Code)
        let link = try #require(paragraph.content[2] as? Link)
        let image = try #require(paragraph.content[4] as? Embedded)
        let reference = try #require(document.content[2] as? Reference)
        #expect(code.literal == "x" && code.attributes.classes == ["code"])
        #expect(try scope(of: code, in: document, source: source).end.column == 10)
        // A reference occurrence carries only what it wrote itself.
        #expect(link.anchor == "own")
        #expect(link.attributes == Attributes(classes: ["same"], records: [Record(name: "k", value: "2")]))
        #expect(link.dest == .reference(label: "r") && link.title == nil)
        #expect(image.anchor == nil)
        #expect(image.dimensions == Dimensions(width: 20, height: 30))
        #expect(image.attributes.classes.isEmpty)
        #expect(image.attributes.records.map(\.value) == ["50%", "2in"])
        #expect(image.dest == .reference(label: "r") && image.title == nil)
        // The definition keeps the attributes it states.
        #expect(reference.anchor == "definition")
        #expect(reference.attributes.classes == ["same"])
        #expect(reference.attributes.records.map(\.value) == ["1", "1"])
        #expect(try scope(of: link, in: document, source: source).end.line == 3)
        #expect(try scope(of: image, in: document, source: source).end.line == 3)
        #expect(try scope(of: reference, in: document, source: source).start.line == 5)
    }
}

extension APISuite {
    @Test("a reference definition is a leaf block with its label, destination and title")
    func referenceFields() throws {
        let source = "[Foo  Bar]: </a b> \"t\" {#x .c k=v}\n\n[u]: /u\n\n[e]: /e \"\"\n"
        let document = try Document.parse(source)
        let references = document.content.compactMap { $0 as? Reference }
        #expect(references.count == 3)
        #expect(references.map(\.label) == ["foo bar", "u", "e"])
        #expect(references.map(\.dest) == [.url("/a b"), .url("/u"), .url("/e")])
        #expect(references.map(\.title) == ["t", nil, ""])
        #expect(references[0].anchor == "x")
        #expect(references[0].attributes == Attributes(classes: ["c"], records: [Record(name: "k", value: "v")]))
        #expect(references[1].anchor == nil && references[1].attributes == .empty)
        var visitor = RecordingWalkingVisitor()
        references[0].walk(with: &visitor)
        #expect(visitor.events == ["enter:Reference", "exit:Reference"])
        #expect(
            try document.dump(references[0], in: source)
                == "Reference scope=1:1..1:34 anchor=\"x\" attributes={.c k=\"v\"} label=\"foo bar\" "
                + "dest=url(\"/a b\") title=\"t\" children=0\n"
        )
    }

    @Test("every reference form names its definition by the normalized label")
    func referenceDestinations() throws {
        let source = "[t][Ref] [Ref][] [Ref] ![i][REF] [d](/d \"t\")\n\n[ref]: /u \"title\"\n"
        let document = try Document.parse(source)
        let inline = try #require(document.content[0] as? Paragraph)
        let links = inline.content.compactMap { $0 as? Link }
        let embedded = try #require(inline.content.compactMap { $0 as? Embedded }.first)
        let named = Destination.reference(label: "ref")
        #expect(links.map(\.dest) == [named, named, named, .url("/d")])
        #expect(links.map(\.title) == [nil, nil, nil, "t"])
        #expect(embedded.dest == named && embedded.title == nil)
        #expect(try document.dump(links[0], in: source).contains(" dest=reference(\"ref\") title=null "))
        // An unresolved reference stays literal text.
        let literal = try #require(try Document.parse("[missing]\n").content.first as? Paragraph)
        #expect(literal.content.allSatisfy { $0 is Text })
    }

    @Test("the document lists references in source order and resolves a label to a reference or a heading")
    func referenceTable() throws {
        let source =
            "[Target] [a] [Both]\n\n# Target {#chapter}\n\n# Both\n\n"
            + "[a]: /one\n[a]: /two \"t\"\n\n[unused]: /x\n\n[both]: /b\n"
        let document = try Document.parse(source)
        let chapter = try #require(document.content[1] as? Heading)
        #expect(document.references.map(\.label) == ["a", "a", "unused", "both"])
        #expect(document.references.map(\.dest) == [.url("/one"), .url("/two"), .url("/x"), .url("/b")])
        #expect(document.references.map(\.id) == document.content.compactMap { ($0 as? Reference)?.id })
        // Duplicates remain; the first one in source order resolves.
        let first = try #require(document.reference(for: "a") as? Reference)
        #expect(first == document.references[0])
        // A heading resolves a label no definition states.
        let resolved = try #require(document.reference(for: "target") as? Heading)
        #expect(resolved == chapter)
        // A definition resolves before a heading of the same label.
        #expect((document.reference(for: "both") as? Reference) == document.references[3])
        #expect(document.reference(for: "unused")?.isEqual(document.references[2]) == true)
        // A label matches byte for byte: an unnormalized spelling is another label.
        #expect(document.reference(for: "Target") == nil)
        #expect(document.reference(for: "missing") == nil)
        #expect(try Document.parse("text\n").references.isEmpty)
    }
}
