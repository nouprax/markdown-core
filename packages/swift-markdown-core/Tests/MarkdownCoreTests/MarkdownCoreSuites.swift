import MarkdownCoreC
import Testing

// `@testable` for one reason, and it is stated at the use below: `ParseError`
// cannot be reached through the public surface, because no input a Swift caller
// can hand `Document` is invalid. Everything else here goes through the
// published API.
@testable import MarkdownCore

@Suite("api") struct APISuite {
    @Test("parse options and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let options = ParseOptions()
        #expect(options.tables && options.directives && options.formulas)
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.content[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        #expect(table.header.accept(&visitor) == "header")
        #expect(table.header.cells[0].accept(&visitor) == "cell")
        #expect(
            try Document.parse("| a |\n| --- |\n| b |\n", options: ParseOptions(tables: false))
                .content.first is Paragraph
        )
    }
}

@Suite("unicode") struct UnicodeSuite {
    @Test("UTF-8 survives the C-to-Swift boundary")
    func unicode() throws {
        let paragraph = try #require(Document.parse("héllo 🚀 中文\n").content.first as? Paragraph)
        #expect((paragraph.content.first as? Text)?.literal == "héllo 🚀 中文")
    }
}

@Suite("errors") struct ErrorsSuite {
    @Test("a native error crosses into Swift with its code and message, and nil still answers")
    func parseErrorFromNative() throws {
        // THE ONE `@testable` USE. No `String` a caller can hand `Document` is
        // invalid, so this initializer is unreachable through the published
        // surface -- but the C entry point rejects a null source with a real
        // error object, which is the only way to watch a native code and
        // message actually cross.
        var native: OpaquePointer?
        #expect(markdown_core_document_parse(nil, 1, nil, &native) == nil)
        let error = try #require(native)
        defer { markdown_core_error_free(error) }
        let crossed = ParseError(from: error)
        #expect(crossed.code == .invalidArgument)
        #expect(crossed.message.contains("must not be null"))

        // And the other arm: a loss the engine could not allocate an error for
        // still has to answer with something.
        let fallback = ParseError(from: nil)
        #expect(fallback.code == .internal)
        #expect(!fallback.message.isEmpty)
    }

    @Test("a written-but-empty destination is empty, not absent")
    func emptyDestinationIsEmpty() throws {
        // `[a]()` WROTE a destination and wrote nothing in it. The native side
        // answers that with a null pointer and length 0, which is the one place
        // a string with no bytes is still a string.
        let paragraph = try #require(Document.parse("[a]()\n").content.first as? Paragraph)
        let link = try #require(paragraph.content.first as? Link)
        // `destination` is not optional at all -- Q26 -- so empty is the only
        // way it can say "nothing was written between the parens"; `title` is,
        // and says absent instead.
        #expect(link.destination.isEmpty)
        #expect(link.title == nil)
    }

    @Test("empty input maps to an empty document")
    func empty() throws {
        #expect(try Document.parse("").content.isEmpty)
    }
}

@Suite("ownership") struct OwnershipSuite {
    @Test("values remain usable and Sendable after native release")
    func copiedAndSendable() async throws {
        requireSendable(Document.self)
        requireSendable(ParseOptions.self)
        let document = try Document.parse("parallel 🚀\n")
        let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { group in
            for _ in 0..<20 { group.addTask { document.content.count } }
            return await group.reduce(into: []) { $0.append($1) }
        }
        #expect(counts == Array(repeating: 1, count: 20))
    }

}

@Suite("api") struct DirectiveLabelSuite {
    @Test("a directive label is dumped as a field but is not content")
    func labelledDirectiveBlock() throws {
        let source = ":::note[Title]{kind=demo}\nBody\n:::\n"
        let block = try #require(Document.parse(source).content.first as? DirectiveBlock)
        let label = try #require(block.label)
        #expect((label.content.first as? Text)?.literal == "Title")
        #expect(block.content.count == 1)
        #expect(block.content.first is Paragraph)
        #expect(block.attributes?.first?.name == "kind")

        #expect(block.content.allSatisfy { !($0 is DirectiveLabel) })
        #expect(label.content.count == 1)
        #expect(label.content.first is Text)
        #expect(block.dump().contains("DirectiveLabel"))

        // The other field arm: no label is emitted when none was written.
        let bare = try #require(
            Document.parse(":::note\nBody\n:::\n").content.first as? DirectiveBlock
        )
        #expect(bare.label == nil)
        #expect(bare.dump().contains("children=1"))
    }

    @Test("the dump escapes every character JSON cannot carry literally")
    func dumpEscapes() throws {
        // The dumper's escape table has an arm per character and nothing in the
        // corpus writes most of them. A fenced code block carries its literal
        // through untouched, so it is the one place a test can put them all.
        let literal = "a\"b\\c\td\u{08}e\u{0c}f\u{01}g"
        let document = try Document.parse("```\n\(literal)\n```\n")
        let dump = document.dump()
        for expected in ["\\\"", "\\\\", "\\t", "\\b", "\\f", "\\n", "\\u0001"] {
            #expect(dump.contains(expected), "dump is missing the escape \(expected)")
        }
    }
}

@Suite("robustness") struct RobustnessSuite {
    @Test("large and deeply nested inputs preserve complete value trees")
    func workloads() throws {
        let unit = "## Section\n\nParagraph with **strong**, [link](/), and 🚀.\n\n"
        #expect(try Document.parse(String(repeating: unit, count: 5_000)).content.count == 10_000)
        var node = try #require(
            Document.parse(String(repeating: "> ", count: 128) + "leaf\n").content.first
        )
        for _ in 0..<128 { node = try #require((node as? BlockQuote)?.content.first) }
        #expect(node is Paragraph)
        for _ in 0..<2_000 { #expect(try Document.parse("# Copy\n\n- [x] item\n").content.count == 2) }
    }
}

private struct KindVisitor: MarkupVisitor {
    mutating func visit(_ node: Document) -> String { kindName(node) }
    mutating func visit(_ node: BlockQuote) -> String { kindName(node) }
    mutating func visit(_ node: Paragraph) -> String { kindName(node) }
    mutating func visit(_ node: Heading) -> String { "heading:\(node.level)" }
    mutating func visit(_ node: ThematicBreak) -> String { kindName(node) }
    mutating func visit(_ node: MarkdownCore.List) -> String { kindName(node) }
    mutating func visit(_ node: ListItem) -> String { kindName(node) }
    mutating func visit(_ node: CodeBlock) -> String { kindName(node) }
    mutating func visit(_ node: HTMLBlock) -> String { kindName(node) }
    mutating func visit(_ node: FormulaBlock) -> String { kindName(node) }
    mutating func visit(_ node: Table) -> String { kindName(node) }
    mutating func visit(_ node: DirectiveBlock) -> String { kindName(node) }

    mutating func visit(_ node: DirectiveLabel) -> String { kindName(node) }
    mutating func visit(_ node: FootnoteDefinition) -> String { kindName(node) }

    mutating func visit(_ node: ReferenceDefinition) -> String { kindName(node) }

    mutating func visit(_ node: LinkReference) -> String { kindName(node) }

    mutating func visit(_ node: ImageReference) -> String { kindName(node) }
    mutating func visit(_ node: Text) -> String { kindName(node) }
    mutating func visit(_ node: SoftBreak) -> String { kindName(node) }
    mutating func visit(_ node: LineBreak) -> String { kindName(node) }
    mutating func visit(_ node: Code) -> String { kindName(node) }
    mutating func visit(_ node: HTML) -> String { kindName(node) }
    mutating func visit(_ node: Formula) -> String { kindName(node) }
    mutating func visit(_ node: Emphasis) -> String { kindName(node) }
    mutating func visit(_ node: Strong) -> String { kindName(node) }
    mutating func visit(_ node: Strikethrough) -> String { kindName(node) }
    mutating func visit(_ node: Link) -> String { kindName(node) }
    mutating func visit(_ node: Image) -> String { kindName(node) }
    mutating func visit(_ node: Directive) -> String { kindName(node) }
    mutating func visit(_ node: FootnoteReference) -> String { kindName(node) }
    mutating func visit(_ node: TableRow) -> String { node.isHeader ? "header" : "row" }
    mutating func visit(_ node: TableCell) -> String { "cell" }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}
