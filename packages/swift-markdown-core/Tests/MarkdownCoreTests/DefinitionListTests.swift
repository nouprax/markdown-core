import MarkdownCore
import Testing

@Suite("ast") struct DefinitionListTests {
    @Test("definition terms and ordered bodies survive native release and walk without body wrapper nodes")
    func ownedFields() throws {
        let block = try #require(
            Document.parse("::: box\n*T*\n: one\n~\n\nU\n\n: two\n:::\n").content.first as? DirectiveBlock
        )
        #expect(block.name == nil)
        #expect(block.attributes.classes == ["box"])
        let list = try #require(block.content.first as? DefinitionList)
        #expect(list.definitions.count == 2)
        let first = list.definitions[0]
        #expect(first.compact)
        #expect(first.content.count == 2 && first.content[1].isEmpty)
        #expect(((first.term.first as? Emphasis)?.content.first as? Text)?.literal == "T")
        #expect(((first.content[0].first as? Paragraph)?.content.first as? Text)?.literal == "one")
        #expect(!list.definitions[1].compact)
        var visitor = RecordingWalkingVisitor()
        list.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:DefinitionList", "entering:Definition", "entering:Emphasis", "entering:Text", "exiting:Text",
                "exiting:Emphasis", "entering:Paragraph", "entering:Text", "exiting:Text", "exiting:Paragraph",
                "exiting:Definition", "entering:Definition", "entering:Text", "exiting:Text", "entering:Paragraph",
                "entering:Text", "exiting:Text", "exiting:Paragraph", "exiting:Definition", "exiting:DefinitionList",
            ]
        )
    }
}
