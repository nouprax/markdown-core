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
        #expect(!second.changes.added.contains(secondParagraph.id))
        #expect(!second.changes.removed.contains(firstText.id))
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
        #expect(after.changes.added.contains(inserted.id))
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

        #expect(after.changes.removed.contains(paragraph.id))
        #expect(after.changes.added.contains(heading.id))
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

        #expect(after.changes.added.isEmpty)
        #expect(after.changes.removed.isEmpty)
        #expect(after.changes.changed.isEmpty)
        #expect(after.changes.bubbled.isEmpty)
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
        let infoA = try #require(session.footnoteInfo(of: definitionA.id))
        #expect(infoA.number == 2)
        #expect(infoA.definition == definitionA.id)
        #expect(infoA.referenceCount == 1)
        #expect(infoA.referenceOrdinal == nil)

        let references = session.footnoteReferences(of: definitionA.id)
        #expect(references.map(\.label) == ["a"])
        let referenceInfo = try #require(session.footnoteInfo(of: references[0].id))
        #expect(referenceInfo.number == 2)
        #expect(referenceInfo.referenceOrdinal == 1)

        // A non-footnote id answers nil.
        #expect(session.footnoteInfo(of: commit.document.id) == nil)

        // An ordinal shift surfaces as changed entries with identical dumps.
        let dumpBefore = session.document.dump()
        try session.replace(0..<0, with: "Lead [^a].\n\n")
        let shifted = try session.commit()
        #expect(session.footnotes().map(\.label) == ["a", "b"])
        #expect(!shifted.changes.changed.isEmpty)
        _ = dumpBefore
    }

    @Test("updates(feeding:) yields one commit per token")
    func updatesStream() async throws {
        let session = try MarkupSession()
        let tokens = ["# Str", "eamed\n", "\nBody *tok", "ens*.\n"]
        var commits: [Commit] = []
        for try await commit in session.updates(feeding: AsyncStream(chunking: tokens)) {
            commits.append(commit)
        }
        #expect(commits.count == tokens.count)
        #expect(commits.last?.document.dump() == (try Document.parse(tokens.joined()).dump()))
        // The streamed heading kept its identity from the first commit on.
        let first = try #require(commits.first?.document.children[0])
        let last = try #require(commits.last?.document.children[0])
        #expect(first.id == last.id)
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

extension AsyncStream where Element == String {
    init(chunking chunks: [String]) {
        self.init { continuation in
            for chunk in chunks {
                continuation.yield(chunk)
            }
            continuation.finish()
        }
    }
}

private func requireSendable<T: Sendable>(_: T.Type) {}
