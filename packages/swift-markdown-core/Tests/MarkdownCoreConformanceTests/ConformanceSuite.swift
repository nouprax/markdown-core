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
        let documents = try sources.map { try Document($0) }
        let nodes = documents.flatMap(flatten)
        let kinds = Set(nodes.map(kindName))
        let expected: Set<String> = [
            "Document", "BlockQuote", "Paragraph", "Heading", "ThematicBreak", "List",
            "ListItem", "CodeBlock", "HTMLBlock", "FormulaBlock", "Table",
            "TableRow", "TableCell", "DirectiveBlock", "DirectiveLabel",
            "FootnoteDefinition", "Text", "SoftBreak", "LineBreak",
            "Code", "HTML", "Formula", "Emphasis", "Strong",
            "Strikethrough", "Link", "Image", "Directive", "FootnoteReference",
        ]
        #expect(kinds == expected)
        #expect(
            documents.allSatisfy { $0.scope.start == Position(line: 1, column: 1) }
        )
    }

    @Test("field and nullability mapping uses Swift-native types")
    func fieldsAndNullability() throws {
        let document = try Document(
            "3. item\n\n- [x] task\n\n| a |\n| :-: |\n| b |\n\n[link](/go) ![alt](/image \"title\")\n"
        )
        let ordered = try #require(document.content[0] as? MarkdownCore.List)
        #expect(ordered.flavor == .ordered)
        #expect(ordered.start == 3)
        #expect(ordered.tight)
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

    @Test("code carries its placement mode and code blocks their fence state")
    func codePlacementModes() throws {
        let document = try Document("`span`\n\n```swift\nbody\n```\n\n    indented\n")
        let paragraph = try #require(document.content[0] as? Paragraph)
        let code = try #require(paragraph.content[0] as? Code)
        #expect(code.mode == .embedded)
        let fencedBlock = try #require(document.content[1] as? CodeBlock)
        #expect(fencedBlock.mode == .standalone)
        #expect(fencedBlock.fenced && fencedBlock.closed)
        let indentedBlock = try #require(document.content[2] as? CodeBlock)
        #expect(indentedBlock.mode == .standalone)
        #expect(!indentedBlock.fenced && indentedBlock.closed)
    }

    @Test("directives partition typed label and content; nil and empty labels stay distinct")
    func directiveLabelPartition() throws {
        let document = try Document(
            ":::note[*Title*]{kind=demo}\nBody\n:::\n\n:::bare\nBody\n:::\n\n"
                + "Inline :badge[label] then :plain and :empty[].\n"
        )
        let labeled = try #require(document.content[0] as? DirectiveBlock)
        #expect(labeled.mode == .standalone)
        let labeledLabel = try #require(labeled.label)
        #expect(labeledLabel.content.count == 1)
        #expect(labeledLabel.content.first is Emphasis)
        #expect(labeledLabel.id != labeled.id)
        #expect(labeled.content.count == 1)
        #expect(labeled.content.first is Paragraph)
        let bare = try #require(document.content[1] as? DirectiveBlock)
        #expect(bare.label == nil)
        #expect(bare.content.count == 1)
        let paragraph = try #require(document.content[2] as? Paragraph)
        let inline = try #require(paragraph.content[1] as? Directive)
        #expect(inline.mode == .embedded)
        #expect(inline.label?.content.count == 1)
        let unlabeled = try #require(paragraph.content[3] as? Directive)
        #expect(unlabeled.label == nil)
        let empty = try #require(paragraph.content[5] as? Directive)
        #expect(empty.label != nil)
        #expect(empty.label?.content.isEmpty == true)
        #expect(
            (try #require(empty.label)).scope
                == Scope(
                    start: Position(line: 9, column: 44),
                    end: Position(line: 9, column: 45)
                )
        )
    }

    @Test("all manifest cases match the shared canonical AST spec")
    func sharedCanonicalAST() throws {
        let manifest = try loadManifest()
        for testCase in manifest.cases {
            let document = try Document(testCase.source, options: testCase.parseOptions.value)
            #expect(MarkupDumper.dump(document) == testCase.expected, Comment(rawValue: testCase.name))
            #expect(document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }

    @Test("edits replay the manifest corpus to dump equality per revision")
    func editEquivalenceReplay() throws {
        let manifest = try loadManifest()
        for testCase in manifest.cases {
            var document = try Document("", options: testCase.parseOptions.value)
            var replayed = ""
            // The cumulative id ledger: every id the series has ever shown,
            // its last-sighted revision, and whether it is still in a tree.
            // Cumulative because retirement is forever — two adjacent
            // snapshots alone would forgive an id resurrected a commit later.
            var ledger: [UInt64: LedgerEntry] = [:]
            verifyTree(document, against: nil, ledger: &ledger, testCase.name)
            for chunk in lineChunks(testCase.source) {
                replayed += chunk
                let previous = document
                document = try previous.edit(replayed)
                #expect(document.series == previous.series, Comment(rawValue: testCase.name))

                // Equivalence: the edited document dumps byte-equal to a
                // one-shot parse of the same text.
                let reference = try Document(replayed, options: testCase.parseOptions.value)
                #expect(document.dump() == reference.dump(), Comment(rawValue: testCase.name))

                verifyTree(document, against: previous, ledger: &ledger, testCase.name)
            }
            #expect(document.dump() == testCase.expected, Comment(rawValue: testCase.name))
        }
    }
}

private struct LedgerEntry {
    var revision: UInt64
    var alive: Bool
}

/// Walks one snapshot against the cumulative ledger — the Swift double-walk,
/// mirroring the C harness's `er_walk_visit`. Per node: the id is nonzero
/// and unique within the tree, a child's revision never exceeds its
/// parent's, a never-seen id carries the new document revision, a retired id
/// never comes back, and a surviving id's revision is either its last
/// sighting — in which case its subtree value must equal the predecessor's —
/// or the new document revision, nothing in between and never less.
/// Afterwards, live ledger ids the walk did not meet are retired.
///
/// The subtree form of the (id, revision) promise is checked at each
/// TOPMOST unchanged node: its content dump covers every descendant's kind,
/// fields, and literal, and its (id, revision) walk pins the identities
/// underneath, so nodes inside a compared subtree need no comparison of
/// their own.
private func verifyTree(
    _ document: Document,
    against previous: Document?,
    ledger: inout [UInt64: LedgerEntry],
    _ name: String
) {
    let comment = Comment(rawValue: name)
    // Any minted or changed node bubbles a stamp up to the root, so whenever
    // this walk needs the new document revision — only ever at such a node —
    // the root's own revision IS that revision.
    let successorRevision = document.revision
    var seen: Set<UInt64> = []
    var parentRevisions: [UInt64] = []
    // Whether an ancestor's subtree has already been compared against the
    // predecessor, which covers this node too.
    var covered: [Bool] = []
    MarkupWalker().walk(document) { event, node, _ in
        guard event == .entering else {
            parentRevisions.removeLast()
            covered.removeLast()
            return
        }
        let id = node.id.rawValue
        var covers = covered.last ?? false
        defer {
            parentRevisions.append(node.revision)
            covered.append(covers)
        }
        #expect(id != 0, comment)
        #expect(!seen.contains(id), comment)
        seen.insert(id)
        if let parent = parentRevisions.last {
            #expect(node.revision <= parent, comment)
        }
        guard let entry = ledger[id] else {
            // A never-seen id is minted by this commit and carries its
            // revision.
            #expect(node.revision == successorRevision, comment)
            ledger[id] = LedgerEntry(revision: node.revision, alive: true)
            return
        }
        #expect(entry.alive, comment)
        #expect(node.revision >= entry.revision, comment)
        if node.revision != entry.revision {
            #expect(node.revision == successorRevision, comment)
        } else if let previous, !covers {
            // A topmost unchanged survivor: its whole subtree must be the
            // value the predecessor held for this id.
            if let before = previous.node(node.id) {
                #expect(
                    contentDump(previous, of: before) == contentDump(document, of: node),
                    comment
                )
                #expect(track(previous, of: before) == track(document, of: node), comment)
            } else {
                Issue.record(Comment(rawValue: "a live ledger id is missing from the predecessor tree: \(name)"))
            }
            covers = true
        }
        ledger[id] = LedgerEntry(revision: node.revision, alive: true)
    }
    for (id, entry) in ledger where entry.alive && !seen.contains(id) {
        ledger[id] = LedgerEntry(revision: entry.revision, alive: false)
    }
}

/// The subtree's canonical dump with every `scope=` field stripped: content
/// only, because a pure positional shift never changes a node's revision and
/// must not fail the unchanged-subtree comparison.
private func contentDump(_ document: Document, of node: any Markup) -> String {
    document.dump(of: node)
        .split(separator: "\n", omittingEmptySubsequences: false)
        .map { line -> String in
            var line = String(line)
            if let range = line.range(of: #"scope=\S+ "#, options: .regularExpression) {
                line.removeSubrange(range)
            }
            return line
        }
        .joined(separator: "\n")
}

/// The subtree's (id, revision) pairs in preorder.
private func track(_ document: Document, of node: any Markup) -> [[UInt64]] {
    var rows: [[UInt64]] = []
    MarkupWalker().walk(document, from: node) { event, current, _ in
        if event == .entering { rows.append([current.id.rawValue, current.revision]) }
    }
    return rows
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
    let tables: Bool
    let strikethrough: Bool
    let autolinks: Bool
    let taskLists: Bool
    let formulas: Bool
    let directives: Bool
    let crossLinks: Bool
    let embeds: Bool

    var value: ParseOptions {
        ParseOptions(
            smartPunctuation: smartPunctuation,
            footnotes: footnotes,
            tables: tables,
            strikethrough: strikethrough,
            autolinks: autolinks,
            taskLists: taskLists,
            formulas: formulas,
            directives: directives,
            crossLinks: crossLinks,
            embeds: embeds
        )
    }
}

private func flatten(_ document: Document) -> [any Markup] {
    var result: [any Markup] = []
    MarkupWalker().walk(document) { event, node, _ in
        if case .entering = event { result.append(node) }
    }
    return result
}

private func kindName(_ node: any Markup) -> String {
    String(describing: type(of: node))
}
