import MarkdownCore
import Testing

@Suite("streaming") struct StreamingSuite {
    @Test("chunked feeds sealed by finish equal the one-shot parse")
    func chunkedEqualsOneShot() throws {
        // CRLF endings and multi-byte scalars, fed 3 bytes at a time, so a
        // chunk boundary falls inside a UTF-8 sequence and between a CR and
        // its LF -- the splits only a byte stream can spell.
        let source = "# héllo 🚀\r\n\r\n> quoted 中文 *em*\r\n\r\n- [x] task 🚀\r\n"
        let session = try Session()
        try feedChunked(Array(source.utf8), by: 3, into: session)
        let sealed = try session.finish()
        let oneShot = try Document.parse(source)
        #expect(sealed.dump() == oneShot.dump())
        #expect(sealed.concrete == oneShot.concrete)
    }

    @Test("a mid-stream document is a value later feeds cannot disturb")
    func midStreamDocumentIsAValue() throws {
        var early: Document?
        do {
            let session = try Session()
            let updated = try session.feed(chunk: Array("# Heading\n\ntail".utf8))
            // The heading's line ending arrived; `tail`'s has not, so the
            // trailing line is not yet in the projection -- and the normalized
            // source likewise carries only the complete lines.
            #expect(updated.content.count == 1)
            #expect((updated.content.first as? Heading)?.level == 1)
            #expect(updated.concrete.source == Array("# Heading\n\n".utf8))
            let record = updated.dump()
            early = updated
            _ = try session.feed(chunk: Array(" grows\n\n> quote\n".utf8))
            let sealed = try session.finish()
            #expect(updated.dump() == record)
            #expect(sealed.dump() != record)
        }
        // The session died with the scope; the value still reads.
        let held = try #require(early)
        #expect((held.content.first as? Heading)?.level == 1)
        #expect(held.concrete.source == Array("# Heading\n\n".utf8))
    }

    @Test("a sealed session refuses feed and a second finish")
    func sealedSessionRefuses() throws {
        let session = try Session()
        _ = try session.feed(chunk: Array("done\n".utf8))
        _ = try session.finish()

        var fed: ParseError?
        do {
            _ = try session.feed(chunk: Array("late\n".utf8))
        } catch let error as ParseError {
            fed = error
        }
        #expect(fed?.code == .invalidArgument)

        var finished: ParseError?
        do {
            _ = try session.finish()
        } catch let error as ParseError {
            finished = error
        }
        #expect(finished?.code == .invalidArgument)
    }

    @Test("an empty feed is legal and an empty stream seals to an empty document")
    func emptyFeeds() throws {
        let session = try Session()
        let standing = try session.feed(chunk: [])
        #expect(standing.content.isEmpty)
        let sealed = try session.finish()
        let oneShot = try Document.parse("")
        #expect(sealed.content.isEmpty)
        #expect(sealed.dump() == oneShot.dump())
        #expect(sealed.concrete == oneShot.concrete)
    }

    @Test("session options gate constructs exactly as parse options do")
    func optionsMatchParse() throws {
        let source = "| a |\n| --- |\n| b |\n"
        let options = ParseOptions(tables: false)
        let session = try Session(options: options)
        // The canonical spelling: a `String` chunk feeds its UTF-8 bytes.
        let updated = try session.feed(chunk: source)
        #expect(updated.content.first is Paragraph)
        let sealed = try session.finish()
        #expect(sealed.content.first is Paragraph)
        let oneShot = try Document.parse(source, options: options)
        #expect(sealed.dump() == oneShot.dump())
    }
}

private func feedChunked(_ bytes: [UInt8], by width: Int, into session: Session) throws {
    var start = 0
    while start < bytes.count {
        let end = min(start + width, bytes.count)
        _ = try session.feed(chunk: Array(bytes[start..<end]))
        start = end
    }
}
