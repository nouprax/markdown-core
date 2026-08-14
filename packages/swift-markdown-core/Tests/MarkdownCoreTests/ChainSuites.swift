import Foundation
import MarkdownCore
import Testing

/// THE CHAIN CONTRACT through the Swift surface alone: a document chain
/// grows one way — append — and every append supersedes its receiver, the
/// revision line is strictly +1 per append, a stale receiver's append fails
/// deterministically and disturbs nothing, an empty append still advances
/// the chain, and a superseded document keeps answering every read from its
/// decoded state. Replacing the text is a new `Document`: a new chain, a
/// new series.
///
/// The poison path — a failed append ends the chain — needs C-side
/// allocation injection and is gated there. Its error surface needs nothing
/// new here: `ParseError` carries whatever category and message the engine
/// reports, "the chain is done" included.
@Suite("chain") struct ChainSuite {
    @Test("an append supersedes its receiver; stale appends fail deterministically and disturb nothing")
    func staleMutationDeterminism() throws {
        let head = try Document("alpha")
        let second = try head.append(" beta\n")
        #expect(second.series == head.series)
        #expect(second.revision == head.revision + 1)

        // The receiver is superseded: appending to it now fails, and
        // deterministically: the same illegal call yields the same verdict
        // every time.
        let staleAppend = try #require(mutationError { try head.append("x") })
        #expect(staleAppend.code == .invalidArgument)
        let repeated = try #require(mutationError { try head.append("x") })
        #expect(repeated.code == staleAppend.code)
        #expect(repeated.message == staleAppend.message)

        // A rejected argument fails the call, never the chain: the stale
        // receiver keeps answering every read from its decoded state, and
        // the live head still appends — the revision line continuing +1 on
        // the chain's one counter.
        #expect(head.dump().contains("Paragraph"))
        #expect(head.node(head.content[0].id)?.id == head.content[0].id)
        let third = try second.append("\n# gamma\n")
        #expect(third.revision == head.revision + 2)
        #expect(third.content.last is Heading)
    }

    @Test("an empty append advances the chain over an identical projection")
    func emptyAppendIsARealMutation() throws {
        let base = try Document("alpha")
        let idle = try base.append("")

        // Identical projection: same root (id, revision) — that is what
        // `==` compares — same (id, revision) rows node for node, same
        // dump byte for byte.
        #expect(idle == base)
        #expect(track(idle) == track(base))
        #expect(idle.dump() == base.dump())

        // Yet a real mutation: the receiver is superseded, not skipped.
        #expect(try #require(mutationError { try base.append("!") }).code == .invalidArgument)

        // And the chain's counter advanced underneath the unchanged
        // projection: the next change is stamped TWO past the fresh parse.
        // Were the empty append a no-op, this root would carry +1.
        let grown = try idle.append(" beta\n")
        #expect(grown.revision == base.revision + 2)
        let paragraph = try #require(grown.content.first as? Paragraph)
        #expect((paragraph.content.first as? Text)?.literal == "alpha beta")
    }

    @Test("a superseded receiver's values, lookups, and dump outlive the whole chain")
    func supersededReadsOutliveTheChain() throws {
        var retained: Document?
        var early: Paragraph?
        do {
            let head = try Document("One\n\nTwo")
            early = head.content.first as? Paragraph
            let next = try head.append("\n\nThree\n")
            _ = try next.append("Four\n")
            retained = head
        }
        // Every successor is gone; the superseded receiver still answers
        // from the values it decoded when it was built — reads never touch
        // the native parse again, so there is nothing left to invalidate.
        let head = try #require(retained)
        #expect(head.content.count == 2)
        #expect(head.dump().contains("Paragraph"))
        let paragraph = try #require(early)
        #expect(head.node(paragraph.id)?.revision == paragraph.revision)
        #expect(paragraph.scope.start == Position(line: 1, column: 1))
        // The visitor-walk overload reads the same decoded state: every
        // node dispatches exactly once, in preorder.
        var visitor = CountingVisitor()
        MarkupWalker().walk(head, visitor: &visitor)
        #expect(visitor.count == 5)
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
        // WHOLE chain's: an append inherits it, which is what makes an id
        // minted before the append still name something after it.
        #expect(first.series == first.id.series)
        let grown = try first.append("And more.\n")
        #expect(grown.series == first.series)
        #expect(grown.id.series == first.id.series)

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

    @Test("documents are Sendable values; ids are stable dictionary keys")
    func valueSemantics() throws {
        // `Document: Sendable` rests on the chain's threading contract:
        // mutations are exclusive and serialized by the caller, concurrent
        // reads between mutations never touch the native parse, and decoded
        // values are pure — safe on any thread, forever.
        requireSendable(Document.self)
        requireSendable(MarkupID.self)
        requireSendable(Diagnostic.self)

        let empty = try Document("")
        #expect(empty.content.isEmpty)
        let document = try empty.append("Alpha\n")
        let paragraph = try #require(document.content[0] as? Paragraph)
        var byID: [MarkupID: String] = [:]
        byID[paragraph.id] = "paragraph"
        #expect(document.node(paragraph.id)?.id == paragraph.id)
        // The root answers for itself, like any node reconciled by id.
        #expect(document.node(document.id)?.id == document.id)
        #expect(byID[paragraph.id] == "paragraph")
    }

    @Test("diagnostics travel with the document that raised them")
    func diagnosticsFollowTheDocument() throws {
        // The one thing an editor underlines: a directive's `{...}` did not
        // parse, so the braces stayed literal text. Invisible in the tree —
        // the node simply has no attributes — which is why it is reported.
        let before = try Document("lead\n\n")
        #expect(before.diagnostics.isEmpty)

        let after = try before.append(":::note{= bad}\nbody\n:::\n")
        #expect(after.diagnostics.map(\.code) == [.directiveAttributes])
        #expect(
            after.diagnostics.first?.scope
                == Scope(start: Position(line: 3, column: 8), end: Position(line: 3, column: 14))
        )
        // The predecessor keeps reporting its own — none — because
        // diagnostics are values it copied out at parse time, like every
        // other part of it.
        #expect(before.diagnostics.isEmpty)
    }

    @Test("streaming: every message rides the real append; render ticks read the head")
    func streamedAppends() throws {
        // The shape of a real LLM consumer: every socket message is one
        // append — `document = document.append(chunk)`, no self-held
        // accumulated text resent — and an irregular render tick only READS
        // the head it is handed. Three assistant turns extend one document;
        // blocks settled at a turn boundary must stay frozen while later
        // turns stream.
        let driver = try StreamDriver()
        for turn in StreamDriver.turns {
            try driver.stream(turn)
            try driver.settle()
        }
        try driver.finish()
    }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

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

/// Drives one simulated LLM conversation over the real append mutation and
/// asserts the streaming contract at every render tick. One fixed generator
/// drives batch sizes and tick timing, so the burst shapes are irregular but
/// reproducible — and identical in the Kotlin and ES mirrors of this test.
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
    /// words. Every message is one real append — the burst shape IS the
    /// mutation now — and cuts land at raw character offsets: mid-word,
    /// mid-marker, even between the two newlines of a block boundary,
    /// because that is the steady state of LLM output.
    func stream(_ turn: String) throws {
        var pending = Array(turn)[...]
        while !pending.isEmpty {
            let width = draw(10) < 2 ? 2 + Int(draw(18)) : 80 + Int(draw(71))
            let chunk = String(pending.prefix(width))
            pending = pending.dropFirst(width)
            streamed += chunk
            messages += 1
            let next = try document.append(chunk)
            // The traffic this append caused: every node minted or changed
            // here is stamped with the new document revision, and any change
            // bubbles to the root — so when the root's revision moved, the
            // nodes carrying it are exactly this mutation's traffic, and
            // when it did not, the append touched nothing.
            if next.revision != document.revision {
                MarkupWalker().walk(next) { event, node, _ in
                    if event == .entering, node.revision == next.revision { touched += 1 }
                }
            }
            document = next
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

        // Near-O(n) pipeline: total revision traffic — per append, the
        // nodes stamped with that mutation's new document revision — stays
        // within one stamp per final node plus bounded frontier churn per
        // message. A full restamp per message would be on the order of
        // messages * nodes.
        var nodes = 0
        MarkupWalker().walk(document) { event, _, _ in
            if event == .entering { nodes += 1 }
        }
        #expect(touched < nodes + 16 * messages)
    }

    private func draw(_ bound: UInt64) -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return (state >> 33) % bound
    }

    /// A render tick READS the head it already holds — the appends between
    /// ticks were the mutations — and asserts the contract: the head dumps
    /// identically to a one-shot parse of every byte so far, and blocks
    /// settled at a turn boundary stayed frozen through later appends.
    private func tick() throws {
        ticks += 1
        #expect(document.dump() == (try Document(streamed).dump()))
        for entry in frozen {
            let node = document.content[entry.index]
            #expect(node.id == entry.id)
            #expect(node.revision == entry.revision)
        }
    }
}

/// Runs one mutation and hands back the `ParseError` it threw, nil when it
/// succeeded — the shape every determinism assertion above needs.
private func mutationError(_ mutation: () throws -> Document) -> ParseError? {
    do {
        _ = try mutation()
        return nil
    } catch let error as ParseError {
        return error
    } catch {
        return nil
    }
}

/// The document's (id, revision) pairs in preorder — the projection's
/// identity content, positions excluded.
private func track(_ document: Document) -> [[UInt64]] {
    var rows: [[UInt64]] = []
    MarkupWalker().walk(document) { event, node, _ in
        if event == .entering { rows.append([node.id.rawValue, node.revision]) }
    }
    return rows
}
