import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let parsed = try Document.parse("## Consumer\n")
        let document = parsed.semantic

        #expect((document.content.first as? Heading)?.level == 2)
        #expect(document.dump() == TreeDumper.dump(document))
        #expect(parsed.concrete.source == Array("## Consumer\n".utf8))
    }
}
