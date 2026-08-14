import Foundation
import Testing

// `@testable` for one reason, stated at its use: `ParseError` is not
// reachable through the public surface, because no input a Swift caller can
// hand `Document` is invalid.
@testable import MarkdownCore

@Suite("api") struct APISuite {
    @Test("parse options and visitor dispatch use the public Swift API")
    func publicAPI() throws {
        let options = ParseOptions()
        #expect(options.tables && options.directives && options.formulas && options.crossLinks && options.embeds)
        let document = try Document("# Heading\n")
        var visitor = KindVisitor()
        #expect(document.content[0].accept(&visitor) == "heading:1")
        let table = try #require(
            Document("| a |\n| --- |\n| b |\n").content.first as? Table
        )
        #expect(table.header.accept(&visitor) == "header")
        #expect(table.header.cells[0].accept(&visitor) == "cell")
        let directive = try #require(
            (Document(":badge[label]\n").content.first as? Paragraph)?
                .content.first as? Directive
        )
        #expect(directive.label?.accept(&visitor) == "DirectiveLabel")
        #expect(
            try Document("| a |\n| --- |\n| b |\n", options: ParseOptions(tables: false))
                .content.first is Paragraph
        )
    }

    @Test("cross-link and embed syntax is typed, source-faithful, and independently gated")
    func crossReferenceOptions() throws {
        let source =
            "before [[folder/note#^block|display]] and ![[folder/note#^block|display]] after\n"
        let paragraph = try #require(Document(source).content.first as? Paragraph)
        let crossLink = try #require(paragraph.content[1] as? CrossLink)
        let embed = try #require(paragraph.content[3] as? Embed)
        #expect(crossLink.reference == "folder/note#^block|display")
        #expect(embed.reference == "folder/note#^block|display")

        let linksDisabled = try #require(
            Document(source, options: ParseOptions(crossLinks: false))
                .content.first as? Paragraph
        )
        #expect(linksDisabled.content[1] is Embed)

        let embedsDisabled = try #require(
            Document(source, options: ParseOptions(embeds: false))
                .content.first as? Paragraph
        )
        #expect(embedsDisabled.content[1] is CrossLink)
    }

    @Test("the formulas switch controls every supported formula syntax")
    func formulaOption() throws {
        let inlineDollar = try Document("$x$\n")
        let blockDollar = try Document("$$x$$\n")
        let inlineLaTeX = try Document("\\\\(x\\\\)\n")
        let blockLaTeX = try Document("\\\\[x\\\\]\n")
        let fenced = try Document("```formula\nx\n```\n")

        #expect((inlineDollar.content.first as? Paragraph)?.content.first is Formula)
        #expect(blockDollar.content.first is FormulaBlock)
        #expect((inlineLaTeX.content.first as? Paragraph)?.content.first is Formula)
        #expect(blockLaTeX.content.first is FormulaBlock)
        #expect(fenced.content.first is FormulaBlock)

        let disabled = ParseOptions(formulas: false)
        #expect(
            (try Document("$x$\n", options: disabled).content.first as? Paragraph)?
                .content.first is Text
        )
        #expect(try Document("$$x$$\n", options: disabled).content.first is Paragraph)
        #expect(
            (try Document("\\\\(x\\\\)\n", options: disabled).content.first as? Paragraph)?
                .content.first is Text
        )
        #expect(try Document("\\\\[x\\\\]\n", options: disabled).content.first is Paragraph)
        #expect(try Document("```formula\nx\n```\n", options: disabled).content.first is CodeBlock)
    }
}

@Suite("unicode") struct UnicodeSuite {
    @Test("UTF-8 survives the C-to-Swift boundary")
    func unicode() throws {
        let paragraph = try #require(Document("héllo 🚀 中文\n").content.first as? Paragraph)
        #expect((paragraph.content.first as? Text)?.literal == "héllo 🚀 中文")
    }
}

@Suite("errors") struct ErrorsSuite {
    @Test("empty input maps to an empty document")
    func empty() throws {
        #expect(try Document("").content.isEmpty)
    }

    @Test("ParseError carries its native message through every presentation path")
    func parseErrorPresentation() {
        // Every byte sequence is a valid Markdown document, so `Document` and
        // `append` reject nothing a Swift caller can express: the engine's
        // remaining failures are allocation and internal, and no test can
        // provoke either. The presentation paths are pinned directly, because
        // `localizedDescription` degraded to a bare domain and code once and
        // that is invisible until a user is looking at the alert.
        let error = ParseError(
            code: .invalidArgument,
            message: "markdown must not be null when length is nonzero"
        )
        #expect(String(describing: error) == error.message)
        #expect(error.localizedDescription == error.message)
        #expect((error as NSError).localizedDescription == error.message)
        // A native failure that carried no error object still says something.
        #expect(!ParseError(from: nil).message.isEmpty)
        #expect(ParseError(from: nil).code == .internal)
    }
}

@Suite("ownership") struct OwnershipSuite {
    @Test("values remain usable and Sendable after native release")
    func copiedAndSendable() async throws {
        requireSendable(Document.self)
        requireSendable(ParseOptions.self)
        let document = try Document("parallel 🚀\n")
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
        #expect(try Document(String(repeating: unit, count: 5_000)).content.count == 10_000)
        var node = try #require(
            Document(String(repeating: "> ", count: 128) + "leaf\n").content.first
        )
        for _ in 0..<128 { node = try #require((node as? BlockQuote)?.content.first) }
        #expect(node is Paragraph)
        for _ in 0..<2_000 { #expect(try Document("# Copy\n\n- [x] item\n").content.count == 2) }
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
                        let dump = try Document(combo.source, options: combo.options).dump()
                        #expect(dump == combo.reference)
                    }
                }
            }
            try await group.waitForAll()
        }
    }
}

@Suite("depth") struct DepthSuite {
    /// Primitive results ferried out of the worker threads; access is
    /// sequenced by thread completion, never concurrent.
    private final class Outcome: @unchecked Sendable {
        var failure: String?
        var quoteEnters = 0
        var events = 0
        var dumpHasQuote = false
    }

    @Test("adversarial nesting walks and dumps beyond the call-stack budget")
    func adversarialNestingDepth() throws {
        // 4096 nested quotes overflowed the recursive walker. Two explicit
        // stacks make the proof exact: deep value trees deallocate through
        // recursive ARC releases, so every deep document lives and dies on
        // a 16 MiB-stack thread, while the walk and dump run on a 512 KiB
        // stack that recursive traversal at this depth could not survive.
        let depth = 4096
        let outcome = Outcome()
        let finished = DispatchSemaphore(value: 0)
        let owner = Thread {
            defer { finished.signal() }
            do {
                let prefix = String(repeating: "> ", count: depth)
                let document = try Document(prefix + "leaf\n")

                let walked = DispatchSemaphore(value: 0)
                // `unowned`, deliberately — weak would not do: if the
                // walker's closure owned the deep tree, the walker THREAD's
                // finalization could drop the last reference and run the
                // recursive ARC release on this 512 KiB stack. The owner
                // thread holds the document until `walked` is signaled —
                // after the walker's final use — so the lifetime is safe and
                // the deep tree still dies on the 16 MiB stack.
                // swiftlint:disable:next unowned_variable_capture
                let walker = Thread { [unowned document] in
                    defer { walked.signal() }
                    var quoteEnters = 0
                    var events = 0
                    MarkupWalker().walk(document) { event, node, _ in
                        events += 1
                        if event == .entering, node is BlockQuote { quoteEnters += 1 }
                    }
                    outcome.quoteEnters = quoteEnters
                    outcome.events = events
                    outcome.dumpHasQuote = document.dump().contains("BlockQuote")
                }
                walker.stackSize = 512 * 1024
                walker.start()
                walked.wait()
            } catch {
                outcome.failure = String(describing: error)
            }
        }
        owner.stackSize = 16 * 1024 * 1024
        owner.start()
        finished.wait()

        #expect(outcome.failure == nil)
        #expect(outcome.quoteEnters == depth)
        // Every node enters exactly once and exits exactly once: the
        // document, the quote chain, and the innermost paragraph and text.
        #expect(outcome.events == 2 * (depth + 3))
        #expect(outcome.dumpHasQuote)
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
                reference: try Document(source, options: options).dump()
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
