import Foundation
import MarkdownCore
import Testing

@Suite("api") struct APISuite {
    @Test("parse options and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let options = ParseOptions()
        #expect(options.tables && options.directives && options.formulas && options.crossLinks && options.embeds)
        let document = try Document.parse("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.content[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document.parse("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        #expect(table.header.accept(&visitor) == "header")
        #expect(table.header.cells[0].accept(&visitor) == "cell")
        let directive = try #require(
            (Document.parse(":badge[label]\n").content.first as? Paragraph)?
                .content.first as? Directive
        )
        #expect(directive.label?.accept(&visitor) == "DirectiveLabel")
        #expect(
            try Document.parse("| a |\n| --- |\n| b |\n", options: ParseOptions(tables: false))
                .content.first is Paragraph
        )
    }

    @Test("cross-link and embed syntax is typed, source-faithful, and independently gated")
    func crossReferenceOptions() throws {
        let source =
            "before [[folder/note#^block|display]] and ![[folder/note#^block|display]] after\n"
        let paragraph = try #require(Document.parse(source).content.first as? Paragraph)
        let crossLink = try #require(paragraph.content[1] as? CrossLink)
        let embed = try #require(paragraph.content[3] as? Embed)
        #expect(crossLink.reference == "folder/note#^block|display")
        #expect(embed.reference == "folder/note#^block|display")

        let linksDisabled = try #require(
            Document.parse(source, options: ParseOptions(crossLinks: false))
                .content.first as? Paragraph
        )
        #expect(linksDisabled.content[1] is Embed)

        let embedsDisabled = try #require(
            Document.parse(source, options: ParseOptions(embeds: false))
                .content.first as? Paragraph
        )
        #expect(embedsDisabled.content[1] is CrossLink)
    }

    @Test("the formulas switch controls every supported formula syntax")
    func formulaOption() throws {
        let inlineDollar = try Document.parse("$x$\n")
        let blockDollar = try Document.parse("$$x$$\n")
        let inlineLaTeX = try Document.parse("\\\\(x\\\\)\n")
        let blockLaTeX = try Document.parse("\\\\[x\\\\]\n")
        let fenced = try Document.parse("```formula\nx\n```\n")

        #expect((inlineDollar.content.first as? Paragraph)?.content.first is Formula)
        #expect(blockDollar.content.first is FormulaBlock)
        #expect((inlineLaTeX.content.first as? Paragraph)?.content.first is Formula)
        #expect(blockLaTeX.content.first is FormulaBlock)
        #expect(fenced.content.first is FormulaBlock)

        let disabled = ParseOptions(formulas: false)
        #expect(
            (try Document.parse("$x$\n", options: disabled).content.first as? Paragraph)?
                .content.first is Text
        )
        #expect(try Document.parse("$$x$$\n", options: disabled).content.first is Paragraph)
        #expect(
            (try Document.parse("\\\\(x\\\\)\n", options: disabled).content.first as? Paragraph)?
                .content.first is Text
        )
        #expect(try Document.parse("\\\\[x\\\\]\n", options: disabled).content.first is Paragraph)
        #expect(try Document.parse("```formula\nx\n```\n", options: disabled).content.first is CodeBlock)
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

    @Test("ParseError carries its native message through every presentation path")
    func parseErrorPresentation() throws {
        let session = try MarkupSession()
        do {
            try session.replace(5..<9, with: "beyond the stored text")
            Issue.record("an out-of-range edit must throw")
        } catch let error as ParseError {
            #expect(error.code == .invalidArgument)
            #expect(!error.message.isEmpty)
            // String interpolation and Foundation presentation must agree:
            // localizedDescription previously degraded to a bare domain/code.
            #expect(String(describing: error) == error.message)
            #expect(error.localizedDescription == error.message)
            #expect((error as NSError).localizedDescription == error.message)
        }
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
            directives: false,
            crossLinks: false,
            embeds: false
        ),
        ParseOptions(
            strikethrough: false,
            formulas: false
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
    mutating func visit(_ node: TableRow) -> String { node.isHeader ? "header" : "row" }
    mutating func visit(_ node: TableCell) -> String { "cell" }
    mutating func visit(_ node: DirectiveBlock) -> String { kindName(node) }
    mutating func visit(_ node: DirectiveLabel) -> String { kindName(node) }
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
    mutating func visit(_ node: ReferenceDefinition) -> String { kindName(node) }
    mutating func visit(_ node: LinkReference) -> String { kindName(node) }
    mutating func visit(_ node: ImageReference) -> String { kindName(node) }
    mutating func visit(_ node: CrossLink) -> String { kindName(node) }
    mutating func visit(_ node: Embed) -> String { kindName(node) }
}

private func requireSendable<T: Sendable>(_: T.Type) {}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}
