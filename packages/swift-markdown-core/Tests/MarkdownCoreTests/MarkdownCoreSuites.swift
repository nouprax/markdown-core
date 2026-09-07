import MarkdownCoreC
import Testing

// `@testable` for one reason, and it is stated at the use below: `ParseError`
// cannot be reached through the public surface, because no input a Swift caller
// can hand `Document` is invalid. Everything else here goes through the
// published API.
@testable import MarkdownCore

@Suite("api") struct APISuite {
    @Test("parse and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.content[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        #expect(table.header.accept(&visitor) == "header")
        #expect(table.header.cells[0].accept(&visitor) == "cell")
    }

    @Test("the dialect has no switches: every feature is recognised by a plain parse")
    func wholeDialect() throws {
        // One witness per feature that used to sit behind a `ParseOptions`
        // field, and one for the substitution smart punctuation used to make.
        #expect(try Document.parse("| a |\n| --- |\n| b |\n").content.first is Table)
        #expect(try Document.parse("~~x~~\n").dump().contains("Strikethrough scope="))
        #expect(try Document.parse("www.example.com\n").dump().contains("Link scope="))
        #expect(try Document.parse("- [x] task\n").dump().contains("marker=\"x\""))
        #expect(try Document.parse("ref[^a]\n\n[^a]: note\n").dump().contains("Cite scope="))
        #expect(try Document.parse("$x$\n").dump().contains("Formula scope="))
        #expect(try Document.parse(":badge[label]\n").dump().contains("Directive scope="))
        #expect(try Document.parse("\"quotes\" -- ...\n").dump().contains("literal=\"\\\"quotes\\\" -- ...\""))
    }

    @Test("walking dispatch is typed and preserves owned-field semantics")
    func walkingVisitor() throws {
        let block = try #require(
            Document.parse(":::note[Title]\nBody\n:::\n").content.first as? DirectiveBlock
        )
        var visitor = RecordingWalkingVisitor()
        block.walk(with: &visitor)

        #expect(
            visitor.events == [
                "entering:DirectiveBlock",
                "entering:DirectiveLabel",
                "entering:Text",
                "exiting:Text",
                "exiting:DirectiveLabel",
                "entering:Paragraph",
                "entering:Text",
                "exiting:Text",
                "exiting:Paragraph",
                "exiting:DirectiveBlock",
            ]
        )
        #expect(block.content.count == 1)
        #expect(block.content.allSatisfy { !($0 is DirectiveLabel) })

        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        var tableVisitor = RecordingWalkingVisitor()
        table.walk(with: &tableVisitor)
        #expect(tableVisitor.tableRowKinds == [true, false])
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
        #expect(markdown_core_document_parse(nil, 1, &native) == nil)
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
        // `dest` is not optional at all -- Q26 -- so the `url` branch holding
        // the empty string is the only way it can say "nothing was written
        // between the parens"; `title` is, and says absent instead.
        #expect(link.dest == .url(""))
        #expect(link.title == nil)
    }

    @Test("every occurrence of one reference definition materializes one resource")
    func sharedResource() throws {
        // M2: the C tree shares one resource across every occurrence of a
        // definition, and the Swift tree decodes it once. The destination is
        // long enough to live in heap storage, so two Strings that share it
        // report one buffer and two independent decodes would report two.
        let destination = "/" + String(repeating: "u", count: 1024)
        let count = 5_000
        let document = try Document.parse("[a]: \(destination)\n\n" + String(repeating: "[a]\n\n", count: count))
        let links = try document.content.map { try #require(($0 as? Paragraph)?.content.first as? Link) }
        #expect(links.count == count)
        guard case .url(var first) = links[0].dest else {
            Issue.record("a resolved reference is the url branch")
            return
        }
        #expect(first == destination)
        let storage = first.withUTF8 { UnsafeRawPointer($0.baseAddress!) }
        for link in links.dropFirst() {
            guard case .url(var url) = link.dest else {
                Issue.record("a resolved reference is the url branch")
                return
            }
            #expect(url.withUTF8 { UnsafeRawPointer($0.baseAddress!) } == storage)
        }
    }

    @Test("every `>` container is a metadata-free callout")
    func callout() throws {
        // M3: the kind is `Callout`; the metadata rule that fills variant,
        // collapsed, and title in lands with O8, so every callout reads as
        // metadata-free and dumps its fields as such.
        let document = try Document.parse("> quote\n")
        let callout = try #require(document.content.first as? Callout)
        #expect(callout.variant == nil)
        #expect(callout.collapsed == nil)
        #expect(callout.title == nil)
        #expect(callout.content.count == 1)
        #expect(
            document.dump()
                == "Document scope=1:1..1:7 children=1\n"
                + "└── Callout scope=1:1..1:7 variant=null collapsed=null children=1\n"
                + "    └── Paragraph scope=1:3..1:7 children=1\n"
                + "        └── Text scope=1:3..1:7 literal=\"quote\" children=0\n"
        )
    }

    @Test("citations are values and the document owns its footnotes")
    func citations() throws {
        // M4: an inherited call is a one-item cite naming its footnote by id
        // with empty affixes; the footnote is a value the document owns, never
        // content, and the walk reaches it after the content. Repeated calls
        // share one footnote: the first definition of an id is the one they
        // resolve to, and a later definition of the same id is a footnote
        // after it, as the inherited grammar parses it.
        let document = try Document.parse("[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n")
        let cites = document.content.flatMap { ($0 as? Paragraph)?.content ?? [] }.compactMap { $0 as? Cite }
        #expect(document.content.count == 1)
        #expect(cites.count == 2)
        for cite in cites {
            let citation = try #require(cite.citations.first)
            #expect(cite.citations.count == 1)
            #expect(citation.referent == .footnote(id: "a"))
            #expect(citation.prefix.isEmpty && citation.suffix.isEmpty)
        }
        let footnote = try #require(document.footnotes.first)
        let later = try #require(document.footnotes.last)
        #expect(document.footnotes.map(\.id) == ["a", "a"])
        #expect(footnote.scope == Scope(start: Position(line: 3, column: 1), end: Position(line: 4, column: 0)))
        #expect(((footnote.content.first as? Paragraph)?.content.first as? Text)?.literal == "once")
        #expect(later.scope == Scope(start: Position(line: 5, column: 1), end: Position(line: 5, column: 11)))
        #expect(((later.content.first as? Paragraph)?.content.first as? Text)?.literal == "twice")
        let dump = document.dump()
        #expect(dump.hasPrefix("Document scope=1:1..5:11 children=1\n"))
        let tail = """
            └── Footnote scope=5:1..5:11 id="a" children=1
                └── Paragraph scope=5:7..5:11 children=1
                    └── Text scope=5:7..5:11 literal="twice" children=0

            """
        #expect(dump.hasSuffix(tail))

        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        let expected = [
            "entering:Document", "entering:Paragraph",
            "entering:Cite", "entering:Citation", "exiting:Citation", "exiting:Cite",
            "entering:Text", "exiting:Text",
            "entering:Cite", "entering:Citation", "exiting:Citation", "exiting:Cite",
            "exiting:Paragraph",
            "entering:Footnote", "entering:Paragraph", "entering:Text", "exiting:Text", "exiting:Paragraph",
            "exiting:Footnote",
            "entering:Footnote", "entering:Paragraph", "entering:Text", "exiting:Text", "exiting:Paragraph",
            "exiting:Footnote", "exiting:Document",
        ]
        #expect(visitor.events == expected)
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
        let depth = 10_000
        var document: Document? = try Document.parse(
            String(repeating: "- ", count: depth) + "leaf\n"
        )
        var walkingVisitor = RecordingWalkingVisitor(recordEvents: false)
        document?.walk(with: &walkingVisitor)
        #expect(walkingVisitor.entered == walkingVisitor.exited)
        #expect(walkingVisitor.entered > depth * 2)

        // Keep the next value alive while releasing each ancestor. Swift's
        // nested value-tree destruction is otherwise recursive independently
        // of the walk implementation being exercised here.
        var node = try #require(document?.content.first)
        document = nil
        for _ in 0..<depth {
            let list = try #require(node as? MarkdownCore.List)
            node = try #require(list.items.first?.content.first)
        }
        #expect(node is Paragraph)
        for _ in 0..<2_000 { #expect(try Document.parse("# Copy\n\n- [x] item\n").content.count == 2) }
    }
}

private struct KindVisitor: MarkupVisitor {
    mutating func visit(_ node: Document) -> String { kindName(node) }
    mutating func visit(_ node: Callout) -> String { kindName(node) }
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
    mutating func visit(_ node: Text) -> String { kindName(node) }
    mutating func visit(_ node: SoftBreak) -> String { kindName(node) }
    mutating func visit(_ node: LineBreak) -> String { kindName(node) }
    mutating func visit(_ node: Code) -> String { kindName(node) }
    mutating func visit(_ node: HTML) -> String { kindName(node) }
    mutating func visit(_ node: MarkdownCore.Comment) -> String { kindName(node) }
    mutating func visit(_ node: Formula) -> String { kindName(node) }
    mutating func visit(_ node: Emphasis) -> String { kindName(node) }
    mutating func visit(_ node: Strong) -> String { kindName(node) }
    mutating func visit(_ node: Strikethrough) -> String { kindName(node) }
    mutating func visit(_ node: Link) -> String { kindName(node) }
    mutating func visit(_ node: Image) -> String { kindName(node) }
    mutating func visit(_ node: Directive) -> String { kindName(node) }
    mutating func visit(_ node: Cite) -> String { kindName(node) }
    mutating func visit(_ node: TableRow) -> String { node.isHeader ? "header" : "row" }
    mutating func visit(_ node: TableCell) -> String { "cell" }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}

private struct RecordingWalkingVisitor: MarkupWalkingVisitor {
    private let recordEvents: Bool
    var events: [String] = []
    var tableRowKinds: [Bool] = []
    var entered = 0
    var exited = 0

    init(recordEvents: Bool = true) {
        self.recordEvents = recordEvents
    }

    private mutating func record(_ node: any Markup, _ phase: WalkPhase) {
        record(kindName(node), phase)
    }

    private mutating func record(_ name: String, _ phase: WalkPhase) {
        switch phase {
        case .entering: entered += 1
        case .exiting: exited += 1
        }
        if recordEvents { events.append("\(phase):\(name)") }
    }

    mutating func visit(_ node: Document, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Callout, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Paragraph, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Heading, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: ThematicBreak, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.List, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: ListItem, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: CodeBlock, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: HTMLBlock, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: FormulaBlock, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Table, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveBlock, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveLabel, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Text, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: SoftBreak, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: LineBreak, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Code, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: HTML, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.Comment, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Formula, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Emphasis, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Strong, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Strikethrough, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Link, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Image, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Directive, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Cite, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: TableRow, phase: WalkPhase) {
        record(node, phase)
        if phase == .entering { tableRowKinds.append(node.isHeader) }
    }
    mutating func visit(_ node: TableCell, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ value: Citation, phase: WalkPhase) { record("Citation", phase) }
    mutating func visit(_ value: Footnote, phase: WalkPhase) { record("Footnote", phase) }
}
