import MarkdownCore
import Testing

@Suite("text") struct TextSuite {
    @Test("an embedded NUL becomes U+FFFD and does not truncate the source")
    func embeddedNulSurvives() throws {
        // A Swift String may contain U+0000. Handing the engine a
        // NUL-terminated C string loses it AND everything after it; the
        // engine takes an explicit length and replaces NUL with U+FFFD.
        let source = "a\u{0}b\n\nsecond\n"
        #expect(Array(source.utf8).count == 12)

        let document = try Document(source)
        #expect(document.content.count == 2)
        let first = try #require(document.content[0] as? Paragraph)
        #expect((first.content[0] as? Text)?.literal == "a\u{FFFD}b")
        let second = try #require(document.content[1] as? Paragraph)
        #expect((second.content[0] as? Text)?.literal == "second")

        // The edit path takes the same route and must not truncate either.
        let commit = try document.edit("lead\n\nx\u{0}y\n")
        let edited = commit.document
        #expect(edited.content.count == 2)
        let tail = try #require(edited.content[1] as? Paragraph)
        #expect((tail.content[0] as? Text)?.literal == "x\u{FFFD}y")
    }
}
