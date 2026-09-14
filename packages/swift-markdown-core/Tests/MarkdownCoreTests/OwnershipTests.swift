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
        weak var storage: MarkupStore?
        do {
            let document = try Document.parse(String(repeating: "- ", count: depth) + "leaf\n")
            storage = document.fields.store
            withExtendedLifetime(document) { #expect(document.content.count == 1) }
        }
        #expect(storage == nil)

        var subtree: MarkdownCore.List?
        do {
            let document = try Document.parse(String(repeating: "- ", count: depth) + "leaf\n")
            storage = document.fields.store
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

    @Test("deep dumps consume walker callbacks without recursive node visits")
    func deepDump() throws {
        let depth = 512
        let document = try Document.parse(String(repeating: "- ", count: depth) + "leaf\n")
        let lines = document.dump().split(separator: "\n")
        #expect(lines.count == depth * 2 + 3)
        #expect(lines.first?.hasPrefix("Document ") == true)
        #expect(lines.last?.contains("literal=\"leaf\"") == true)
        #expect(lines.last?.hasPrefix(String(repeating: "    ", count: depth * 2 + 1) + "└── ") == true)
    }

    @Test("retained body groups and individual bodies own the store through their last release")
    func retainedGroups() async throws {
        requireSendable(MarkupGroups<any Markup>.self)
        weak var storage: MarkupStore?
        var groups: MarkupGroups<any Markup>?
        var body: MarkupCollection<any Markup>?
        do {
            let document = try Document.parse("T\n: one\n:\n")
            storage = document.fields.store
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

    @Test("typed store references distinguish occurrences and documents after root release")
    func storedReferences() throws {
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
        #expect(first.scope.start.line == 1 && second.scope.start.line == 3 && other.scope.start.line == 1)
    }

    @Test("field views infer relation values while preserving optional scalar values")
    func inferredFields() throws {
        let document = try Document.parse("---\ntitle: Example\n---\n3. item\n")
        let metadata = document.fields.metadata
        let content = document.fields.content
        let attributes = document.fields.attributes
        #expect(metadata?.title == .scalar(.text("Example")))
        #expect(attributes == .empty)
        let list = try #require(content.first as? MarkdownCore.List)
        let start = list.fields.start
        #expect(start == 3)

        let plain = try Document.parse("> body\n")
        let absent = plain.fields.metadata
        #expect(absent == nil)
        let quote = try #require(plain.content.first as? Callout)
        let missingTitle = quote.fields.title
        #expect(missingTitle == nil)

        let titled = try Document.parse("> [!note] Title\n> body\n")
        let callout = try #require(titled.content.first as? Callout)
        let title = callout.fields.title
        #expect((title?.first as? Text)?.literal == "Title")

        let definitions = try Document.parse("Term\n: body\n:\n")
        let definitionList = try #require(definitions.content.first as? DefinitionList)
        let groups = definitionList.definitions[0].fields.content
        #expect(groups.count == 2 && groups[1].isEmpty)
        #expect(((groups[0].first as? Paragraph)?.content.first as? Text)?.literal == "body")
    }

    @Test("every view fits the existential inline buffer without copying its fields")
    func inlineViews() {
        let inlineCapacity = 3 * MemoryLayout<Int>.size
        #expect(MemoryLayout<Document>.size <= inlineCapacity)
        #expect(MemoryLayout<Paragraph>.size <= inlineCapacity)
        #expect(MemoryLayout<TableCaption>.size <= inlineCapacity)
        // Leaves are views too: a `Text` in a content collection is never boxed.
        #expect(MemoryLayout<Text>.size <= inlineCapacity)
        #expect(MemoryLayout<CodeBlock>.size <= inlineCapacity)
        #expect(MemoryLayout<SoftBreak>.size <= inlineCapacity)
        #expect(MemoryLayout<CrossEmbedded>.size <= inlineCapacity)
        #expect(MemoryLayout<MarkupReference<Metadata>>.size == MemoryLayout<Int>.size)
        #expect(MemoryLayout<MarkupReferences<any Markup>>.size == MemoryLayout<[Int]>.size)
        #expect(MemoryLayout<MarkupGroupReferences<any Markup>>.size == MemoryLayout<[[Int]]>.size)
    }
}

private func requireSendable<T: Sendable>(_: T.Type) {}
