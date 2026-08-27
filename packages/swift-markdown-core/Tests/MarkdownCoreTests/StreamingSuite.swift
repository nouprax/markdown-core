import MarkdownCore
import Testing

@Suite("streaming") struct StreamingSuite {
    @Test("chunked feeds equal the whole-text parse once sealed")
    func chunkedEqualsWholeText() throws {
        // CRLF endings and multi-byte scalars, fed 3 bytes at a time, so a
        // chunk boundary falls inside a UTF-8 sequence and between a CR and
        // its LF -- the splits only a byte stream can spell.
        let source = "# héllo 🚀\r\n\r\n> quoted 中文 *em*\r\n\r\n- [x] task 🚀\r\n"
        let document = try Document()
        try feedChunked(Array(source.utf8), by: 3, into: document)
        let sealed = try document.seal()
        let wholeText = try Document(markdown: source).seal()
        #expect(sealed.dump() == wholeText.dump())
        #expect(sealed.concrete == wholeText.concrete)
    }

    @Test("a mid-stream read is a value later feeds cannot disturb")
    func midStreamReadIsAValue() throws {
        var early: Read?
        do {
            let document = try Document()
            let updated = try document.feed(chunk: Array("# Heading\n\ntail".utf8))
            // The heading's line ending arrived; `tail`'s has not, so the
            // trailing line is not yet in the projection -- and the normalized
            // source likewise carries only the complete lines.
            #expect(updated.semantic.content.count == 1)
            #expect((updated.semantic.content.first as? Heading)?.level == 1)
            #expect(updated.concrete.source == Array("# Heading\n\n".utf8))
            let record = updated.dump()
            early = updated
            _ = try document.feed(chunk: Array(" grows\n\n> quote\n".utf8))
            let sealed = try document.seal()
            #expect(updated.dump() == record)
            #expect(sealed.dump() != record)
        }
        // The document died with the scope; the value still reads.
        let held = try #require(early)
        #expect((held.semantic.content.first as? Heading)?.level == 1)
        #expect(held.concrete.source == Array("# Heading\n\n".utf8))
    }

    @Test("a block keeps its identity across feeds and a reference names the first definition")
    func identityAcrossFeedsAndTheDefinitionEdge() throws {
        let document = try Document()
        // The heading is the element a consumer renders; later feeds and the
        // seal must keep calling it by the same name (D4) -- the render key.
        let first = try document.feed(chunk: "# Title\n\nsee [a] and [^n].\n\n")
        let heading = try #require(first.semantic.content.first as? Heading)
        let second = try document.feed(chunk: "[a]: /first\n\n[a]: /second\n\n[^n]: note\n")
        #expect(second.semantic.content.first?.id == heading.id)
        let sealed = try document.seal()
        #expect(sealed.semantic.content.first?.id == heading.id)

        // Duplicate definitions: both stay in the tree, and the reference
        // names the FIRST by identity -- its own match key is the winning
        // definition's norm.
        let definitions = sealed.semantic.content.compactMap { $0 as? ReferenceDefinition }
        #expect(definitions.count == 2)
        let paragraph = try #require(sealed.semantic.content[1] as? Paragraph)
        let reference = try #require(paragraph.content.compactMap { $0 as? LinkReference }.first)
        #expect(reference.definition == definitions.first?.id)
        #expect(definitions.first?.norm == "a")
        let footnote = try #require(sealed.semantic.content.compactMap { $0 as? FootnoteDefinition }.first)
        let call = try #require(paragraph.content.compactMap { $0 as? FootnoteReference }.first)
        #expect(call.definition == footnote.id)
        #expect(footnote.norm == "^n")

        // An inline's identity is (owning block, ordinal): unique within its
        // paragraph, owned by it.
        for node in paragraph.content {
            #expect(node.id.block == paragraph.id.block)
        }
        #expect(Set(paragraph.content.map { $0.id }).count == paragraph.content.count)
    }

    @Test("sealing releases the shell and a sealed document refuses every call")
    func sealedDocumentRefuses() throws {
        let document = try Document(markdown: "done\n")
        _ = try document.seal()

        var fed: ParseError?
        do {
            _ = try document.feed(chunk: Array("late\n".utf8))
        } catch let error as ParseError {
            fed = error
        }
        #expect(fed?.code == .invalidArgument)

        var sealedAgain: ParseError?
        do {
            _ = try document.seal()
        } catch let error as ParseError {
            sealedAgain = error
        }
        #expect(sealedAgain?.code == .invalidArgument)
    }

    @Test("an empty feed is legal and an empty stream seals to an empty read")
    func emptyFeeds() throws {
        let document = try Document()
        let standing = try document.feed(chunk: [])
        #expect(standing.semantic.content.isEmpty)
        let sealed = try document.seal()
        let wholeText = try Document(markdown: "").seal()
        #expect(sealed.semantic.content.isEmpty)
        #expect(sealed.dump() == wholeText.dump())
        #expect(sealed.concrete == wholeText.concrete)
    }

    @Test("options gate constructs identically however the document is opened")
    func optionsMatchAcrossEntries() throws {
        let source = "| a |\n| --- |\n| b |\n"
        let options = ParseOptions(tables: false)
        let document = try Document(options: options)
        // The canonical spelling: a `String` chunk feeds its UTF-8 bytes.
        let updated = try document.feed(chunk: source)
        #expect(updated.semantic.content.first is Paragraph)
        let sealed = try document.seal()
        #expect(sealed.semantic.content.first is Paragraph)
        let wholeText = try Document(markdown: source, options: options).seal()
        #expect(sealed.dump() == wholeText.dump())
    }
}

private func feedChunked(_ bytes: [UInt8], by width: Int, into document: Document) throws {
    var start = 0
    while start < bytes.count {
        let end = min(start + width, bytes.count)
        _ = try document.feed(chunk: Array(bytes[start..<end]))
        start = end
    }
}
