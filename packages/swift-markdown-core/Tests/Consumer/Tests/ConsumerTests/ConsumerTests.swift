import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let document = try Document.parse("## Consumer\n")

        #expect((document.content.first as? Heading)?.level == 2)
        #expect(document.dump() == TreeDumper.dump(document))
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
}
