import MarkdownCoreC
import Testing

// `@testable` covers reserved-value decoding paths that cannot yet be reached
// by parsing source. Other tests use the public API.
@testable import MarkdownCore

@Suite("api") struct APISuite {
    @Test("specimen definitions are content the document lists, and absent facts stay absent")
    func specimenValues() throws {
        // These reserved scalar combinations are deliberately constructed as
        // records; they need not depend on currently authored syntax.
        let citation = CitationRecord(
            fields(4),
            referent: .specimen(label: "étude"),
            note: nil,
            prefix: [],
            suffix: []
        )
        let paragraph = ParagraphRecord(fields(2), children: [CiteRecord(fields(3), children: [citation])])
        let footnote = FootnoteRecord(fields(5), label: "n", content: [])
        let named = SpecimenRecord(fields(6), label: "étude", start: 5, content: [])
        let anonymous = SpecimenRecord(fields(7), label: nil, start: nil, content: [])
        let document = Document(
            record: DocumentRecord(
                fields(1),
                unit: .utf16,
                metadata: nil,
                content: [paragraph, footnote, named, anonymous],
                footnotes: [footnote],
                specimens: [named, anonymous],
                references: [],
                referenceLabels: [:]
            )
        )
        #expect(document.specimens[0].start == 5)
        #expect(document.specimens[1].label == nil)
        #expect(document.specimen(for: "étude")?.id == MarkupID(6))
        // A label matches byte for byte: the decomposed spelling is another label.
        #expect(document.specimen(for: "e\u{301}tude") == nil)
        #expect(document.footnote(for: "n")?.id == MarkupID(5))
        let dump = try document.dump(in: "")
        #expect(dump.contains("referent=specimen(label=\"étude\")"))
        #expect(dump.contains("Specimen scope=1:1..1:0 anchor=null attributes={} label=null start=null children=0"))
        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        #expect(visitor.events.filter { $0 == "enter:Specimen" }.count == 2)
        let footnoteExit = try #require(visitor.events.firstIndex(of: "exit:Footnote"))
        let specimenEnter = try #require(visitor.events.firstIndex(of: "enter:Specimen"))
        #expect(footnoteExit < specimenEnter)
    }

    @Test("all native delimiter branches retain their authored value")
    func listDelimiters() {
        let cases: [(markdown_core_ordered_list_delimiter, OrderedListDelimiter)] = [
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PERIOD, closed: false), .period),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS, closed: false), .parenthesis(closed: false)),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_PARENTHESIS, closed: true), .parenthesis(closed: true)),
            (.init(kind: MARKDOWN_CORE_ORDERED_LIST_DELIMITER_DEFAULT, closed: false), .default),
        ]
        for (value, expected) in cases {
            #expect(ListRecord.delimiter(value) == expected)
        }
    }

    @Test("typed callbacks collect results through the public walk API")
    func publicAPI() throws {
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        document.content[0].walk(with: &visitor)
        #expect(visitor.kinds == ["heading:1", "Text"])
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        visitor = KindVisitor()
        table.head[0].walk(with: &visitor)
        #expect(visitor.kinds == ["row", "cell", "Text"])
        visitor = KindVisitor()
        table.head[0].cells[0].walk(with: &visitor)
        #expect(visitor.kinds == ["cell", "Text"])
    }

    @Test("the dialect has no switches: every feature is recognised by a plain parse")
    func wholeDialect() throws {
        // One witness per feature that used to sit behind a `ParseOptions`
        // field, and one for the substitution smart punctuation used to make.
        #expect(try Document.parse("| a |\n| --- |\n| b |\n").content.first is Table)
        #expect(try dumped("~~x~~\n").contains("Strikethrough scope="))
        #expect(try dumped("www.example.com\n").contains("Link scope="))
        #expect(try dumped("- [x] task\n").contains("marker=\"x\""))
        #expect(try dumped("ref[^a]\n\n[^a]: note\n").contains("Cite scope="))
        #expect(try dumped("$x$\n").contains("Formula scope="))
        #expect(try dumped(":badge[label]\n").contains("Directive scope="))
        #expect(try dumped("\"quotes\" -- ...\n").contains("literal=\"\\\"quotes\\\" -- ...\""))
    }

    @Test("marks retain typed content and walk both phases after native release")
    func marks() throws {
        let document = try Document.parse("==a *b*==")
        let paragraph = try #require(document.content.first as? Paragraph)
        let mark = try #require(paragraph.content.first as? Mark)
        var visitor = RecordingWalkingVisitor()
        mark.walk(with: &visitor)
        #expect(
            visitor.events == [
                "enter:Mark", "enter:Text", "exit:Text", "enter:Emphasis",
                "enter:Text", "exit:Text", "exit:Emphasis", "exit:Mark",
            ]
        )
        #expect(mark.content.count == 2)
        #expect(((mark.content[1] as? Emphasis)?.content.first as? Text)?.literal == "b")
        #expect(
            try scope(of: mark, in: document, source: "==a *b*==")
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 9))
        )
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
                "enter:DirectiveBlock",
                "enter:DirectiveLabel",
                "enter:Text",
                "exit:Text",
                "exit:DirectiveLabel",
                "enter:Paragraph",
                "enter:Text",
                "exit:Text",
                "exit:Paragraph",
                "exit:DirectiveBlock",
            ]
        )
        #expect(block.content.count == 1)
        #expect(block.content.allSatisfy { !($0 is DirectiveLabel) })

        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        var tableVisitor = RecordingWalkingVisitor()
        table.walk(with: &tableVisitor)
        #expect(tableVisitor.rows == [table.head[0].id, table.content[0].id])
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
            try document.dump(in: "> quote\n")
                == "Document scope=1:1..1:7 anchor=null attributes={} children=1\n"
                + "└── Callout scope=1:1..1:7 anchor=null attributes={} variant=null collapsed=null children=1\n"
                + "    └── Paragraph scope=1:3..1:7 anchor=null attributes={} children=1\n"
                + "        └── Text scope=1:3..1:7 anchor=null attributes={} literal=\"quote\" children=0\n"
        )
    }

    @Test("footnote definitions are content, and the document lists and looks them up")
    func citations() throws {
        // An inherited call is a one-item cite naming its footnote by label
        // with empty affixes. Every definition stays in the content where it
        // was written; the document lists them in source order, and a label
        // resolves to its first definition.
        let source = "[^a] [^a]\n\n[^a]: once\n\n[^a]: twice\n"
        let document = try Document.parse(source)
        let cites = document.content.compactMap { $0 as? Paragraph }.flatMap(\.content).compactMap { $0 as? Cite }
        #expect(document.content.count == 3)
        #expect(cites.count == 2)
        for cite in cites {
            let citation = try #require(cite.citations.first)
            #expect(cite.citations.count == 1)
            #expect(citation.referent == .footnote(target: .label(value: "a")))
            #expect(citation.prefix.isEmpty && citation.suffix.isEmpty)
        }
        let footnote = try #require(document.footnotes.first)
        let later = try #require(document.footnotes.last)
        #expect(document.footnotes.map(\.label) == ["a", "a"])
        #expect(document.footnotes.map(\.id) == [document.content[1].id, document.content[2].id])
        #expect(document.footnote(for: "a") == footnote)
        #expect(document.footnote(for: "A") == nil)
        #expect(
            try scope(of: footnote, in: document, source: source)
                == Scope(start: Position(line: 3, column: 1), end: Position(line: 4, column: 0))
        )
        #expect(((footnote.content.first as? Paragraph)?.content.first as? Text)?.literal == "once")
        #expect(
            try scope(of: later, in: document, source: source)
                == Scope(start: Position(line: 5, column: 1), end: Position(line: 5, column: 11))
        )
        #expect(((later.content.first as? Paragraph)?.content.first as? Text)?.literal == "twice")
        let dump = try document.dump(in: source)
        #expect(dump.hasPrefix("Document scope=1:1..5:11 anchor=null attributes={} children=3\n"))
        let tail = """
            └── Footnote scope=5:1..5:11 anchor=null attributes={} label="a" children=1
                └── Paragraph scope=5:7..5:11 anchor=null attributes={} children=1
                    └── Text scope=5:7..5:11 anchor=null attributes={} literal="twice" children=0

            """
        #expect(dump.hasSuffix(tail))

        var visitor = RecordingWalkingVisitor()
        document.walk(with: &visitor)
        let expected = [
            "enter:Document", "enter:Paragraph",
            "enter:Cite", "enter:Citation", "exit:Citation", "exit:Cite",
            "enter:Text", "exit:Text",
            "enter:Cite", "enter:Citation", "exit:Citation", "exit:Cite",
            "exit:Paragraph",
            "enter:Footnote", "enter:Paragraph", "enter:Text", "exit:Text", "exit:Paragraph",
            "exit:Footnote",
            "enter:Footnote", "enter:Paragraph", "enter:Text", "exit:Text", "exit:Paragraph",
            "exit:Footnote", "exit:Document",
        ]
        #expect(visitor.events == expected)
    }

    @Test("empty input maps to an empty document")
    func empty() throws {
        let document = try Document.parse("")
        #expect(document.content.isEmpty)
        #expect(
            try scope(of: document, in: document, source: "")
                == Scope(start: Position(line: 1, column: 1), end: Position(line: 1, column: 0))
        )
        #expect(try document.dump(in: "") == "Document scope=1:1..1:0 anchor=null attributes={} children=0\n")
    }
}

@Suite("api") struct DirectiveLabelSuite {
    @Test("a directive label is dumped as a field but is not content")
    func labelledDirectiveBlock() throws {
        let source = ":::note[Title]{kind=demo}\nBody\n:::\n"
        let document = try Document.parse(source)
        let block = try #require(document.content.first as? DirectiveBlock)
        let label = try #require(block.label)
        #expect((label.content.first as? Text)?.literal == "Title")
        #expect(block.content.count == 1)
        #expect(block.content.first is Paragraph)
        #expect(block.attributes.records.first?.name == "kind")

        #expect(block.content.allSatisfy { !($0 is DirectiveLabel) })
        #expect(label.content.count == 1)
        #expect(label.content.first is Text)
        #expect(try document.dump(block, in: source).contains("DirectiveLabel"))

        // The other field arm: no label is emitted when none was written.
        let written = ":::note\nBody\n:::\n"
        let other = try Document.parse(written)
        let bare = try #require(other.content.first as? DirectiveBlock)
        #expect(bare.label == nil)
        #expect(try other.dump(bare, in: written).hasPrefix("DirectiveBlock scope=1:1..3:3 "))
    }

    @Test("the dump escapes every character JSON cannot carry literally")
    func dumpEscapes() throws {
        // The dumper's escape table has an arm per character and nothing in the
        // corpus writes most of them. A fenced code block carries its literal
        // through untouched, so it is the one place a test can put them all.
        let literal = "a\"b\\c\td\u{08}e\u{0c}f\u{01}g"
        let dump = try dumped("```\n\(literal)\n```\n")
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

@Suite("errors") struct ErrorSuite {
    @Test("each C status maps to the error code of the same name")
    func statusCodes() {
        let cases: [(markdown_core_status, ErrorCode)] = [
            (MARKDOWN_CORE_ALLOCATION_FAILED, .allocationFailed),
            (MARKDOWN_CORE_OUT_OF_BOUNDS, .outOfBounds),
            (MARKDOWN_CORE_KIND_MISMATCH, .kindMismatch),
            (MARKDOWN_CORE_INSIDE_SCALAR, .insideScalar),
        ]
        for (status, code) in cases {
            #expect(MarkdownCoreError(status).code == code)
        }
    }
}
