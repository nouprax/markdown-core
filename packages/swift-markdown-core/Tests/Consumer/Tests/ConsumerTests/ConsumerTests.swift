import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let document = try Document.parse("## Consumer\n")

        #expect((document.content.first as? Heading)?.level == 2)
        #expect(document.dump() == TreeDumper.dump(document))
        #expect(document.concrete.source == Array("## Consumer\n".utf8))
    }

    @Test("the streaming session is on the same public product")
    func streamedProduct() throws {
        let session = try Session()
        let updated = try session.feed(chunk: Array("## Consu".utf8))
        // The trailing line has no ending yet, so it is not in the projection.
        #expect(updated.content.isEmpty)
        _ = try session.feed(chunk: Array("mer\n".utf8))
        let sealed = try session.finish()
        let oneShot = try Document.parse("## Consumer\n")
        #expect(sealed.dump() == oneShot.dump())
        #expect(sealed.concrete == oneShot.concrete)
    }
}
