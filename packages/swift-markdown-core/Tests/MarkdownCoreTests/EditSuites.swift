import Foundation
import MarkdownCore
import Testing

@Suite("edits") struct EditSuite {
    @Test("extending the trailing text keeps its identity and bumps its revision")
    func trailingExtensionIdentityStability() throws {
        let first = try Document("# Title\n\nHello")
        let firstHeading = try #require(first.content[0] as? Heading)
        let firstParagraph = try #require(first.content[1] as? Paragraph)
        let firstText = try #require(firstParagraph.content[0] as? Text)

        let commit = try first.edit("# Title\n\nHello world")
        let second = commit.document
        let secondHeading = try #require(second.content[0] as? Heading)
        let secondParagraph = try #require(second.content[1] as? Paragraph)
        let secondText = try #require(secondParagraph.content[0] as? Text)

        #expect(secondText.literal == "Hello world")
        #expect(secondParagraph.id == firstParagraph.id)
        #expect(secondText.id == firstText.id)
        #expect(secondText.revision > firstText.revision)
        #expect(secondHeading == firstHeading)

        // The heading settled before the edit and is not in the delta at all;
        // the text it precedes is, and nothing is retired.
        let named = Set(commit.delta.diffs.map(\.markup))
        #expect(!named.contains(firstHeading.id))
        #expect(named.contains(secondText.id))
        #expect(commit.delta.diffs.allSatisfy { !$0.parts.isRetired })
        #expect(try #require(commit.delta.diffs.first { $0.markup == secondText.id }).parts.contains(.text))
    }

    @Test("a clean-boundary insert at the top leaves downstream identity intact")
    func cleanBoundaryInsert() throws {
        let before = try Document("First\n\nSecond\n\nThird\n")
        let downstreamBefore = before.content.map { ($0.id, $0.revision) }
        let thirdBefore = try #require(before.content[2] as? Paragraph)

        let commit = try before.edit("# New\n\nFirst\n\nSecond\n\nThird\n")
        let after = commit.document

        #expect(after.content.count == 4)
        let inserted = try #require(after.content[0] as? Heading)
        for (index, node) in after.content.dropFirst().enumerated() {
            #expect(node.id == downstreamBefore[index].0)
            #expect(node.revision == downstreamBefore[index].1)
        }

        // A created node differs from absence in every part it has: a heading
        // has a value and children, and no text of its own.
        let insertedDiff = try #require(commit.delta.diffs.first { $0.markup == inserted.id })
        #expect(insertedDiff.parts == [.value, .children, .descendant])

        // Downstream nodes shifted by two lines. The successor's value is the
        // one that moved; the predecessor's value keeps the extent it was
        // parsed with, because a document is an immutable projection of one
        // text and a node in it does not move.
        let third = try #require(after.content[3] as? Paragraph)
        #expect(third.scope.start.line == 7)
        #expect(thirdBefore.scope.start.line == 5)
        // And they are still EQUAL: identity survived the edit and position is
        // not part of equality, which is exactly what lets an edit above a
        // node leave every reactive comparison below it untouched.
        #expect(third == thirdBefore)
        #expect(after.dump() == (try Document("# New\n\nFirst\n\nSecond\n\nThird\n").dump()))
    }

    @Test("a kind change retires the old identity and mints a new one")
    func kindChangeRetiresIdentity() throws {
        let before = try Document("text\n")
        let paragraph = try #require(before.content[0] as? Paragraph)

        let commit = try before.edit("# text\n")
        let heading = try #require(commit.document.content[0] as? Heading)

        #expect(heading.id != paragraph.id)
        let retired = commit.delta.diffs.filter(\.parts.isRetired).map(\.markup)
        #expect(retired.contains(paragraph.id))
        #expect(commit.delta.diffs.contains { $0.markup == heading.id && !$0.parts.isRetired })
        // A retired node is emitted WHERE IT WAS FOUND — inside its former
        // parent's run, before the nodes that took its place there and before
        // that parent's own row — so a consumer drops a parent's dead children
        // before it re-reads that parent's child list. (It is NOT true that
        // every retirement precedes every survivor: a sibling that merely
        // changed is emitted before the removal that follows it.)
        let retiredIndex = try #require(commit.delta.diffs.firstIndex { $0.markup == paragraph.id })
        let createdIndex = try #require(commit.delta.diffs.firstIndex { $0.markup == heading.id })
        let rootIndex = try #require(commit.delta.diffs.firstIndex { $0.markup == commit.document.id })
        #expect(retiredIndex < createdIndex)
        #expect(createdIndex < rootIndex)
    }

    @Test("equality is series-salted identity plus revision")
    func equalitySemantics() throws {
        let source = "Same *content* twice.\n"
        let first = try Document(source)
        let second = try Document(source)
        // Identical content from different parses never compares equal.
        #expect(first != second)
        #expect(first.content[0] as? Paragraph != second.content[0] as? Paragraph)
        // Within one document, identity is value equality.
        #expect(first.content[0] as? Paragraph == first.content[0] as? Paragraph)
        #expect(first.id.series != second.id.series)
        // An id from another series is not this document's to answer.
        #expect(first.node(second.content[0].id) == nil)

        // `series` reads the salt straight off the document, and it is the
        // WHOLE lineage's: an edit inherits it, which is what makes an id
        // minted before the edit still name something after it.
        #expect(first.series == first.id.series)
        let edited = try first.edit("Same *content* twice, extended.\n")
        #expect(edited.document.series == first.series)
        #expect(edited.document.id.series == first.id.series)

        // Hashable agrees with ==, which is what a document held as a
        // dictionary key or in a Set depends on.
        #expect(first.hashValue == first.hashValue)
        var byDocument: [Document: String] = [first: "first", second: "second"]
        #expect(byDocument.count == 2)
        #expect(byDocument[first] == "first")
        byDocument[first] = "replaced"
        #expect(byDocument.count == 2)
        #expect(byDocument[first] == "replaced")
        #expect(Set([first, first, second]).count == 2)
    }

    @Test("a blank-line-only edit reports an empty delta yet shifts scopes")
    func pureShiftReportsEmptyDelta() throws {
        let before = try Document("Alpha\n\n\n\nOmega\n")
        let omegaBefore = try #require(before.content[1] as? Paragraph)
        #expect(omegaBefore.scope.start.line == 5)

        // Delete two of the blank lines: no node's content changes.
        let commit = try before.edit("Alpha\n\nOmega\n")
        let after = commit.document
        let omegaAfter = try #require(after.content[1] as? Paragraph)

        #expect(commit.delta.diffs.isEmpty)
        #expect(commit.delta.afterRevision > commit.delta.beforeRevision)
        // Equal, and at a different place: two nodes differing only in where
        // they sit are equal, because position is not content.
        #expect(omegaAfter == omegaBefore)
        #expect(omegaAfter.scope.start.line == 3)
        #expect(omegaBefore.scope.start.line == 5)
        #expect(after.dump() == (try Document("Alpha\n\nOmega\n").dump()))
    }

    @Test("a deep rebuild names children before parents in one postorder pass")
    func deepRebuildOrder() throws {
        let depth = 512
        let stable = "Stable\n\n"
        let prefix = String(repeating: "> ", count: depth)
        let before = try Document(stable + prefix + "alpha\n")
        let stableBefore = try #require(before.content.first as? Paragraph)

        let commit = try before.edit(stable + prefix + "bravo\n")
        let after = commit.document

        // The quote chain is untouched material whose descendant changed, so
        // every one of its nodes is named — and named after what it contains.
        #expect(commit.delta.diffs.count >= depth)
        expectPostorder(commit.delta, of: after)
        #expect(after.content.first?.id == stableBefore.id)
        #expect(after.dump() == (try Document(stable + prefix + "bravo\n").dump()))

        // Exactly one node's own projection differs — the innermost text —
        // and every one of its ancestors carries `descendant` and nothing
        // else, which is what lets a renderer stop at the first node whose
        // own parts are empty.
        var innermost: any Markup = try #require(after.content.last)
        while let quote = innermost as? BlockQuote {
            innermost = try #require(quote.content.first)
        }
        let text = try #require((innermost as? Paragraph)?.content.first as? Text)
        #expect(text.literal == "bravo")
        let ownChanges = commit.delta.diffs.filter { $0.parts != .descendant }
        #expect(ownChanges.map(\.markup) == [text.id])
        #expect(ownChanges.first?.parts == .text)
        // The chain, the paragraph it ends in, and the document above it.
        #expect(commit.delta.diffs.count == depth + 3)
    }

    @Test("a superseded document keeps answering from its own tables")
    func supersededDocumentStaysWhole() throws {
        var retained: Document?
        var two: Paragraph?
        do {
            let first = try Document("One\n\nTwo\n")
            two = try #require(first.content[1] as? Paragraph)
            #expect(try #require(two).scope.start.line == 3)

            // Editing hands the native parse to the successor, which then
            // goes out of scope and frees it. The predecessor's values,
            // scopes, and dump were all extracted at parse time and owe that
            // parse nothing.
            _ = try first.edit("Zero\n\nOne\n\nTwo\n")
            retained = first
        }
        let document = try #require(retained)
        #expect(try #require(two).scope.start.line == 3)
        #expect(document.dump().contains("Paragraph"))
        var visitor = CountingVisitor()
        MarkupWalker().walk(document, visitor: &visitor)
        #expect(visitor.count == 5)
    }

    @Test("streaming: irregular render ticks over a multi-turn conversation")
    func conflatedStreaming() throws {
        // The shape of a real LLM consumer: every socket message extends the
        // text (nothing parses), only an irregular render tick edits, and the
        // messages between ticks conflate into that one edit. Three assistant
        // turns extend one document; blocks settled at a turn boundary must
        // stay frozen while later turns stream.
        let driver = try StreamDriver()
        for turn in StreamDriver.turns {
            try driver.stream(turn)
            try driver.settle()
        }
        try driver.finish()
    }

    @Test("documents and deltas are Sendable values; ids are stable dictionary keys")
    func valueSemantics() throws {
        requireSendable(Document.self)
        requireSendable(Commit.self)
        requireSendable(Delta.self)
        requireSendable(Diff.self)
        requireSendable(MarkupID.self)
        requireSendable(Diagnostic.self)

        let empty = try Document("")
        #expect(empty.content.isEmpty)
        let commit = try empty.edit("Alpha\n")
        let document = commit.document
        let paragraph = try #require(document.content[0] as? Paragraph)
        var byID: [MarkupID: String] = [:]
        byID[paragraph.id] = "paragraph"
        #expect(document.node(paragraph.id)?.id == paragraph.id)
        // The root answers for itself, which is what a delta naming it needs.
        #expect(document.node(document.id)?.id == document.id)
        #expect(byID[paragraph.id] == "paragraph")
    }

    @Test("directive labels stay first-class and relink through an edit")
    func directiveLabelRelinking() throws {
        let before = try Document(":::note[*Title*]\nBody\n:::\n")
        let blockBefore = try #require(before.content.first as? DirectiveBlock)
        let labelBefore = try #require(blockBefore.label)
        let bodyBefore = try #require(blockBefore.content.first)

        let after = try before.edit(":::note[*Other*]\nBody\n:::\n").document
        let blockAfter = try #require(after.content.first as? DirectiveBlock)
        let labelAfter = try #require(blockAfter.label)
        let emphasis = try #require(labelAfter.content.first as? Emphasis)
        let text = try #require(emphasis.content.first as? Text)

        #expect(text.literal == "Other")
        #expect(blockAfter.id == blockBefore.id)
        #expect(labelAfter.id == labelBefore.id)
        #expect(blockAfter.content.first?.id == bodyBefore.id)
        #expect(after.node(labelAfter.id) as? DirectiveLabel == labelAfter)
    }

    @Test("diagnostics travel with the document that raised them")
    func diagnosticsFollowTheDocument() throws {
        // The one thing an editor underlines: a directive's `{...}` did not
        // parse, so the braces stayed literal text. Invisible in the tree —
        // the node simply has no attributes — which is why it is reported.
        let before = try Document(":::note{= bad}\nbody\n:::\n")
        #expect(before.diagnostics.map(\.code) == [.directiveAttributes])
        #expect(
            before.diagnostics.first?.scope
                == Scope(start: Position(line: 1, column: 8), end: Position(line: 1, column: 14))
        )

        let after = try before.edit(":::note{a=1}\nbody\n:::\n").document
        #expect(after.diagnostics.isEmpty)
        // The predecessor keeps reporting its own: diagnostics are values it
        // copied out at parse time, like every other part of it.
        #expect(before.diagnostics.count == 1)
    }
}

/// Asserts the delta's own ordering contract: every surviving node appears
/// after all of its own children, so a consumer materializing immutable
/// values bottom-up reads the list once, front to back.
private func expectPostorder(
    _ delta: Delta,
    of document: Document,
    sourceLocation: SourceLocation = #_sourceLocation
) {
    var position: [MarkupID: Int] = [:]
    for (index, diff) in delta.diffs.enumerated() where !diff.parts.isRetired {
        position[diff.markup] = index
    }
    var stack: [MarkupID] = []
    MarkupWalker().walk(document) { event, node, _ in
        switch event {
        case .entering:
            if let parent = stack.last, let above = position[parent], let below = position[node.id] {
                #expect(below < above, sourceLocation: sourceLocation)
            }
            stack.append(node.id)
        case .exiting:
            stack.removeLast()
        }
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
    mutating func visit(_ node: ReferenceDefinition) { record(node) }
    mutating func visit(_ node: LinkReference) { record(node) }
    mutating func visit(_ node: ImageReference) { record(node) }
    mutating func visit(_ node: CrossLink) { record(node) }
    mutating func visit(_ node: Embed) { record(node) }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

/// Drives one simulated LLM conversation and asserts the streaming contract
/// at every render tick. One fixed generator drives batch sizes and tick
/// timing, so the burst shapes are irregular but reproducible — and identical
/// in the Kotlin and ES mirrors of this test.
private final class StreamDriver {
    static let turns = [
        "# Streaming\n\nThe *quick* parser holds **steady** under bursts, "
            + "and the heading keeps its identity from the first render on.\n\n"
            + "Deltas stay proportional to what changed, so a renderer "
            + "reconciles by id instead of walking the whole tree.\n\n"
            + "> Snapshots are values: whatever a tick captured stays valid "
            + "while the socket races ahead.",
        "\n\n- append per message\n- edit per tick\n- settled blocks stay frozen"
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

    private var document: Document
    private var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    private var streamed = ""
    private var frozen: [Settled] = []
    private var messages = 0
    private var ticks = 0
    private var touched = 0

    init() throws {
        // The empty document is a parse like any other; there is no separate
        // "open a session" step.
        document = try Document("")
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
            streamed += String(pending.prefix(width))
            pending = pending.dropFirst(width)
            messages += 1
            if draw(4) == 0 { try tick() }
        }
    }

    /// The turn boundary always renders; everything but the still-hot last
    /// block is now settled and must stay frozen through later turns.
    func settle() throws {
        try tick()
        frozen = document.content.dropLast().enumerated().map {
            Settled(index: $0.offset, id: $0.element.id, revision: $0.element.revision)
        }
    }

    func finish() throws {
        #expect(messages > 9)
        #expect(ticks < messages)
        #expect(document.dump() == (try Document(Self.turns.joined()).dump()))

        // Near-O(n) pipeline: total delta traffic stays within one row per
        // final node plus bounded frontier churn per tick. A full rebuild per
        // tick would be on the order of ticks * nodes.
        var nodes = 0
        MarkupWalker().walk(document) { event, _, _ in
            if event == .entering { nodes += 1 }
        }
        #expect(touched < nodes + 16 * ticks)
    }

    private func draw(_ bound: UInt64) -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return (state >> 33) % bound
    }

    private func tick() throws {
        let commit = try document.edit(streamed)
        document = commit.document
        ticks += 1
        touched += commit.delta.diffs.count
        #expect(document.dump() == (try Document(streamed).dump()))
        expectPostorder(commit.delta, of: document)
        for entry in frozen {
            let node = document.content[entry.index]
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
        var editMatchesReference = false
    }

    @Test("adversarial nesting walks, dumps, and edits beyond the call-stack budget")
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
                let prefix = String(repeating: "> ", count: depth)
                let document = try Document(prefix + "leaf\n")

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

                let edited = try Document(prefix + "leaf\n").edit(prefix + "seed\n").document
                let reference = try Document(prefix + "seed\n")
                outcome.editMatchesReference = edited.dump() == reference.dump()
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
        #expect(outcome.editMatchesReference)
    }
}
