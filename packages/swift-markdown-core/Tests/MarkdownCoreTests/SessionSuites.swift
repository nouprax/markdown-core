import Foundation
import MarkdownCore
import Testing

@Suite("sessions") struct SessionSuite {
    @Test("streaming keeps frontier ids and bumps the trailing text revision")
    func streamingIdentityStability() throws {
        let session = try MarkupSession()
        try session.append("# Title\n\nHello")
        let first = try session.commit()
        let firstHeading = try #require(first.document.content[0] as? Heading)
        let firstParagraph = try #require(first.document.content[1] as? Paragraph)
        let firstText = try #require(firstParagraph.content[0] as? Text)

        try session.append(" world")
        let second = try session.commit()
        let secondHeading = try #require(second.document.content[0] as? Heading)
        let secondParagraph = try #require(second.document.content[1] as? Paragraph)
        let secondText = try #require(secondParagraph.content[0] as? Text)

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
        let downstreamBefore = before.document.content.map { ($0.id, $0.revision) }

        try session.replace(0..<0, with: "# New\n\n")
        let after = try session.commit()

        #expect(after.document.content.count == 4)
        let inserted = try #require(after.document.content[0] as? Heading)
        #expect(after.delta.added.contains(inserted.id))
        for (index, node) in after.document.content.dropFirst().enumerated() {
            #expect(node.id == downstreamBefore[index].0)
            #expect(node.revision == downstreamBefore[index].1)
        }
        // Downstream nodes shifted by two lines: equal values, new scopes.
        let third = try #require(after.document.content[3] as? Paragraph)
        #expect(after.document.scope(of: third).start.line == 7)
        // An unchanged value carried over from the previous snapshot has the
        // same (id, revision) and resolves against the newer snapshot — at
        // its new position. (A stale value whose revision was superseded
        // traps instead of pairing old fields with a current scope.)
        let thirdBefore = try #require(before.document.content[2] as? Paragraph)
        #expect(after.document.scope(of: thirdBefore).start.line == 7)
        #expect(after.document.dump() == (try Document.parse("# New\n\nFirst\n\nSecond\n\nThird\n").dump()))
    }

    @Test("a kind change is reported as removed plus added")
    func kindChangeRetiresIdentity() throws {
        let session = try MarkupSession()
        try session.append("text\n")
        let before = try session.commit()
        let paragraph = try #require(before.document.content[0] as? Paragraph)

        try session.replace(0..<0, with: "# ")
        let after = try session.commit()
        let heading = try #require(after.document.content[0] as? Heading)

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
        #expect(first.content[0] as? Paragraph != second.content[0] as? Paragraph)
        // Within one snapshot, identity is value equality.
        #expect(first.content[0] as? Paragraph == first.content[0] as? Paragraph)
        #expect(first.id.lineage != second.id.lineage)
    }

    @Test("a blank-line-only edit commits an empty delta yet shifts scopes")
    func pureShiftCommitsEmptyDelta() throws {
        let session = try MarkupSession()
        try session.append("Alpha\n\n\n\nOmega\n")
        let before = try session.commit()
        let omegaBefore = try #require(before.document.content[1] as? Paragraph)
        #expect(before.document.scope(of: omegaBefore).start.line == 5)

        // Delete two of the blank lines: no node's content changes.
        try session.replace(6..<8, with: "")
        let after = try session.commit()
        let omegaAfter = try #require(after.document.content[1] as? Paragraph)

        #expect(after.delta.added.isEmpty)
        #expect(after.delta.removed.isEmpty)
        #expect(after.delta.changed.isEmpty)
        #expect(after.delta.bubbled.isEmpty)
        #expect(omegaAfter == omegaBefore)
        #expect(after.document.scope(of: omegaAfter).start.line == 3)
        #expect(after.document.dump() == (try Document.parse("Alpha\n\nOmega\n").dump()))
    }

    @Test("deep incremental rebuild consumes children before parents in one pass")
    func deepIncrementalRebuildOrder() throws {
        let depth = 512
        let stableSource = "Stable\n\n"
        let prefix = String(repeating: "> ", count: depth)
        let beforeSource = stableSource + prefix + "alpha\n"
        let afterSource = stableSource + prefix + "bravo\n"
        let session = try MarkupSession()
        try session.append(beforeSource)
        let before = try session.commit()
        let stable = try #require(before.document.content.first as? Paragraph)

        let leafStart = stableSource.utf8.count + prefix.utf8.count
        try session.replace(leafStart..<(leafStart + 5), with: "bravo")
        let after = try session.commit()

        #expect(after.delta.bubbled.count >= depth)
        #expect(after.document.content.first?.id == stable.id)
        #expect(after.document.dump() == (try Document.parse(afterSource).dump()))
    }

    @Test("materialized scopes survive the session and later commits")
    func scopeMaterializationOutlivesCurrency() throws {
        var session: MarkupSession? = try MarkupSession()
        try session?.append("One\n\nTwo\n")
        let first = try #require(try session?.commit())
        let two = try #require(first.document.content[1] as? Paragraph)
        // Materialize while current.
        #expect(first.document.scope(of: two).start.line == 3)

        try session?.replace(0..<0, with: "Zero\n\n")
        _ = try #require(try session?.commit())
        // The superseded snapshot answers from its cache, at its revision.
        #expect(first.document.scope(of: two).start.line == 3)

        session = nil
        #expect(first.document.scope(of: two).start.line == 3)
    }

    @Test("scope-free visitor traverses an unmaterialized superseded snapshot")
    func scopeFreeVisitorOutlivesCurrency() throws {
        let session = try MarkupSession()
        try session.append("- one\n- two\n")
        let retained = try session.commit().document

        try session.append("\nTail\n")
        _ = try session.commit()

        var visitor = CountingVisitor()
        MarkupWalker().walk(retained, visitor: &visitor)
        #expect(visitor.count == 8)
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
        let driver = try ConflationDriver()
        for turn in ConflationDriver.turns {
            try driver.stream(turn)
            try driver.settle()
        }
        try driver.finish()
    }

    @Test("session snapshots are Sendable values; ids are stable dictionary keys")
    func snapshotValueSemantics() throws {
        requireSendable(Commit.self)
        requireSendable(Delta.self)
        requireSendable(MarkupID.self)
        requireSendable(FootnoteInfo.self)
        let session = try MarkupSession()
        #expect(session.document.content.isEmpty)
        #expect(session.revision == 0)
        try session.append("Alpha\n")
        let commit = try session.commit()
        let paragraph = try #require(commit.document.content[0] as? Paragraph)
        var byID: [MarkupID: String] = [:]
        byID[paragraph.id] = "paragraph"
        #expect(session.node(for: paragraph.id)?.id == paragraph.id)
        #expect(byID[paragraph.id] == "paragraph")
    }

    @Test("directive labels stay first-class and relink through incremental commits")
    func directiveLabelRelinking() throws {
        let source = ":::note[*Title*]\nBody\n:::\n"
        let session = try MarkupSession()
        try session.append(source)
        let before = try session.commit()
        let blockBefore = try #require(before.document.content.first as? DirectiveBlock)
        let labelBefore = try #require(blockBefore.label)
        let bodyBefore = try #require(blockBefore.content.first)

        try session.replace(9..<14, with: "Other")
        let after = try session.commit()
        let blockAfter = try #require(after.document.content.first as? DirectiveBlock)
        let labelAfter = try #require(blockAfter.label)
        let emphasis = try #require(labelAfter.content.first as? Emphasis)
        let text = try #require(emphasis.content.first as? Text)

        #expect(text.literal == "Other")
        #expect(blockAfter.id == blockBefore.id)
        #expect(labelAfter.id == labelBefore.id)
        #expect(blockAfter.content.first?.id == bodyBefore.id)
        #expect(session.node(for: labelAfter.id) as? DirectiveLabel == labelAfter)
        #expect((after.delta.changed + after.delta.bubbled).contains(labelAfter.id))
    }
}

private struct CountingVisitor: MarkupVisitor {
    var count = 0

    private mutating func record(_: some Markup) { count += 1 }

    mutating func visit(_ node: Document) { record(node) }
    mutating func visit(_ node: BlockQuote) { record(node) }
    mutating func visit(_ node: Paragraph) { record(node) }
    mutating func visit(_ node: Heading) { record(node) }
    mutating func visit(_ node: ThematicBreak) { record(node) }
    mutating func visit(_ node: MarkdownCore.List) { record(node) }
    mutating func visit(_ node: ListItem) { record(node) }
    mutating func visit(_ node: CodeBlock) { record(node) }
    mutating func visit(_ node: HTMLBlock) { record(node) }
    mutating func visit(_ node: FormulaBlock) { record(node) }
    mutating func visit(_ node: Table) { record(node) }
    mutating func visit(_ node: TableRow) { record(node) }
    mutating func visit(_ node: TableCell) { record(node) }
    mutating func visit(_ node: DirectiveBlock) { record(node) }
    mutating func visit(_ node: DirectiveLabel) { record(node) }
    mutating func visit(_ node: FootnoteDefinition) { record(node) }
    mutating func visit(_ node: Text) { record(node) }
    mutating func visit(_ node: SoftBreak) { record(node) }
    mutating func visit(_ node: LineBreak) { record(node) }
    mutating func visit(_ node: Code) { record(node) }
    mutating func visit(_ node: HTML) { record(node) }
    mutating func visit(_ node: Formula) { record(node) }
    mutating func visit(_ node: Emphasis) { record(node) }
    mutating func visit(_ node: Strong) { record(node) }
    mutating func visit(_ node: Strikethrough) { record(node) }
    mutating func visit(_ node: Link) { record(node) }
    mutating func visit(_ node: Image) { record(node) }
    mutating func visit(_ node: Directive) { record(node) }
    mutating func visit(_ node: FootnoteReference) { record(node) }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

/// Drives one simulated LLM conversation against a session and asserts the
/// conflation contract at every render tick. One fixed generator drives
/// batch sizes and tick timing, so the burst shapes are irregular but
/// reproducible — and identical in the Kotlin and ES mirrors of this test.
private final class ConflationDriver {
    static let turns = [
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

    private struct Settled {
        let index: Int
        let id: MarkupID
        let revision: UInt64
    }

    private let session: MarkupSession
    private var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    private var streamed = ""
    private var frozen: [Settled] = []
    private var messages = 0
    private var commits = 0
    private var touched = 0

    init() throws {
        session = try MarkupSession()
    }

    /// Streams one turn as service-shaped messages: mostly a 20-30 token
    /// batch (80-150 characters), with occasional tiny flushes of a few
    /// words. Cuts land at raw character offsets — mid-word, mid-marker,
    /// even between the two newlines of a block boundary — because that is
    /// the steady state of LLM output.
    func stream(_ turn: String) throws {
        var pending = Array(turn)[...]
        while !pending.isEmpty {
            let width = draw(10) < 2 ? 2 + Int(draw(18)) : 80 + Int(draw(71))
            let message = String(pending.prefix(width))
            pending = pending.dropFirst(width)
            try session.append(message)
            streamed += message
            messages += 1
            if draw(4) == 0 { try tick() }
        }
    }

    /// The turn boundary always renders; everything but the still-hot last
    /// block is now settled and must stay frozen through later turns.
    func settle() throws {
        try tick()
        frozen = session.document.content.dropLast().enumerated().map {
            Settled(index: $0.offset, id: $0.element.id, revision: $0.element.revision)
        }
    }

    func finish() throws {
        #expect(messages > 9)
        #expect(commits < messages)
        #expect(session.document.dump() == (try Document.parse(Self.turns.joined()).dump()))

        // Near-O(n) pipeline: total delta traffic stays within one add per
        // final node plus bounded frontier churn per tick. A full rebuild
        // per tick would be on the order of commits * nodes.
        var nodes = 0
        MarkupWalker().walk(session.document) { event, _, _ in
            if event == .entering { nodes += 1 }
        }
        #expect(touched < nodes + 16 * commits)
    }

    private func draw(_ bound: UInt64) -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return (state >> 33) % bound
    }

    private func tick() throws {
        let commit = try session.commit()
        commits += 1
        touched +=
            commit.delta.added.count + commit.delta.removed.count
            + commit.delta.changed.count + commit.delta.bubbled.count
        #expect(commit.document.dump() == (try Document.parse(streamed).dump()))
        for entry in frozen {
            let node = commit.document.content[entry.index]
            #expect(node.id == entry.id)
            #expect(node.revision == entry.revision)
        }
    }
}

@Suite("depth") struct DepthSuite {
    /// Primitive results ferried out of the worker threads; access is
    /// sequenced by thread completion, never concurrent.
    private final class Outcome: @unchecked Sendable {
        var failure: String?
        var quoteEnters = 0
        var events = 0
        var dumpHasQuote = false
        var commitMatchesReference = false
    }

    @Test("adversarial nesting walks, dumps, and commits beyond the call-stack budget")
    func adversarialNestingDepth() throws {
        // 4096 nested quotes overflowed the recursive walker. Two explicit
        // stacks make the proof exact: deep value trees deallocate through
        // recursive ARC releases, so every deep document lives and dies on
        // a 16 MiB-stack thread, while the walk and dump run on a 512 KiB
        // stack that recursive traversal at this depth could not survive.
        let depth = 4096
        let outcome = Outcome()
        let finished = DispatchSemaphore(value: 0)
        let owner = Thread {
            defer { finished.signal() }
            do {
                let source = String(repeating: "> ", count: depth) + "leaf\n"
                let document = try Document.parse(source)

                let walked = DispatchSemaphore(value: 0)
                let walker = Thread {
                    defer { walked.signal() }
                    var quoteEnters = 0
                    var events = 0
                    MarkupWalker().walk(document) { event, node, _ in
                        events += 1
                        if event == .entering, node is BlockQuote { quoteEnters += 1 }
                    }
                    outcome.quoteEnters = quoteEnters
                    outcome.events = events
                    outcome.dumpHasQuote = document.dump().contains("BlockQuote")
                }
                walker.stackSize = 512 * 1024
                walker.start()
                walked.wait()

                let session = try MarkupSession()
                try session.append(source)
                _ = try session.commit()
                try session.replace((depth * 2)..<(depth * 2 + 4), with: "seed")
                let second = try session.commit()
                let reference = try Document.parse(String(repeating: "> ", count: depth) + "seed\n")
                outcome.commitMatchesReference = second.document.dump() == reference.dump()
            } catch {
                outcome.failure = String(describing: error)
            }
        }
        owner.stackSize = 16 * 1024 * 1024
        owner.start()
        finished.wait()

        #expect(outcome.failure == nil)
        #expect(outcome.quoteEnters == depth)
        // Every node enters exactly once and exits exactly once: the
        // document, the quote chain, and the innermost paragraph and text.
        #expect(outcome.events == 2 * (depth + 3))
        #expect(outcome.dumpHasQuote)
        #expect(outcome.commitMatchesReference)
    }
}

@Suite("materialization") struct MaterializationSuite {
    @Test("an explicitly materialized snapshot stays usable across commits and deinit")
    func explicitMaterialization() throws {
        var retained: Document?
        do {
            let session = try MarkupSession()
            try session.append("First\n\nSecond\n")
            let first = try session.commit()
            // The explicit contract: materialize while current, stay
            // self-contained forever after.
            first.document.materialize()
            try session.append("\nThird\n")
            _ = try session.commit()
            retained = first.document
        }
        let document = try #require(retained)
        let second = try #require(document.content[1] as? Paragraph)
        #expect(document.scope(of: second).start.line == 3)
        #expect(document.dump().contains("Paragraph"))
    }
}
