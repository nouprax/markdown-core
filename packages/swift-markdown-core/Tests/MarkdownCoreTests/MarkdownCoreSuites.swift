import MarkdownCoreC
import Testing

// `@testable` covers native failure and reserved-value decoding paths that
// cannot yet be reached by parsing source. Other tests use the public API.
@testable import MarkdownCore

@Suite("api") struct APISuite {
    @Test("specimen definitions share citation ownership and preserve absent facts")
    func specimenValues() throws {
        let parsed = try Document.parse("body")
        let scope = parsed.scope
        // These reserved scalar combinations are deliberately constructed as
        // flat records; they need not depend on currently authored syntax.
        let tree = ValueTree(records: [
            .markupDocument(
                .init(
                    scope: scope,
                    anchor: nil,
                    attributes: .empty,
                    content: [1],
                    metadata: nil,
                    footnotes: [4],
                    specimens: [5, 6]
                )
            ),
            .markupParagraph(.init(scope: scope, anchor: nil, attributes: .empty, content: [2])),
            .markupCite(.init(scope: scope, anchor: nil, attributes: .empty, citations: [3])),
            .valueCitation(.init(scope: scope, referent: .specimen(id: "étude"), prefix: [], suffix: [])),
            .valueFootnote(.init(scope: scope, id: "n", content: [])),
            .valueSpecimen(.init(scope: scope, id: "étude", start: 5, content: [])),
            .valueSpecimen(.init(scope: scope, id: nil, start: nil, content: [])),
        ])
        let document = tree.value(at: 0, as: Document.self)
        #expect(document.specimens[0].start == 5)
        #expect(document.specimens[1].id == nil)
        #expect(document.dump().contains("referent=specimen(id=\"étude\")"))
        #expect(document.dump().contains("Specimen scope=1:1..1:4 id=null start=null children=0"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(visitor.events.filter { $0 == "entering:Specimen" }.count == 2)
        let footnoteExit = try #require(visitor.events.firstIndex(of: "exiting:Footnote"))
        let specimenEnter = try #require(visitor.events.firstIndex(of: "entering:Specimen"))
        #expect(footnoteExit < specimenEnter)
    }

    @Test("all native delimiter branches retain their authored value")
    func nativeListDelimiters() {
        let cases: [(markdown_core_ordered_list_delimiter, OrderedListDelimiter)] = [
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD, closed: false), .period),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS, closed: false), .parenthesis(closed: false)),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS, closed: true), .parenthesis(closed: true)),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT, closed: false), .default),
        ]
        for (value, expected) in cases {
            #expect(MarkdownCore.List.Fields.delimiter(value) == expected)
        }
    }

    @Test("parse and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.content[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        #expect(table.head[0].accept(&visitor) == "row")
        #expect(table.head[0].cells[0].accept(&visitor) == "cell")
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

    @Test("marks retain typed content and walk both phases after native release")
    func marks() throws {
        let paragraph = try #require(Document.parse("==a *b*==").content.first as? Paragraph)
        let mark = try #require(paragraph.content.first as? Mark)
        var visitor = RecordingWalkingVisitor()
        mark.walk(with: &visitor)
        #expect(
            visitor.events == [
                "entering:Mark", "entering:Text", "exiting:Text", "entering:Emphasis",
                "entering:Text", "exiting:Text", "exiting:Emphasis", "exiting:Mark",
            ]
        )
        #expect(mark.content.count == 2)
        #expect(((mark.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(mark.scope == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9)))
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
        #expect(tableVisitor.tableRowKinds == [1, 3])
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

    @Test("an ordinary quote is a metadata-free callout")
    func callout() throws {
        let document = try Document.parse("> quote\n")
        let callout = try #require(document.content.first as? Callout)
        #expect(callout.variant == nil)
        #expect(callout.collapsed == nil)
        #expect(callout.title == nil)
        #expect(callout.content.count == 1)
        #expect(
            document.dump()
                == "Document scope=1:1..1:7 anchor=null attributes={} children=1\n"
                + "└── Callout scope=1:1..1:7 anchor=null attributes={} variant=null collapsed=null children=1\n"
                + "    └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1\n"
                + "        └── Text scope=1:3..1:7 anchor=null attributes={} literal=\"quote\" children=0\n"
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
        let cites = document.content.compactMap { $0 as? Paragraph }.flatMap(\.content).compactMap { $0 as? Cite }
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
        #expect(dump.hasPrefix("Document scope=1:1..5:11 anchor=null attributes={} children=1\n"))
        let tail = """
            └── Footnote scope=5:1..5:11 id="a" children=1
                └── Paragraph scope=5:7..5:11 anchor=null attributes={} children=1
                    └── Text scope=5:7..5:11 anchor=null attributes={} literal="twice" children=0

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
        #expect(
            try Document.parse("").scope
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 0, column: 0))
        )
        #expect(
            try Document.parse("é").scope
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 2))
        )
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
        #expect(block.attributes.records.first?.name == "kind")

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
        let document = try Document.parse(String(repeating: "- ", count: 10_000) + "leaf\n")
        var walkingVisitor = RecordingWalkingVisitor(recordEvents: false)
        document.walk(with: &walkingVisitor)
        #expect(walkingVisitor.entered == walkingVisitor.exited)
        #expect(walkingVisitor.entered > 20_000)
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
    mutating func visit(_ node: MarkdownCore.CrossLink) -> String { kindName(node) }
    mutating func visit(_ node: MarkdownCore.CrossEmbedded) -> String { kindName(node) }
    mutating func visit(_ node: Formula) -> String { kindName(node) }
    mutating func visit(_ node: Emphasis) -> String { kindName(node) }
    mutating func visit(_ node: Strong) -> String { kindName(node) }
    mutating func visit(_ node: Strikethrough) -> String { kindName(node) }
    mutating func visit(_ node: Mark) -> String { kindName(node) }
    mutating func visit(_ node: Insertion) -> String { kindName(node) }
    mutating func visit(_ node: Span) -> String { kindName(node) }
    mutating func visit(_ node: Superscript) -> String { kindName(node) }
    mutating func visit(_ node: Subscript) -> String { kindName(node) }
    mutating func visit(_ node: DefinitionList) -> String { kindName(node) }
    mutating func visit(_ node: Definition) -> String { kindName(node) }
    mutating func visit(_ node: Link) -> String { kindName(node) }
    mutating func visit(_ node: Media) -> String { kindName(node) }
    mutating func visit(_ node: Directive) -> String { kindName(node) }
    mutating func visit(_ node: Cite) -> String { kindName(node) }
    mutating func visit(_ node: TableCaption) -> String { kindName(node) }
    mutating func visit(_ node: TableRow) -> String { "row" }
    mutating func visit(_ node: TableCell) -> String { "cell" }
}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}

struct RecordingWalkingVisitor: MarkupWalkingVisitor {
    private let recordEvents: Bool
    var events: [String] = []
    var tableRowKinds: [Int] = []
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
    mutating func visit(_ node: MarkdownCore.CrossLink, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.CrossEmbedded, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Formula, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Emphasis, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Strong, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Strikethrough, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Mark, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Insertion, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Span, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Superscript, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Subscript, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: DefinitionList, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Definition, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Link, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Media, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Directive, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: Cite, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: TableCaption, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ node: TableRow, phase: WalkPhase) {
        record(node, phase)
        if phase == .entering { tableRowKinds.append(Int(node.scope.start.line)) }
    }
    mutating func visit(_ node: TableCell, phase: WalkPhase) { record(node, phase) }
    mutating func visit(_ value: Citation, phase: WalkPhase) { record("Citation", phase) }
    mutating func visit(_ value: Footnote, phase: WalkPhase) { record("Footnote", phase) }
    mutating func visit(_ value: Specimen, phase: WalkPhase) { record("Specimen", phase) }
}
