import Foundation
import MarkdownCore
import Testing

/// The tree of a whole-text parse: `Document(markdown:options:).seal()`,
/// keeping only the semantic view; the streamed cases spell the full entry
/// out themselves.
private func parse(_ source: String, options: ParseOptions = .init()) throws -> Semantic {
    try Document(markdown: source, options: options).seal().semantic
}

@Suite("conformance") struct ConformanceSuite {
    @Test("public node kinds are reachable through Swift values")
    func schemaReachability() throws {
        let sources = [
            "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n"
                + "``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n\n[ref]: /r \"t\"\n"
                + "\n[a][ref] ![b][ref]\n",
            "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) "
                + ":badge[label]{kind=demo} $x$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
            "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n"
                + ":::container[Title]{kind=demo}\nBody\n:::\n",
            "$$\ny\n$$\n",
        ]
        let documents = try sources.map { try parse($0) }
        let nodes = documents.flatMap(flatten)
        let kinds = Set(nodes.map(kindName))
        let expected: Set<String> = [
            "Semantic", "BlockQuote", "Paragraph", "Heading", "ThematicBreak", "List",
            "ListItem", "CodeBlock", "HTMLBlock", "FormulaBlock", "Table",
            "DirectiveBlock", "DirectiveLabel", "FootnoteDefinition", "Text", "SoftBreak",
            "LineBreak",
            "Code", "HTML", "Formula", "Emphasis", "Strong",
            "Strikethrough", "Link", "Image", "Directive", "FootnoteReference",
            "TableRow", "TableCell", "ReferenceDefinition", "LinkReference", "ImageReference",
        ]
        #expect(kinds == expected)
        #expect(documents.allSatisfy { $0.scope.start == Position(line: 1, column: 1) })
    }

    @Test("field and nullability mapping uses Swift-native types")
    func fieldsAndNullability() throws {
        let document = try parse(
            "3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image \"title\")\n"
        )
        let ordered = try #require(document.content[0] as? MarkdownCore.List)
        #expect(ordered.flavor == .ordered)
        #expect(ordered.start == 3)
        let task = try #require(document.content[1] as? MarkdownCore.List)
        #expect(task.items.first?.checked == true)
        let table = try #require(document.content[2] as? Table)
        #expect(table.alignments == [.center])
        #expect(table.header.isHeader)
        #expect(table.rows.allSatisfy { !$0.isHeader })
        #expect(table.header.cells.count == 1)
        let paragraph = try #require(document.content[3] as? Paragraph)
        let link = try #require(paragraph.content[0] as? Link)
        let image = try #require(paragraph.content[2] as? Image)
        #expect(link.destination == "/go" && link.title == nil)
        #expect(image.source == "/image" && image.title == "title")
    }

    @Test("all manifest cases match the shared canonical AST spec")
    func sharedCanonicalAST() throws {
        for testCase in try canonicalCases() {
            let document = try parse(testCase.source, options: testCase.parseOptions.value)
            #expect(TreeDumper.dump(document) == testCase.expected, Comment(rawValue: testCase.name))
            #expect(document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }

    @Test("all manifest cases, streamed in 7-byte chunks, seal to the same goldens")
    func sharedCanonicalASTStreamed() throws {
        // THE STREAM'S corpus is the one-shot corpus (docs/STREAMING.md D6:
        // T14 extends the corpora rather than adding a channel). 7 is prime,
        // so the chunk boundary drifts through every alignment and splits
        // line endings and construct delimiters somewhere in every case.
        for testCase in try canonicalCases() {
            let options = testCase.parseOptions.value
            let document = try Document(options: options)
            let bytes = Array(testCase.source.utf8)
            var start = 0
            while start < bytes.count {
                let end = min(start + 7, bytes.count)
                _ = try document.feed(chunk: Array(bytes[start..<end]))
                start = end
            }
            let sealed = try document.seal()
            #expect(TreeDumper.dump(sealed.semantic) == testCase.expected, Comment(rawValue: testCase.name))
            #expect(sealed.dump() == testCase.expected, Comment(rawValue: testCase.name))
            let wholeText = try Document(markdown: testCase.source, options: options).seal()
            #expect(sealed.concrete == wholeText.concrete, Comment(rawValue: testCase.name))
        }
    }
}

private func canonicalCases() throws -> [CanonicalCase] {
    let resource = try #require(
        Bundle.module.url(forResource: "canonical-ast-fixtures", withExtension: "json")
    )
    let manifestData = try Data(contentsOf: resource)
    let manifest = try JSONDecoder().decode(CanonicalManifest.self, from: manifestData)
    #expect(manifest.schemaVersion == 1)
    #expect(!manifest.cases.isEmpty)
    return manifest.cases
}

private struct CanonicalManifest: Decodable {
    let schemaVersion: Int
    let cases: [CanonicalCase]
}

private struct CanonicalCase: Decodable {
    let name: String
    let source: String
    let expected: String
    let parseOptions: CanonicalParseOptions
}

private struct CanonicalParseOptions: Decodable {
    let smartPunctuation: Bool
    let footnotes: Bool
    let stripHTMLComments: Bool
    let tables: Bool
    let strikethrough: Bool
    let autolinks: Bool
    let taskLists: Bool
    let formulas: Bool
    let directives: Bool

    var value: ParseOptions {
        ParseOptions(
            smartPunctuation: smartPunctuation,
            footnotes: footnotes,
            stripHTMLComments: stripHTMLComments,
            tables: tables,
            strikethrough: strikethrough,
            autolinks: autolinks,
            taskLists: taskLists,
            formulas: formulas,
            directives: directives
        )
    }
}

private func flatten(_ root: any Markup) -> [any Markup] {
    var result: [any Markup] = []
    Walker().walk(root) { event, node in
        if case .entering = event { result.append(node) }
    }
    return result
}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}
