import Foundation
import MarkdownCore
import Testing

@Suite("conformance") struct ConformanceSuite {
    @Test("public node kinds are reachable through Swift values")
    func schemaReachability() throws {
        let sources = [
            "# Heading\n\n> Quote\n\n---\n\n3. ordered\n\n- [x] task\n\n"
                + "``` swift\ncode\n```\n\n<section>raw</section>\n\n[^n]: note\n",
            "Text *em* **strong** ~~strike~~ `code` [link](/go \"title\") ![alt](/image.png) "
                + ":badge[label]{kind=demo} $x$ [^n]  \nnext <i>raw</i>\nsoft\n\n[^n]: definition\n",
            "| left | center |\n| :--- | :----: |\n| a | b |\n\n::leaf[Label]{id=value}\n\n"
                + ":::container[Title]{kind=demo}\nBody\n:::\n",
            "$$\ny\n$$\n",
        ]
        let documents = try sources.map { try Document.parse($0) }
        let nodes = documents.flatMap(flatten)
        let kinds = Set(nodes.map(kindName))
        let expected: Set<String> = [
            "Document", "BlockQuote", "Paragraph", "Heading", "ThematicBreak", "List",
            "ListItem", "CodeBlock", "HTMLBlock", "FormulaBlock", "Table",
            "DirectiveBlock", "FootnoteDefinition", "Text", "SoftBreak", "LineBreak",
            "Code", "HTML", "Formula", "Emphasis", "Strong",
            "Strikethrough", "Link", "Image", "Directive", "FootnoteReference",
            "TableRow", "TableCell",
        ]
        #expect(kinds == expected)
        #expect(
            documents.allSatisfy { $0.scope(of: $0).start == Position(line: 1, column: 1) }
        )
    }

    @Test("field and nullability mapping uses Swift-native types")
    func fieldsAndNullability() throws {
        let document = try Document.parse(
            "3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image \"title\")\n"
        )
        let ordered = try #require(document.children[0] as? MarkdownCore.List)
        #expect(ordered.flavor == .ordered)
        #expect(ordered.start == 3)
        let task = try #require(document.children[1] as? MarkdownCore.List)
        #expect((task.children.first as? ListItem)?.isChecked == true)
        let table = try #require(document.children[2] as? Table)
        #expect(table.alignments == [.center])
        #expect(table.header.isHeader)
        #expect(table.rows.allSatisfy { !$0.isHeader })
        #expect(table.header.cells.count == 1)
        let paragraph = try #require(document.children[3] as? Paragraph)
        let link = try #require(paragraph.children[0] as? Link)
        let image = try #require(paragraph.children[2] as? Image)
        #expect(link.destination == "/go" && link.title == nil)
        #expect(image.source == "/image" && image.title == "title")
    }

    @Test("all manifest cases match the shared canonical AST spec")
    func sharedCanonicalAST() throws {
        let manifest = try loadManifest()
        for testCase in manifest.cases {
            let document = try Document.parse(testCase.source, options: testCase.parseOptions.value)
            #expect(TreeDumper.dump(document) == testCase.expected, Comment(rawValue: testCase.name))
            #expect(document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }

    @Test("sessions replay the manifest corpus to dump equality per commit")
    func sessionEquivalenceReplay() throws {
        let manifest = try loadManifest()
        for testCase in manifest.cases {
            let session = try MarkupSession(options: testCase.parseOptions.value)
            var replayed = ""
            var previous: [UInt64: UInt64] = [:]
            for chunk in lineChunks(testCase.source) {
                replayed += chunk
                try session.append(chunk)
                let commit = try session.commit()

                // Equivalence: the incremental snapshot dumps byte-equal to a
                // one-shot parse of the same prefix.
                let reference = try Document.parse(replayed, options: testCase.parseOptions.value)
                #expect(commit.document.dump() == reference.dump(), Comment(rawValue: testCase.name))

                // Delta-mirror integrity: every node outside the
                // delta kept its exact revision, removed ids are gone,
                // and the four arrays are disjoint.
                let delta = commit.delta
                let touched = [delta.added, delta.changed, delta.bubbled]
                    .joined().map(\.rawValue)
                #expect(
                    touched.count + delta.removed.count
                        == Set(touched).union(delta.removed.map(\.rawValue)).count
                )
                var current: [UInt64: UInt64] = [:]
                let touchedSet = Set(touched)
                for node in flatten(commit.document) {
                    current[node.id.rawValue] = node.revision
                    if !touchedSet.contains(node.id.rawValue) {
                        #expect(previous[node.id.rawValue] == node.revision)
                    }
                }
                for removed in delta.removed {
                    #expect(current[removed.rawValue] == nil)
                }
                previous = current
            }
            #expect(session.document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }
}

private func loadManifest() throws -> CanonicalManifest {
    let resource = try #require(
        Bundle.module.url(forResource: "canonical-ast-fixtures", withExtension: "json")
    )
    let manifestData = try Data(contentsOf: resource)
    let manifest = try JSONDecoder().decode(CanonicalManifest.self, from: manifestData)
    #expect(manifest.schemaVersion == 1)
    #expect(!manifest.cases.isEmpty)
    return manifest
}

private func lineChunks(_ source: String) -> [String] {
    var chunks: [String] = []
    var current = ""
    for character in source {
        current.append(character)
        if character == "\n" {
            chunks.append(current)
            current = ""
        }
    }
    if !current.isEmpty {
        chunks.append(current)
    }
    return chunks
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
    let dollarFormulaDelimiters: Bool
    let latexFormulaDelimiters: Bool
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
            dollarFormulaDelimiters: dollarFormulaDelimiters,
            latexFormulaDelimiters: latexFormulaDelimiters,
            directives: directives
        )
    }
}

private func flatten(_ document: Document) -> [any Markup] {
    var result: [any Markup] = []
    Walker().walk(document) { event, node, _ in
        if case .entering = event { result.append(node) }
    }
    return result
}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}
