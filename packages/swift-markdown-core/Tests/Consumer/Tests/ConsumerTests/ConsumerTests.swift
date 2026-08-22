import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let document = try Document.parse("## Consumer\n")

        #expect((document.content.first as? Heading)?.level == 2)
        #expect(document.dump() == TreeDumper.dump(document))
    }
}
