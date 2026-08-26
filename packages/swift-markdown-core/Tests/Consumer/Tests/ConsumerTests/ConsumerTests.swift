import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let read = try Document(markdown: "## Consumer\n").seal()

        #expect((read.semantic.content.first as? Heading)?.level == 2)
        #expect(read.dump() == TreeDumper.dump(read.semantic))
        #expect(read.concrete.source == Array("## Consumer\n".utf8))
    }

    @Test("the stream is on the same public product")
    func streamedProduct() throws {
        let document = try Document()
        let updated = try document.feed(chunk: Array("## Consu".utf8))
        // The trailing line has no ending yet, so it is not in the projection.
        #expect(updated.semantic.content.isEmpty)
        _ = try document.feed(chunk: Array("mer\n".utf8))
        let sealed = try document.seal()
        let wholeText = try Document(markdown: "## Consumer\n").seal()
        #expect(sealed.dump() == wholeText.dump())
        #expect(sealed.concrete == wholeText.concrete)
    }
}
