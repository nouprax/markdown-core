import MarkdownCore
import Testing

@Suite("consumer") struct ConsumerTests {
    @Test("a clean package consumes the public MarkdownCore product")
    func publicProduct() throws {
        let document = try Document("## Consumer\n")

        #expect((document.content.first as? Heading)?.level == 2)
        #expect(document.dump() == MarkupDumper.dump(document))

        let directiveDocument = try Document(":badge[label]\n")
        let paragraph = try #require(directiveDocument.content.first as? Paragraph)
        let directive = try #require(paragraph.content.first as? Directive)
        let label: DirectiveLabel = try #require(directive.label)
        #expect(label.content.first is Text)
    }
}
