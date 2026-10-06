import Foundation
import MarkdownCore
import Testing

// Testing macros refer to Comment without a module qualifier.
private typealias Comment = Testing.Comment

@Suite("conformance") struct ConformanceSuite {
    @Test("public node kinds are emitted by the per-node Swift dumper")
    func schemaReachability() throws {
        let sources = [
            "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n"
                + "``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n\n[ref]: /r \"t\"\n"
                + "\n[a][ref] ![b][ref]\n",
            "Text *em* **strong** ~~strike~~ ==mark== ++inserted++ [span]{} ^up^ ~down~ "
                + "`code` [link](/go \"title\") ![alt](/image.png) "
                + ":badge[label]{kind=demo} $x$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
            "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n"
                + ":::container[Title]{kind=demo}\nBody\n:::\n",
            "$$\ny\n$$\n",
            "a <!-- b --> c\n\n<!-- block -->\n",
            "[[Note]] ![[#^block|]]\n",
            "Term\n: body\n",
            "---\ntitle: Example\n---\n(@sample) Body\n",
            ":plain :empty[]{} :attrs{#kept .a class=\"b a\" k=1 k=2}\n\n| none |\n| ---- |\n| cell |\n",
        ]
        let documents = try sources.map { try Document.parse($0) }
        let dumps = try zip(documents, sources).map { try $0.dump(in: $1) }
        let kinds = Set(dumps.flatMap { dumpKinds($0) })
        let expected: Set<String> = [
            "Document", "Callout", "Paragraph", "Heading", "ThematicBreak", "List",
            "ListItem", "CodeBlock", "HTMLBlock", "FormulaBlock", "Table",
            "DirectiveBlock", "DirectiveLabel", "Text", "SoftBreak",
            "LineBreak",
            "Code", "HTML", "Comment", "CrossLink", "CrossEmbedded", "Formula", "Emphasis", "Strong",
            "Strikethrough", "Mark", "Insertion", "Span", "Superscript", "Subscript", "DefinitionList", "Definition",
            "Link", "Embedded", "Directive",
            "Cite", "Citation", "Footnote", "Specimen", "Metadata", "Reference",
            "TableRow", "TableCell", "TableCaption",
        ]
        #expect(kinds == expected)
        for (document, source) in zip(documents, sources) {
            #expect(try document.scope(of: document, in: source).first?.start == Position(line: 1, column: 1))
        }
    }

    @Test("field and nullability mapping uses Swift-native types")
    func fieldsAndNullability() throws {
        let document = try Document.parse(
            "3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image \"title\")\n"
        )
        let ordered = try #require(document.content[0] as? MarkdownCore.List)
        #expect(ordered.flavor == .ordered)
        #expect(ordered.start == 3)
        #expect(ordered.variant == .decimal)
        #expect(ordered.delimiter == .period)
        let parenthesized = try #require(try Document.parse("1) item\n").content[0] as? MarkdownCore.List)
        #expect(parenthesized.delimiter == .parenthesis(closed: false))
        let task = try #require(document.content[1] as? MarkdownCore.List)
        #expect(task.items.first?.marker == "x")
        #expect(task.items.first?.tasked == true)
        #expect(task.items.first?.completed == true)
        let table = try #require(document.content[2] as? Table)
        #expect(table.columns.map(\.flow) == [.center])
        #expect(table.head.count == 1)
        #expect(table.content.count == 1 && table.foot.isEmpty)
        #expect(table.head[0].cells.count == 1)
        let paragraph = try #require(document.content[3] as? Paragraph)
        let link = try #require(paragraph.content[0] as? Link)
        let image = try #require(paragraph.content[2] as? Embedded)
        #expect(link.dest == .url("/go") && link.title == nil)
        #expect(image.dest == .url("/image") && image.title == "title")
    }

    @Test("task markers preserve scalars and derive completion")
    func taskMarkers() throws {
        for marker in [" ", "x", "X", "?", "é", "✓", "🚀", "́", "]"] {
            let list = try #require(try Document.parse("- [\(marker)] body\n").content.first as? MarkdownCore.List)
            let item = try #require(list.items.first)
            #expect(item.marker == marker)
            #expect(item.tasked)
            #expect(item.completed == (marker != " "))
        }
        let list = try #require(try Document.parse("- [é] body\n").content.first as? MarkdownCore.List)
        #expect(list.items.first?.marker == nil)
        #expect(list.items.first?.tasked == false)
        #expect(list.items.first?.completed == false)
    }

    @Test("all manifest cases match the shared canonical AST spec")
    func sharedCanonicalAST() throws {
        let resource = try #require(
            Bundle.module.url(forResource: "canonical-ast-fixtures", withExtension: "json")
        )
        let manifestData = try Data(contentsOf: resource)
        let manifest = try JSONDecoder().decode(CanonicalManifest.self, from: manifestData)
        #expect(manifest.schemaVersion == 1)
        #expect(!manifest.cases.isEmpty)

        for testCase in manifest.cases {
            let document = try Document.parse(testCase.source)
            // `Testing.Comment`, qualified: the package exports a `Comment` markup kind.
            #expect(
                try document.dump(in: testCase.source) == testCase.expected,
                Testing.Comment(rawValue: testCase.name)
            )
        }
    }

    @Test("a fresh parse numbers its nodes from 1 in completion order, and two parses are equal")
    func freshIdentifiers() throws {
        let resource = try #require(
            Bundle.module.url(forResource: "canonical-ast-fixtures", withExtension: "json")
        )
        let manifest = try JSONDecoder().decode(CanonicalManifest.self, from: Data(contentsOf: resource))
        for testCase in manifest.cases {
            let document = try Document.parse(testCase.source)
            var visitor = IdentifierVisitor()
            document.walk(with: &visitor)
            // Every owned relation is walked, so 1...n is also uniqueness; the
            // document completes last.
            #expect(
                visitor.ids.sorted() == Array(1...UInt64(visitor.ids.count)),
                Testing.Comment(rawValue: testCase.name)
            )
            #expect(document.id.value == UInt64(visitor.ids.count), Testing.Comment(rawValue: testCase.name))
            let again = try Document.parse(testCase.source)
            #expect(document == again, Testing.Comment(rawValue: testCase.name))
            #expect(document.hashValue == again.hashValue, Testing.Comment(rawValue: testCase.name))
        }
    }
}

private struct CanonicalManifest: Decodable {
    let schemaVersion: Int
    let cases: [CanonicalCase]
}

private struct CanonicalCase: Decodable {
    let name: String
    let source: String
    let expected: String
}

/// Scoped lines identify Markup nodes; unscoped values and groups are excluded.
private func dumpKinds(_ dump: String) -> [String] {
    dump.split(separator: "\n").compactMap { line -> String? in
        guard line.contains(" scope=") else { return nil }
        return line.trimmingCharacters(in: CharacterSet(charactersIn: "│ ├└─"))
            .split(separator: " ")
            .first
            .map(String.init)
    }
}

/// The ids of a walk's nodes, in the order it enters them.
private struct IdentifierVisitor: MarkupVisitor {
    var ids: [UInt64] = []

    private mutating func record(_ node: some Markup, _ phase: MarkupVisitPhase) {
        if phase == .enter { ids.append(node.id.value) }
    }

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Table, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Code, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: MarkdownCore.Comment, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Span, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Link, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) { record(node, phase) }
    mutating func visit(_ node: Reference, phase: MarkupVisitPhase) { record(node, phase) }
}
