import Foundation
import MarkdownCore
import Testing
import struct Testing.Comment

@Suite("conformance") struct ConformanceSuite {
    @Test("public node kinds are emitted by the per-node Swift dumper")
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
            "a <!-- b --> c\n\n<!-- block -->\n",
        ]
        let documents = try sources.map { try Document.parse($0) }
        let kinds = Set(documents.flatMap { dumpKinds($0.dump()) })
        let expected: Set<String> = [
            "Document", "Callout", "Paragraph", "Heading", "ThematicBreak", "List",
            "ListItem", "CodeBlock", "HTMLBlock", "FormulaBlock", "Table",
            "DirectiveBlock", "DirectiveLabel", "Text", "SoftBreak",
            "LineBreak",
            "Code", "HTML", "Comment", "Formula", "Emphasis", "Strong",
            "Strikethrough", "Link", "Image", "Directive", "Cite",
            "TableRow", "TableCell",
        ]
        #expect(kinds == expected)
        #expect(documents.allSatisfy { $0.scope.start == Position(line: 1, column: 1) })
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
        #expect(table.alignments == [.center])
        #expect(table.header.isHeader)
        #expect(table.rows.allSatisfy { !$0.isHeader })
        #expect(table.header.cells.count == 1)
        let paragraph = try #require(document.content[3] as? Paragraph)
        let link = try #require(paragraph.content[0] as? Link)
        let image = try #require(paragraph.content[2] as? Image)
        #expect(link.dest == .url("/go") && link.title == nil)
        #expect(image.dest == .url("/image") && image.title == "title")
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
            #expect(TreeDumper.dump(document) == testCase.expected, Testing.Comment(rawValue: testCase.name))
            #expect(document.dump() == testCase.expected, Testing.Comment(rawValue: testCase.name))
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

/// The node lines of a dump: value lines (`Citation`, `Footnote`) and group
/// lines are not kinds.
private func dumpKinds(_ dump: String) -> [String] {
    let names = dump.split(separator: "\n").compactMap { line -> String? in
        guard line.contains(" scope=") else { return nil }
        return line.trimmingCharacters(in: CharacterSet(charactersIn: "│ ├└─"))
            .split(separator: " ")
            .first
            .map(String.init)
    }
    return names.filter { $0 != "Citation" && $0 != "Footnote" }
}
