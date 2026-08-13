import Foundation
import Testing

// `@testable` for one reason, stated at its use: the gate needs a second,
// prune-free decode of the SAME native tree (`reprojectedContent`), which
// the public surface deliberately does not offer.
@testable import MarkdownCore

/// The value-mirror gate. On the append path the binding does not re-decode
/// the whole tree: a subtree whose (id, revision) the predecessor already
/// decoded is reused whole. This gate is the only oracle that can see a
/// reuse defect — the dump oracle checks the native tree the values came
/// from, and `==` is (id, revision), exactly the equivalence pruning
/// trusts, so exactly the one this comparison must not use. So after EVERY
/// append, the pruned decode is compared field by field — kinds, typed
/// fields, literals, scopes, identity rows, child structure, recursively —
/// against an independent full decode of the same native tree.
@Suite("value mirror") struct ValueMirrorSuite {
    @Test("after every append, the pruned decode equals a full decode of the same native tree")
    func prunedDecodeMatchesFullDecode() throws {
        let manifest = try loadManifest()
        for testCase in manifest.cases {
            var document = try Document("", options: testCase.parseOptions.value)
            for chunk in burstChunks(testCase.source) {
                document = try document.append(chunk)
                #expect(
                    sameForest(document.content, document.reprojectedContent()),
                    Comment(rawValue: testCase.name)
                )
            }
            // The chunks partition the source, so the streamed document must
            // land exactly on the case's expected canonical tree.
            #expect(document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }
}

/// The StreamDriver's burst shape scaled to fixture-sized sources: the same
/// fixed generator, mostly a several-word batch with occasional one-to-four
/// character flushes, cut at raw character offsets — mid-word, mid-marker,
/// mid-construct — so the chunking is irregular but reproducible.
private func burstChunks(_ source: String) -> [String] {
    var chunks: [String] = []
    var state: UInt64 = 0x9E37_79B9_7F4A_7C15
    func draw(_ bound: UInt64) -> UInt64 {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return (state >> 33) % bound
    }
    var pending = Array(source)[...]
    while !pending.isEmpty {
        let width = draw(10) < 2 ? 1 + Int(draw(4)) : 5 + Int(draw(25))
        chunks.append(String(pending.prefix(width)))
        pending = pending.dropFirst(width)
    }
    return chunks
}

private func sameForest(_ lhs: [any Markup], _ rhs: [any Markup]) -> Bool {
    lhs.count == rhs.count && zip(lhs, rhs).allSatisfy { sameNode($0, $1) }
}

private func sameLabel(_ lhs: DirectiveLabel?, _ rhs: DirectiveLabel?) -> Bool {
    switch (lhs, rhs) {
    case (nil, nil): true
    case let (lhs?, rhs?): sameNode(lhs, rhs)
    default: false
    }
}

// Deep structural equality: everything a consumer can read off the two
// values, compared by VALUE, never by `==`. The identity row (id, revision)
// is compared as CONTENT here — the full decode read it off the native
// tree, so a reused value carrying a stale row fails — and then every typed
// field, literal, scope, and child, recursively. A kind mismatch, and any
// kind this switch does not know, is a loud failure. One case per kind is
// the point, so the complexity limits stand aside.
// swiftlint:disable:next cyclomatic_complexity function_body_length
private func sameNode(_ lhs: any Markup, _ rhs: any Markup) -> Bool {
    guard lhs.id == rhs.id, lhs.revision == rhs.revision, lhs.scope == rhs.scope else { return false }
    switch (lhs, rhs) {
    case let (lhs as BlockQuote, rhs as BlockQuote):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as Paragraph, rhs as Paragraph):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as Heading, rhs as Heading):
        return lhs.level == rhs.level && sameForest(lhs.content, rhs.content)
    case (is ThematicBreak, is ThematicBreak):
        return true
    case let (lhs as MarkdownCore.List, rhs as MarkdownCore.List):
        return lhs.flavor == rhs.flavor && lhs.start == rhs.start && lhs.tight == rhs.tight
            && sameForest(lhs.items, rhs.items)
    case let (lhs as ListItem, rhs as ListItem):
        return lhs.checked == rhs.checked && sameForest(lhs.content, rhs.content)
    case let (lhs as CodeBlock, rhs as CodeBlock):
        return lhs.mode == rhs.mode && lhs.info == rhs.info && lhs.language == rhs.language
            && lhs.literal == rhs.literal && lhs.fenced == rhs.fenced && lhs.closed == rhs.closed
    case let (lhs as HTMLBlock, rhs as HTMLBlock):
        return lhs.comment == rhs.comment && lhs.literal == rhs.literal
    case let (lhs as FormulaBlock, rhs as FormulaBlock):
        return lhs.mode == rhs.mode && lhs.literal == rhs.literal
    case let (lhs as Table, rhs as Table):
        return lhs.alignments == rhs.alignments && sameNode(lhs.header, rhs.header)
            && sameForest(lhs.rows, rhs.rows)
    case let (lhs as TableRow, rhs as TableRow):
        return lhs.isHeader == rhs.isHeader && sameForest(lhs.cells, rhs.cells)
    case let (lhs as TableCell, rhs as TableCell):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as DirectiveBlock, rhs as DirectiveBlock):
        return lhs.mode == rhs.mode && lhs.name == rhs.name && lhs.attributes == rhs.attributes
            && sameLabel(lhs.label, rhs.label) && sameForest(lhs.content, rhs.content)
    case let (lhs as DirectiveLabel, rhs as DirectiveLabel):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as FootnoteDefinition, rhs as FootnoteDefinition):
        return lhs.label == rhs.label && sameForest(lhs.content, rhs.content)
    case let (lhs as Text, rhs as Text):
        return lhs.literal == rhs.literal
    case (is SoftBreak, is SoftBreak):
        return true
    case (is LineBreak, is LineBreak):
        return true
    case let (lhs as Code, rhs as Code):
        return lhs.mode == rhs.mode && lhs.literal == rhs.literal
    case let (lhs as HTML, rhs as HTML):
        return lhs.comment == rhs.comment && lhs.literal == rhs.literal
    case let (lhs as Formula, rhs as Formula):
        return lhs.mode == rhs.mode && lhs.literal == rhs.literal
    case let (lhs as Emphasis, rhs as Emphasis):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as Strong, rhs as Strong):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as Strikethrough, rhs as Strikethrough):
        return sameForest(lhs.content, rhs.content)
    case let (lhs as Link, rhs as Link):
        return lhs.destination == rhs.destination && lhs.title == rhs.title
            && sameForest(lhs.content, rhs.content)
    case let (lhs as Image, rhs as Image):
        return lhs.source == rhs.source && lhs.title == rhs.title
            && sameForest(lhs.content, rhs.content)
    case let (lhs as Directive, rhs as Directive):
        return lhs.mode == rhs.mode && lhs.name == rhs.name && lhs.attributes == rhs.attributes
            && sameLabel(lhs.label, rhs.label)
    case let (lhs as FootnoteReference, rhs as FootnoteReference):
        return lhs.label == rhs.label
    case let (lhs as ReferenceDefinition, rhs as ReferenceDefinition):
        return lhs.label == rhs.label && lhs.destination == rhs.destination && lhs.title == rhs.title
    case let (lhs as LinkReference, rhs as LinkReference):
        return lhs.label == rhs.label && lhs.form == rhs.form && sameForest(lhs.content, rhs.content)
    case let (lhs as ImageReference, rhs as ImageReference):
        return lhs.label == rhs.label && lhs.form == rhs.form && sameForest(lhs.content, rhs.content)
    case let (lhs as CrossLink, rhs as CrossLink):
        return lhs.reference == rhs.reference
    case let (lhs as Embed, rhs as Embed):
        return lhs.reference == rhs.reference
    default:
        return false
    }
}
