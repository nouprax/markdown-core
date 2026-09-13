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

    @Test("deep trees and independently retained subtrees release with bounded stack", arguments: [30_000, 65_536])
    func deepRelease(depth: Int) throws {
        weak var storage: ValueTree?
        do {
            let document = try Document.parse(String(repeating: "- ", count: depth) + "leaf\n")
            storage = document.tree
            withExtendedLifetime(document) { #expect(document.content.count == 1) }
        }
        #expect(storage == nil)

        var subtree: MarkdownCore.List?
        do {
            let document = try Document.parse(String(repeating: "- ", count: depth) + "leaf\n")
            storage = document.tree
            subtree = try #require(document.content.first as? MarkdownCore.List)
        }
        #expect(storage != nil)
        do {
            var visitor = RecordingWalkingVisitor(recordEvents: false)
            try #require(subtree).walk(with: &visitor)
            #expect(visitor.entered == visitor.exited)
            #expect(visitor.entered == depth * 2 + 2)
        }
        subtree = nil
        #expect(storage == nil)
    }

    @Test("retained body groups and individual bodies own the store through their last release")
    func retainedGroups() async throws {
        requireSendable(MarkupGroups<any Markup>.self)
        weak var storage: ValueTree?
        var groups: MarkupGroups<any Markup>?
        var body: MarkupCollection<any Markup>?
        do {
            let document = try Document.parse("T\n: one\n:\n")
            storage = document.tree
            let list = try #require(document.content.first as? DefinitionList)
            groups = list.definitions[0].content
        }
        #expect(storage != nil)
        do {
            let retained = try #require(groups)
            let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { tasks in
                for _ in 0..<20 { tasks.addTask { retained[0].count + retained[1].count } }
                return await tasks.reduce(into: []) { $0.append($1) }
            }
            #expect(counts == Array(repeating: 1, count: 20))
            body = retained[0]
        }
        groups = nil
        #expect(storage != nil)
        #expect(((body?.first as? Paragraph)?.content.first as? Text)?.literal == "one")
        body = nil
        #expect(storage == nil)
    }

}

private func requireSendable<T: Sendable>(_: T.Type) {}
