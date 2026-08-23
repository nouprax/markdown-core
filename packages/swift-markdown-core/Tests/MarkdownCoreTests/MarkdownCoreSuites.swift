import MarkdownCore
import Testing

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
    @Test("empty input maps to an empty document")
    func empty() throws {
        #expect(try Document.parse("").content.isEmpty)
    }
}

@Suite("ownership") struct OwnershipSuite {
    @Test("values remain usable and Sendable after native release")
    func copiedAndSendable() async throws {
        requireSendable(Document.self)
        requireSendable(Concrete.self)
        requireSendable(ParseOptions.self)
        let document = try Document.parse("parallel 🚀\n")
        let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { group in
            for _ in 0..<20 { group.addTask { document.content.count } }
            return await group.reduce(into: []) { $0.append($1) }
        }
        #expect(counts == Array(repeating: 1, count: 20))
    }

}

@Suite("concrete") struct ConcreteSuite {
    /// The requirement's own sentence: the concrete view survives being copied
    /// into value types and the handle being freed. `parse` frees it before it
    /// returns, so everything below reads a view that has no native anything
    /// left behind it.
    @Test("the concrete view is total and its owners resolve after native release")
    func twoViews() throws {
        let source = """
            # Heading ##

            > quoted *em* and `code`

            | a | b |
            | --- | --- |
            | c | d |

            :::container[Title]{kind=demo}
            Body
            :::

            [a]: /u "t"

            see [a].

            """
        let document = try Document.parse(source)
        let concrete = document.concrete
        #expect(concrete.source == Array(source.utf8))
        #expect(concrete.lineCount == 15)
        #expect(concrete.lineStart(1) == 0)
        #expect(concrete.lineStart(3) == 14)
        #expect(concrete.lineStart(0) == nil)
        #expect(concrete.lineStart(16) == nil)
        #expect(concrete.region(at: -1) == nil)
        #expect(concrete.region(at: concrete.regionCount) == nil)

        var covered = 0
        var resolved = 0
        var markers = 0
        for index in 0..<concrete.regionCount {
            let region = try #require(concrete.region(at: index))
            #expect(region.start == covered)
            #expect(region.length > 0)
            covered += region.length
            if document.owner(of: region) != nil { resolved += 1 }
            if region.role == .marker { markers += 1 }
        }
        #expect(covered == concrete.source.count)
        #expect(resolved == concrete.regionCount)
        // The heading's closing `##`, the table's pipes and the definition's
        // punctuation are in no literal anywhere in the semantic tree, and the
        // count above says every byte of them is in a region here.
        #expect(markers > 0)
        #expect(document.owner(of: Region(start: 0, length: 1, role: .content, owner: [99])) == nil)

        // THE DESCENT IS THE C CHILD ORDER, not the value tree's named fields.
        // A table holds its header BEFORE its rows, so byte 42 -- the `a` of
        // the header row -- has to land on line 5 and not on line 7.
        #expect(owner(of: 42, in: document)?.scope.start == Position(line: 5, column: 3))
        // A directive holds its LABEL before its content, so byte 106 -- the
        // `B` of `Body` -- has to land on line 10 and not inside the label on
        // line 9.
        let body = try #require(owner(of: 106, in: document))
        #expect(body.scope.start == Position(line: 10, column: 1))
    }

    /// The owner of the region the byte at `offset` belongs to.
    private func owner(of offset: Int, in document: Document) -> (any Markup)? {
        for index in 0..<document.concrete.regionCount {
            guard let region = document.concrete.region(at: index) else { return nil }
            if offset >= region.start, offset < region.start + region.length {
                return document.owner(of: region)
            }
        }
        return nil
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
