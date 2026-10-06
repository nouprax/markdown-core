import Foundation
import Testing

@testable import MarkdownCore

@Suite("ownership") struct OwnershipSuite {
    @Test("values remain usable and Sendable after native release")
    func copiedAndSendable() async throws {
        requireSendable(Document.self)
        let document = try Document.parse("parallel 🚀\n")
        let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { group in
            for _ in 0..<20 { group.addTask { document.content.count } }
            return await group.reduce(into: []) { $0.append($1) }
        }
        #expect(counts == Array(repeating: 1, count: 20))
    }

    @Test("deep trees release, compare, walk, locate and describe on a small stack", arguments: [30_000, 65_536])
    func deepTrees(depth: Int) throws {
        let failures = try onSmallStack { deepTreeFailures(depth: depth) }
        #expect(failures.isEmpty, "\(failures)")
    }

    @Test("deep session documents release, compare, walk, locate and describe", arguments: [30_000, 65_536])
    func deepSessionTrees(depth: Int) throws {
        let failures = try onSmallStack { deepSessionFailures(depth: depth) }
        #expect(failures.isEmpty, "\(failures)")
    }

    @Test("deep dumps consume walker callbacks without recursive node visits")
    func deepDump() throws {
        let depth = 512
        let lines = try dumped(String(repeating: "- ", count: depth) + "leaf\n").split(separator: "\n")
        #expect(lines.count == depth * 2 + 3)
        #expect(lines.first?.hasPrefix("Document ") == true)
        #expect(lines.last?.contains("literal=\"leaf\"") == true)
        #expect(lines.last?.hasPrefix(String(repeating: "    ", count: depth * 2 + 1) + "└── ") == true)
    }

    @Test("retained body groups and individual bodies own their records through their last release")
    func retainedGroups() async throws {
        requireSendable(MarkupGroups<any Markup>.self)
        weak var root: MarkupRecord?
        weak var owner: MarkupRecord?
        weak var leaf: MarkupRecord?
        var groups: MarkupGroups<any Markup>?
        var body: MarkupCollection<any Markup>?
        do {
            let document = try Document.parse("T\n: one\n:\n")
            root = document.record
            let list = try #require(document.content.first as? DefinitionList)
            groups = list.definitions[0].content
            owner = list.definitions[0].record
        }
        #expect(root == nil)
        #expect(owner != nil)
        do {
            let retained = try #require(groups)
            let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { tasks in
                for _ in 0..<20 { tasks.addTask { retained[0].count + retained[1].count } }
                return await tasks.reduce(into: []) { $0.append($1) }
            }
            #expect(counts == Array(repeating: 1, count: 20))
            body = retained[0]
            leaf = body?.records.first
        }
        // A body shares its own records, not its owner's.
        groups = nil
        #expect(owner == nil)
        #expect(leaf != nil)
        #expect(((body?.first as? Paragraph)?.content.first as? Text)?.literal == "one")
        body = nil
        #expect(leaf == nil)
    }

    @Test("nodes of released documents keep their own values and ids")
    func releasedDocuments() throws {
        let (first, second) = try {
            let document = try Document.parse("first\n\nsecond\n")
            return (
                try #require(document.content[0] as? Paragraph),
                try #require(document.content[1] as? Paragraph)
            )
        }()
        let other = try #require(Document.parse("other\n").content.first as? Paragraph)
        #expect((first.content.first as? Text)?.literal == "first")
        #expect((second.content.first as? Text)?.literal == "second")
        #expect((other.content.first as? Text)?.literal == "other")
        #expect(first.id == MarkupID(2) && second.id == MarkupID(4) && other.id == MarkupID(2))
        #expect(second.extent == Extent(lead: 2, span: 6))
        // Ids from different documents are not comparable; equality still is.
        #expect(first != other)
    }

    @Test("equal documents are equal, and a difference at the deepest leaf is not")
    func deepEquality() throws {
        let source = "> - *a **b [c](/d)***\n>\n> | x |\n> | - |\n> | y |\n"
        let document = try Document.parse(source)
        let again = try Document.parse(source)
        #expect(document == again)
        #expect(document.hashValue == again.hashValue)
        #expect(document.isEqual(again))
        #expect(Set([document, again]).count == 1)
        let changed = try Document.parse(source.replacingOccurrences(of: "y", with: "z"))
        #expect(document != changed)
        #expect(!document.isEqual(changed))
        // Hashing reads the id alone, so the unequal documents still collide.
        #expect(document.hashValue == changed.hashValue)
        // Kinds differ, so existentials are unequal even at one id.
        let paragraph = try #require(Document.parse("x").content.first)
        let heading = try #require(Document.parse("# x").content.first)
        #expect(paragraph.id == heading.id)
        #expect(!paragraph.isEqual(heading))
        #expect(paragraph.isEqual(paragraph))
    }

    @Test("a node describes its kind and id without reading its descendants")
    func describes() throws {
        let document = try Document.parse("# x\n")
        #expect(document.description == "Document(id=3)")
        #expect(String(describing: document.content[0]) == "Heading(id=1)")
    }
}

/// Every deep-tree operation of the plan's gate, as failure messages: the
/// thread it runs on has no test context to record issues in.
private func deepTreeFailures(depth: Int) -> [String] {
    var failures: [String] = []
    func check(_ condition: Bool, _ message: String) {
        if !condition { failures.append(message) }
    }
    let source = String(repeating: "- ", count: depth) + "leaf\n"
    guard let document = try? Document.parse(source), let changed = try? Document.parse(source + "x\n"),
        let lead = try? Document.parse(String(repeating: "- ", count: depth) + "lead\n")
    else { return ["parse failed"] }

    // Walking: a list and an item per level, then the document, paragraph and text.
    var visitor = RecordingWalkingVisitor(recordEvents: false)
    document.walk(with: &visitor)
    check(visitor.entered == depth * 2 + 3 && visitor.exited == visitor.entered, "walk count")

    // The deepest paragraph, reached with a loop.
    var deepest: Paragraph?
    var list = document.content.first as? MarkdownCore.List
    while let item = list?.items.first {
        list = item.content.first as? MarkdownCore.List
        if list == nil { deepest = item.content.first as? Paragraph }
    }
    guard let leaf = deepest?.content.first as? Text else { return failures + ["no leaf"] }

    // Equality at the deepest leaf: same ids and extents, one literal apart.
    check(document == (try? Document.parse(source)), "equal deep documents")
    check(document != lead, "deepest leaf difference")
    check(document != changed, "trailing difference")

    // Scope lookup and hit testing, whose walks are as deep as the tree.
    let column = Int32(depth * 2 + 1)
    let scope = try? document.scope(of: leaf, in: source)
    check(
        scope == [Scope(start: Position(line: 1, column: column), end: Position(line: 1, column: column + 3))],
        "leaf scope"
    )
    check(
        (try? document.node(at: Position(line: 1, column: column + 1), in: source)?.isEqual(leaf)) == true,
        "hit test"
    )
    check(document.description == "Document(id=\(depth * 2 + 3))", "description")
    check(leaf.description == "Text(id=\(depth * 2 + 2))", "leaf description")
    return failures + deepReleaseFailures(source: source)
}

/// The deep documents of a session (plan gates 4.9): the edit at the deepest
/// leaf publishes a document that compares, walks, locates and describes like
/// a fresh one, and the previous document is released while the new one is
/// alive.
private func deepSessionFailures(depth: Int) -> [String] {
    var failures: [String] = []
    func check(_ condition: Bool, _ message: String) {
        if !condition { failures.append(message) }
    }
    guard let session = try? MarkdownSession(String(repeating: "- ", count: depth) + "leaf\n") else {
        return ["session failed"]
    }
    weak var previous: MarkupRecord?
    var edited: Document?
    do {
        let first = session.document
        previous = first.record
        edited = try? session.edit([TextEdit(depth * 2..<depth * 2 + 4, with: "lean")])
        check(edited != nil && edited != first, "deepest leaf difference")
        check(edited?.content.first?.id == first.content.first?.id, "continued id")
    }
    check(previous == nil, "previous document released")
    guard let document = edited else { return failures }

    var visitor = RecordingWalkingVisitor(recordEvents: false)
    document.walk(with: &visitor)
    check(visitor.entered == depth * 2 + 3 && visitor.exited == visitor.entered, "walk count")
    var deepest: Paragraph?
    var list = document.content.first as? MarkdownCore.List
    while let item = list?.items.first {
        list = item.content.first as? MarkdownCore.List
        if list == nil { deepest = item.content.first as? Paragraph }
    }
    guard let leaf = deepest?.content.first as? Text else { return failures + ["no leaf"] }
    check(leaf.literal == "lean", "edited leaf")
    let text = session.text
    let column = Int32(depth * 2 + 1)
    let scope = try? document.scope(of: leaf, in: text)
    check(
        scope == [Scope(start: Position(line: 1, column: column), end: Position(line: 1, column: column + 3))],
        "leaf scope"
    )
    check(
        (try? document.node(at: Position(line: 1, column: column), in: text)?.isEqual(leaf)) == true,
        "hit test"
    )
    check(document.description == "Document(id=\(depth * 2 + 3))", "description")
    return failures
}

/// Release of deep documents and of the deep subtrees views hold past them.
private func deepReleaseFailures(source: String) -> [String] {
    var failures: [String] = []
    func check(_ condition: Bool, _ message: String) {
        if !condition { failures.append(message) }
    }

    // Release while a view holds a subtree: the root goes, the paragraph stays.
    weak var root: MarkupRecord?
    weak var held: MarkupRecord?
    var retained: Paragraph?
    do {
        guard let copy = try? Document.parse(source) else { return failures + ["parse failed"] }
        root = copy.record
        var list = copy.content.first as? MarkdownCore.List
        while let item = list?.items.first {
            list = item.content.first as? MarkdownCore.List
            if list == nil { retained = item.content.first as? Paragraph }
        }
        held = retained?.record
    }
    check(root == nil, "deep document released")
    check((retained?.content.first as? Text)?.literal == "leaf", "held subtree readable")
    retained = nil
    check(held == nil, "held subtree released")

    // Release of a deep subtree a view held past its document.
    var top: MarkdownCore.List?
    do {
        guard let copy = try? Document.parse(source) else { return failures + ["parse failed"] }
        top = copy.content.first as? MarkdownCore.List
        held = top?.record
    }
    check(held != nil, "held list alive")
    top = nil
    check(held == nil, "held list released")
    return failures
}

/// Runs `body` on a thread with a small fixed stack, so recursion over tree
/// depth fails the same way on every platform.
private func onSmallStack<Value: Sendable>(_ body: @escaping @Sendable () -> Value) throws -> Value {
    let outcome = Outcome<Value>()
    let thread = Thread {
        outcome.value = body()
        outcome.done.signal()
    }
    thread.stackSize = 1 << 19
    thread.start()
    outcome.done.wait()
    return try #require(outcome.value)
}

/// The value a thread hands back; the semaphore orders its one write before
/// the read.
private final class Outcome<Value>: @unchecked Sendable {
    let done = DispatchSemaphore(value: 0)
    var value: Value?
}

private func requireSendable<T: Sendable>(_: T.Type) {}
