import MarkdownCore
import Testing

@Suite("api") struct APISuite {
    @Test("parse options and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let options = ParseOptions()
        #expect(options.tables && options.directives && options.formulas)
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.children[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").children.first as? Table
        )
        #expect(table.header.accept(&visitor) == "header")
        #expect(table.header.cells[0].accept(&visitor) == "cell")
        #expect(
            try Document.parse("| a |\n| --- |\n| b |\n", options: ParseOptions(tables: false))
                .children.first is Paragraph
        )
    }
}

@Suite("unicode") struct UnicodeSuite {
    @Test("UTF-8 survives the C-to-Swift boundary")
    func unicode() throws {
        let paragraph = try #require(Document.parse("héllo 🚀 中文\n").children.first as? Paragraph)
        #expect((paragraph.children.first as? Text)?.literal == "héllo 🚀 中文")
    }
}

@Suite("errors") struct ErrorsSuite {
    @Test("empty input maps to an empty document")
    func empty() throws {
        #expect(try Document.parse("").children.isEmpty)
    }
}

@Suite("ownership") struct OwnershipSuite {
    @Test("values remain usable and Sendable after native release")
    func copiedAndSendable() async throws {
        requireSendable(Document.self)
        requireSendable(ParseOptions.self)
        let document = try Document.parse("parallel 🚀\n")
        let counts = await withTaskGroup(of: Int.self, returning: [Int].self) { group in
            for _ in 0..<20 { group.addTask { document.children.count } }
            return await group.reduce(into: []) { $0.append($1) }
        }
        #expect(counts == Array(repeating: 1, count: 20))
    }

}

@Suite("robustness") struct RobustnessSuite {
    @Test("large and deeply nested inputs preserve complete value trees")
    func workloads() throws {
        let unit = "## Section\n\nParagraph with **strong**, [link](/), and 🚀.\n\n"
        #expect(try Document.parse(String(repeating: unit, count: 5_000)).children.count == 10_000)
        var node = try #require(
            Document.parse(String(repeating: "> ", count: 128) + "leaf\n").children.first
        )
        for _ in 0..<128 { node = try #require((node as? BlockQuote)?.children.first) }
        #expect(node is Paragraph)
        for _ in 0..<2_000 { #expect(try Document.parse("# Copy\n\n- [x] item\n").children.count == 2) }
    }

    @Test("simultaneous parses with disagreeing options never interfere")
    func concurrentParses() async throws {
        // The engine holds no process-global state: parses that disagree
        // about extension special characters ('~', '$', ':') must never
        // observe each other. Dumps are compared byte-for-byte against
        // single-threaded references.
        let combos = try makeConcurrencyCombos()
        try await withThrowingTaskGroup(of: Void.self) { group in
            for worker in 0..<8 {
                group.addTask {
                    for iteration in 0..<25 {
                        let combo = combos[(worker + iteration) % combos.count]
                        let dump = try Document.parse(combo.source, options: combo.options).dump()
                        #expect(dump == combo.reference)
                    }
                }
            }
            try await group.waitForAll()
        }
    }
}

private struct ConcurrencyCombo: Sendable {
    let source: String
    let options: ParseOptions
    let reference: String
}

private func makeConcurrencyCombos() throws -> [ConcurrencyCombo] {
    let sources = [
        "# Heading\n\nPlain *emphasis* and **strong** text with `code`.\n",
        "| a | b |\n| --- | :-: |\n| 1 | 2 |\n\n~~struck~~ and *a~b*c~ mix.\n",
        "Formula $x^2$ inline and *a$b*c$ flanking.\n\n$$\nx = y\n$$\n",
        ":::note[Label]{id=1 title=\"T\"}\ncontent *here*\n:::\n\nInline :dir[text]{k=v} tail.\n",
    ]
    let variants = [
        ParseOptions(),
        ParseOptions(
            smartPunctuation: false,
            footnotes: false,
            stripHTMLComments: false,
            tables: false,
            strikethrough: false,
            autolinks: false,
            taskLists: false,
            formulas: false,
            dollarFormulaDelimiters: false,
            latexFormulaDelimiters: false,
            directives: false
        ),
        ParseOptions(
            strikethrough: false,
            formulas: false,
            dollarFormulaDelimiters: false,
            latexFormulaDelimiters: false
        ),
    ]
    return try sources.flatMap { source in
        try variants.map { options in
            ConcurrencyCombo(
                source: source,
                options: options,
                reference: try Document.parse(source, options: options).dump()
            )
        }
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
    mutating func visit(_ node: FootnoteDefinition) -> String { kindName(node) }
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
