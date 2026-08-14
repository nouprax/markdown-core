import Foundation
import MarkdownCore
import Testing

/// THE CHAIN CONTRACT through the Swift surface alone: both mutations
/// supersede their receiver, the revision line is strictly +1 per mutation
/// whichever kind, a stale receiver's mutation fails deterministically and
/// disturbs nothing, an empty append still advances the chain, and a
/// superseded document keeps answering every read from its decoded state.
///
/// The poison path — a failed append ends the chain — needs C-side
/// allocation injection and is gated there. Its error surface needs nothing
/// new here: `ParseError` carries whatever category and message the engine
/// reports, "the chain is done" included.
@Suite("chain") struct ChainSuite {
    @Test("a mutation supersedes its receiver; stale mutations fail deterministically and disturb nothing")
    func staleMutationDeterminism() throws {
        let head = try Document("alpha")
        let second = try head.append(" beta\n")
        #expect(second.series == head.series)
        #expect(second.revision == head.revision + 1)

        // The receiver is superseded: BOTH mutations on it now fail — one
        // rule, whichever kind — and deterministically: the same illegal
        // call yields the same verdict every time.
        let staleAppend = try #require(mutationError { try head.append("x") })
        #expect(staleAppend.code == .invalidArgument)
        let repeated = try #require(mutationError { try head.append("x") })
        #expect(repeated.code == staleAppend.code)
        #expect(repeated.message == staleAppend.message)
        let staleEdit = try #require(mutationError { try head.edit("y") })
        #expect(staleEdit.code == .invalidArgument)

        // A rejected argument fails the call, never the chain: the stale
        // receiver keeps answering every read from its decoded state, and
        // the live head still mutates — the revision line continuing +1
        // with an EDIT is the two mutations sharing one counter.
        #expect(head.dump().contains("Paragraph"))
        #expect(head.node(head.content[0].id)?.id == head.content[0].id)
        let third = try second.edit("# gamma\n")
        #expect(third.revision == head.revision + 2)
        #expect(third.content.first is Heading)
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
