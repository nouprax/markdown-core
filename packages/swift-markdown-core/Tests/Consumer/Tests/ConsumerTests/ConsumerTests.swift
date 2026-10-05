import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let source = "## Consumer\n"
        let document = try Document.parse(source)

        let heading = try #require(document.content.first as? Heading)
        #expect(heading.level == 2)
        #expect(try document.dump(in: source).hasPrefix("Document scope=1:1..1:11 "))
        #expect(try document.dump(heading, in: source).hasPrefix("Heading scope=1:1..1:11 "))
        #expect(
            try document.scope(of: heading, in: source)
                == [Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 11))]
        )
        #expect(try document.node(at: Position(line: 1, column: 4), in: source)?.isEqual(heading.content[0]) == true)
        #expect(document.content.count == 1)
        #expect(Array(document.content.indices) == [0])
        let blocks: [any Markup] = Array(document.content)
        #expect(blocks[0] is Heading)
        #expect(document.content.reversed().first is Heading)
    }

    @Test("definition groups expose random access and preserve their grouping")
    func definitionGroups() throws {
        let document = try Document.parse("T\n: one\n:\n: three\n")
        let list = try #require(document.content.first as? DefinitionList)
        let groups: MarkupGroups<any Markup> = list.definitions[0].content
        #expect(Array(groups.indices) == [0, 1, 2])
        #expect(groups[1].isEmpty)
        #expect(Array(groups[1...].indices) == [1, 2])
        #expect(((groups.reversed().first?.first as? Paragraph)?.content.first as? Text)?.literal == "three")
        let bodies: [MarkupCollection<any Markup>] = Array(groups)
        #expect(bodies.map(\.count) == [1, 0, 1])
    }

    @Test("reference definitions are public leaf blocks that a reference link names")
    func referenceDefinitions() throws {
        let document = try Document.parse("[Text][Label]\n\n[label]: /u \"t\"\n")
        let link = try #require((document.content.first as? Paragraph)?.content.first as? Link)
        let reference = try #require(document.content.last as? Reference)
        #expect(link.dest == .reference(label: "label") && link.title == nil)
        #expect(reference.label == "label" && reference.dest == .url("/u") && reference.title == "t")
        #expect(Array(document.references) == [reference])
        #expect((document.reference(for: "label") as? Reference) == reference)
    }
}
