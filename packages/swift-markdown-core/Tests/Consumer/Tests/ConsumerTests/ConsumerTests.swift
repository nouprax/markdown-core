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
}
