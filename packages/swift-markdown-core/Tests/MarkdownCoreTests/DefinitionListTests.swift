import Testing

@testable import MarkdownCore

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

    @Test("body groups preserve empty and multiblock bodies without adding AST records", arguments: [1, 4_096])
    func groupedFields(repetitions: Int) throws {
        let document = try Document.parse("T\n" + String(repeating: ":\n: one\n\n    two\n", count: repetitions))
        let list = try #require(document.content.first as? DefinitionList)
        let definition = try #require(list.definitions.first)
        #expect(definition.content.count == repetitions * 2)
        for offset in stride(from: 0, to: definition.content.count, by: 2) {
            #expect(definition.content[offset].isEmpty)
            let body = definition.content[offset + 1]
            #expect(body.count == 2)
            #expect(((body[0] as? Paragraph)?.content.first as? Text)?.literal == "one")
            #expect(((body[1] as? Paragraph)?.content.first as? Text)?.literal == "two")
        }
        var visitor = RecordingWalkingVisitor(recordEvents: false)
        document.walk(with: &visitor)
        #expect(visitor.entered == 4 + repetitions * 4)
        #expect(document.tree.records.count == visitor.entered)
    }
}
