import MarkdownCore
import Testing

@Suite("sessions") struct SessionSuite {
    @Test("streaming keeps frontier ids and bumps the trailing text revision")
    func streamingIdentityStability() throws {
        let session = try MarkupSession()
        try session.append("# Title\n\nHello")
        let first = try session.commit()
        let firstHeading = try #require(first.document.children[0] as? Heading)
        let firstParagraph = try #require(first.document.children[1] as? Paragraph)
        let firstText = try #require(firstParagraph.children[0] as? Text)

        try session.append(" world")
        let second = try session.commit()
        let secondHeading = try #require(second.document.children[0] as? Heading)
        let secondParagraph = try #require(second.document.children[1] as? Paragraph)
        let secondText = try #require(secondParagraph.children[0] as? Text)

        #expect(secondText.literal == "Hello world")
        #expect(secondParagraph.id == firstParagraph.id)
        #expect(secondText.id == firstText.id)
        #expect(secondText.revision > firstText.revision)
        #expect(secondHeading == firstHeading)
        #expect(!second.delta.added.contains(secondParagraph.id))
        #expect(!second.delta.removed.contains(firstText.id))
    }

    @Test("a clean-boundary insert at the top leaves downstream identity intact")
    func cleanBoundaryInsert() throws {
        let session = try MarkupSession()
        try session.append("First\n\nSecond\n\nThird\n")
        let before = try session.commit()
        let downstreamBefore = before.document.children.map { ($0.id, $0.revision) }

        try session.replace(0..<0, with: "# New\n\n")
        let after = try session.commit()

        #expect(after.document.children.count == 4)
        let inserted = try #require(after.document.children[0] as? Heading)
        #expect(after.delta.added.contains(inserted.id))
        for (index, node) in after.document.children.dropFirst().enumerated() {
            #expect(node.id == downstreamBefore[index].0)
            #expect(node.revision == downstreamBefore[index].1)
        }
        // Downstream nodes shifted by two lines: equal values, new scopes.
        let third = try #require(after.document.children[3] as? Paragraph)
        #expect(after.document.scope(of: third).start.line == 7)
        // An unchanged value carried over from the previous snapshot has the
        // same (id, revision) and resolves against the newer snapshot — at
        // its new position. (A stale value whose revision was superseded
        // traps instead of pairing old fields with a current scope.)
        let thirdBefore = try #require(before.document.children[2] as? Paragraph)
        #expect(after.document.scope(of: thirdBefore).start.line == 7)
        #expect(after.document.dump() == (try Document.parse("# New\n\nFirst\n\nSecond\n\nThird\n").dump()))
    }

    @Test("a kind change is reported as removed plus added")
    func kindChangeRetiresIdentity() throws {
        let session = try MarkupSession()
        try session.append("text\n")
        let before = try session.commit()
        let paragraph = try #require(before.document.children[0] as? Paragraph)

        try session.replace(0..<0, with: "# ")
        let after = try session.commit()
        let heading = try #require(after.document.children[0] as? Heading)

        #expect(after.delta.removed.contains(paragraph.id))
        #expect(after.delta.added.contains(heading.id))
        #expect(heading.id != paragraph.id)
    }

    @Test("equality is lineage-salted identity plus revision")
    func equalitySemantics() throws {
        let source = "Same *content* twice.\n"
        let first = try Document.parse(source)
        let second = try Document.parse(source)
        // Identical content from different parses never compares equal.
        #expect(first != second)
        #expect(first.children[0] as? Paragraph != second.children[0] as? Paragraph)
        // Within one snapshot, identity is value equality.
        #expect(first.children[0] as? Paragraph == first.children[0] as? Paragraph)
        #expect(first.id.lineage != second.id.lineage)
    }

    @Test("a blank-line-only edit commits an empty delta yet shifts scopes")
    func pureShiftCommitsEmptyDelta() throws {
        let session = try MarkupSession()
        try session.append("Alpha\n\n\n\nOmega\n")
        let before = try session.commit()
        let omegaBefore = try #require(before.document.children[1] as? Paragraph)
        #expect(before.document.scope(of: omegaBefore).start.line == 5)

        // Delete two of the blank lines: no node's content changes.
        try session.replace(6..<8, with: "")
        let after = try session.commit()
        let omegaAfter = try #require(after.document.children[1] as? Paragraph)

        #expect(after.delta.added.isEmpty)
        #expect(after.delta.removed.isEmpty)
        #expect(after.delta.changed.isEmpty)
        #expect(after.delta.bubbled.isEmpty)
        #expect(omegaAfter == omegaBefore)
        #expect(after.document.scope(of: omegaAfter).start.line == 3)
        #expect(after.document.dump() == (try Document.parse("Alpha\n\nOmega\n").dump()))
    }

    @Test("materialized scopes survive the session and later commits")
    func scopeMaterializationOutlivesCurrency() throws {
        var session: MarkupSession? = try MarkupSession()
        try session?.append("One\n\nTwo\n")
        let first = try #require(try session?.commit())
        let two = try #require(first.document.children[1] as? Paragraph)
        // Materialize while current.
        #expect(first.document.scope(of: two).start.line == 3)

        try session?.replace(0..<0, with: "Zero\n\n")
        _ = try #require(try session?.commit())
        // The superseded snapshot answers from its cache, at its revision.
        #expect(first.document.scope(of: two).start.line == 3)

        session = nil
        #expect(first.document.scope(of: two).start.line == 3)
    }

    @Test("footnote queries answer numbering, resolution, and back-references")
    func footnoteQueries() throws {
        let session = try MarkupSession()
        try session.append("See [^b] then [^a].\n\n[^a]: A\n\n[^b]: B\n")
        let commit = try session.commit()

        let footnotes = session.footnotes()
        #expect(footnotes.map(\.label) == ["b", "a"])

        let definitionA = try #require(footnotes.last)
        let infoA = try #require(session.footnote(of: definitionA.id))
        #expect(infoA.number == 2)
        #expect(infoA.definition == definitionA.id)
        #expect(infoA.referenceCount == 1)
        #expect(infoA.referenceOrdinal == nil)

        let references = session.references(of: definitionA.id)
        #expect(references.map(\.label) == ["a"])
        let referenceInfo = try #require(session.footnote(of: references[0].id))
        #expect(referenceInfo.number == 2)
        #expect(referenceInfo.referenceOrdinal == 1)

        // A non-footnote id answers nil.
        #expect(session.footnote(of: commit.document.id) == nil)

        // An ordinal shift surfaces as changed entries with identical dumps.
        let dumpBefore = session.document.dump()
        try session.replace(0..<0, with: "Lead [^a].\n\n")
        let shifted = try session.commit()
        #expect(session.footnotes().map(\.label) == ["a", "b"])
        #expect(!shifted.delta.changed.isEmpty)
        _ = dumpBefore
    }

    @Test("conflated streaming: irregular render ticks over a multi-turn conversation")
    func conflatedStreaming() throws {
        // The shape of a real LLM consumer: every socket message appends
        // (nothing parses), only an irregular render tick commits, and the
        // messages between ticks conflate into that one commit. Three
        // assistant turns extend one document; blocks settled at a turn
        // boundary must stay frozen while later turns stream.
        let turns = [
            "# Streaming\n\nThe *quick* parser holds **steady** under bursts, "
                + "and the heading keeps its identity from the first render on.\n\n"
                + "Deltas stay proportional to what changed, so a renderer "
                + "reconciles by id instead of walking the whole tree.\n\n"
                + "> Snapshots are values: whatever a tick captured stays valid "
                + "while the socket races ahead.",
            "\n\n- append per message\n- commit per tick\n- settled blocks stay frozen"
                + "\n- identical items stress identity\n- identical items stress identity"
                + "\n\n```swift\nlet constant = 1\nlet mirror = [Int: String]()\n"
                + "for index in 0..<3 {\n    print(index, constant)\n}\n```\n\n"
                + "Fenced code arrives line by line and only closes at the final tick.",
            "\n\nA table lands late in the conversation:\n\n"
                + "| stage | commits | messages |\n| - | - | - |\n| one | 3 | 9 |\n"
                + "| two | 5 | 14 |\n| three | 8 | 21 |\n\n"
                + "Tail with a footnote[^n] whose definition arrives last.\n\n"
                + "[^n]: Resolved at the end, after every reference already rendered.",
        ]
        // One fixed generator drives batch sizes and tick timing, so the
        // burst shapes are irregular but reproducible — and identical in the
        // Kotlin and ES mirrors of this test.
        var state: UInt64 = 0x9E37_79B9_7F4A_7C15
        func draw(_ bound: UInt64) -> UInt64 {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return (state >> 33) % bound
        }
        let session = try MarkupSession()
        var streamed = ""
        var frozen: [(index: Int, id: MarkupID, revision: UInt64)] = []
        var messages = 0
        var commits = 0
        var touched = 0
        func tick() throws {
            let commit = try session.commit()
            commits += 1
            touched += commit.delta.added.count + commit.delta.removed.count
                + commit.delta.changed.count + commit.delta.bubbled.count
            #expect(commit.document.dump() == (try Document.parse(streamed).dump()))
            for entry in frozen {
                let node = commit.document.children[entry.index]
                #expect(node.id == entry.id)
                #expect(node.revision == entry.revision)
            }
        }

        for turn in turns {
            var pending = Array(turn)[...]
            while !pending.isEmpty {
                // Mostly a 20-30 token batch (80-150 characters), with
                // occasional tiny flushes of a few words. Cuts land at raw
                // character offsets — mid-word, mid-marker, even between
                // the two newlines of a block boundary — because that is
                // the steady state of LLM output.
                let width = draw(10) < 2 ? 2 + Int(draw(18)) : 80 + Int(draw(71))
                let message = String(pending.prefix(width))
                pending = pending.dropFirst(width)
                try session.append(message)
                streamed += message
                messages += 1
                if draw(4) == 0 { try tick() }
            }
            // The turn boundary always renders; everything but the still-hot
            // last block is now settled.
            try tick()
            frozen = session.document.children.dropLast().enumerated().map {
                (index: $0.offset, id: $0.element.id, revision: $0.element.revision)
            }
        }
        #expect(messages > 9)
        #expect(commits < messages)
        #expect(session.document.dump() == (try Document.parse(turns.joined()).dump()))

        // Near-O(n) pipeline: total delta traffic stays within one add per
        // final node plus bounded frontier churn per tick. A full rebuild
        // per tick would be on the order of commits * nodes.
        var nodes = 0
        MarkupWalker().walk(session.document) { event, _, _ in
            if event == .entering { nodes += 1 }
        }
        #expect(touched < nodes + 16 * commits)
    }

    @Test("session snapshots are Sendable values; ids are stable dictionary keys")
    func snapshotValueSemantics() throws {
        requireSendable(Commit.self)
        requireSendable(Delta.self)
        requireSendable(MarkupID.self)
        requireSendable(FootnoteInfo.self)
        let session = try MarkupSession()
        #expect(session.document.children.isEmpty)
        #expect(session.revision == 0)
        try session.append("Alpha\n")
        let commit = try session.commit()
        let paragraph = try #require(commit.document.children[0] as? Paragraph)
        var byID: [MarkupID: String] = [:]
        byID[paragraph.id] = "paragraph"
        #expect(session.node(for: paragraph.id)?.id == paragraph.id)
        #expect(byID[paragraph.id] == "paragraph")
    }
}

private func requireSendable<T: Sendable>(_: T.Type) {}
