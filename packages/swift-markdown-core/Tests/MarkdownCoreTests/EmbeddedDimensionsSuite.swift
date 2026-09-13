import MarkdownCore
import Testing

@Suite("ast") struct EmbeddedDimensionsSuite {
    @Test("image dimensions preserve formatted alt and per-occurrence values")
    func imageDimensions() throws {
        let document = try Document.parse("![*alt*|2147483647x2][r] ![3][r] ![bad|01][r]\n\n[r]: /shared \"title\"\n")
        let paragraph = try #require(document.content.first as? Paragraph)
        let images = paragraph.content.compactMap { $0 as? Embedded }
        #expect(images.map(\.dimensions) == [Dimensions(width: 2147483647, height: 2), Dimensions(width: 3), nil])
        let size = Dimensions(width: 640, height: 480)
        #expect(Set([size, Dimensions(width: 640, height: 480)]).count == 1)
        #expect(images[0].dest == images[1].dest)
        #expect(images[0].title == "title")
        let alt = try #require(images[0].content.first as? Emphasis)
        #expect((alt.content.first as? Text)?.literal == "alt")
        #expect(alt.scope.end.column == 7)
        #expect(images[1].content.isEmpty)
        #expect((images[2].content.first as? Text)?.literal == "bad|01")
        var visitor = RecordingWalkingVisitor()
        images[0].walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Embedded", "entering:Emphasis", "entering:Text", "exiting:Text", "exiting:Emphasis",
                "exiting:Embedded",
            ]
        )
    }
    @Test("embedded cross links share Dimensions while preserving raw prefixes")
    func embeddedDimensions() throws {
        let document = try Document.parse("![[v.mp4|*raw*|2147483647x2]] ![[n|3]] [[n|100]] ![[n|bad|01]] ![[n]]\n")
        let paragraph = try #require(document.content.first as? Paragraph)
        let links = paragraph.content.compactMap { $0 as? CrossEmbedded }
        #expect(
            links.map(\.dimensions) == [Dimensions(width: 2147483647, height: 2), Dimensions(width: 3), nil, nil]
        )
        #expect(links.map(\.label) == ["*raw*", "", "bad|01", nil])
        let plain = try #require(paragraph.content.compactMap { $0 as? CrossLink }.first)
        #expect(plain.label == "100")
        var visitor = RecordingWalkingVisitor()
        links[0].walk(with: &visitor)
        #expect(visitor.events == ["entering:CrossEmbedded", "exiting:CrossEmbedded"])
    }

}
