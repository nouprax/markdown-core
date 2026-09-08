/// Produces the canonical debug tree for immutable Markdown markup.
public enum TreeDumper {
    /// Returns the canonical debug dump for `root` and its owned markup.
    public static func dump(_ root: some Markup) -> String {
        let state = DumpState()
        state.dump(root)
        return state.result
    }
}

extension Markup {
    /// Returns the canonical debug dump for this markup subtree.
    public func dump() -> String { TreeDumper.dump(self) }
}

private final class DumpState {
    private struct Frame {
        var remainingNodes: Int
    }

    private var frames: [Frame] = []
    private var lines: [String] = []

    var result: String { lines.joined(separator: "\n") + "\n" }

    func dump(_ node: any Markup) {
        var visitor = DumpVisitor(state: self)
        node.accept(&visitor)
    }

    func line(
        _ kind: String,
        _ node: any Markup,
        fields: [String] = [],
        children: Int = 0
    ) {
        line(
            kind,
            scope: node.scope,
            fields: ["anchor=\(optionalString(node.anchor))", "attributes=\(attributesString(node.attributes))"]
                + fields,
            children: children
        )
    }

    /// A value line has the node line's shape: a scoped value prints like a node.
    func line(
        _ kind: String,
        scope: Scope,
        fields: [String],
        children: Int
    ) {
        let fieldText = fields.isEmpty ? "" : " " + fields.joined(separator: " ")
        emit("\(kind) \(scopeString(scope))\(fieldText) children=\(children)")
    }

    /// A group line nests a node-valued list under its owner: `Kind children=N`
    /// with no scope and no fields. The caller opens the list's own nesting.
    func group(_ kind: String, children: Int, fields: [String] = []) {
        let fieldText = fields.isEmpty ? "" : " " + fields.joined(separator: " ")
        emit("\(kind)\(fieldText) children=\(children)")
    }

    private func emit(_ text: String) {
        guard !frames.isEmpty else {
            lines.append(text)
            return
        }

        let parent = frames.count - 1
        let prefix = frames.dropLast().map { $0.remainingNodes > 0 ? "│   " : "    " }.joined()
        let connector = frames[parent].remainingNodes == 1 ? "└── " : "├── "
        lines.append(prefix + connector + text)
        frames[parent].remainingNodes -= 1
    }

    func nested(_ count: Int, body: () -> Void) {
        frames.append(Frame(remainingNodes: count))
        body()
        precondition(frames.removeLast().remainingNodes == 0)
    }
}

/// Each visit emits exactly that node. It also chooses which structural
/// content and node-valued fields to dump; there is no generic tree walker.
private struct DumpVisitor: MarkupVisitor {
    let state: DumpState

    mutating func visit(_ node: Document) {
        // The footnotes are value lines after the content, each nesting its
        // own content; `children` counts the content alone.
        state.line("Document", node, children: node.content.count)
        state.nested(node.content.count + node.footnotes.count + node.specimens.count + (node.metadata == nil ? 0 : 1))
        {
            if let metadata = node.metadata { dumpMetadata(metadata) }
            node.content.forEach(state.dump)
            for footnote in node.footnotes { dumpFootnote(footnote) }
            for specimen in node.specimens { dumpSpecimen(specimen) }
        }
    }

    mutating func visit(_ node: Callout) {
        state.line(
            "Callout",
            node,
            fields: ["variant=\(optionalString(node.variant))", "collapsed=\(node.collapsed.map(boolean) ?? "null")"],
            children: node.content.count
        )
        // A non-null title is a `Title` group before the content; a
        // null one prints nothing. Neither is counted by `children`.
        state.nested(node.content.count + (node.title == nil ? 0 : 1)) {
            if let title = node.title {
                state.group("Title", children: title.count)
                state.nested(title.count) { title.forEach(state.dump) }
            }
            node.content.forEach(state.dump)
        }
    }

    mutating func visit(_ node: Paragraph) {
        state.line("Paragraph", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Heading) {
        state.line("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: ThematicBreak) { state.line("ThematicBreak", node) }

    mutating func visit(_ node: MarkdownCore.List) {
        state.line(
            "List",
            node,
            fields: [
                "flavor=\(node.flavor.rawValue)",
                "start=\(node.start.map(String.init) ?? "null")",
                "variant=\(orderedListVariant(node.variant))",
                "delimiter=\(orderedListDelimiter(node.delimiter))",
                "tight=\(boolean(node.tight))",
            ],
            children: node.items.count
        )
        state.nested(node.items.count) { node.items.forEach(state.dump) }
    }

    mutating func visit(_ node: ListItem) {
        state.line(
            "ListItem",
            node,
            fields: ["marker=\(optionalString(node.marker))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: CodeBlock) {
        state.line(
            "CodeBlock",
            node,
            fields: [
                "info=\(optionalString(node.info))",
                "language=\(optionalString(node.language))",
                "literal=\(jsonString(node.literal))",
                "fenced=\(boolean(node.fenced))",
                "closed=\(boolean(node.closed))",
            ]
        )
    }

    mutating func visit(_ node: HTMLBlock) {
        state.line("HTMLBlock", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock) {
        state.line("FormulaBlock", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: Table) {
        let columns = node.columns.map { "\($0.alignment.rawValue):\($0.relative.map(decimal) ?? "null")" }.joined(
            separator: ","
        )
        let count = node.head.count + node.content.count + node.foot.count
        state.line("Table", node, fields: ["columns=[\(columns)]"], children: count)
        state.nested(3) {
            for (name, rows) in [("TableHead", node.head), ("TableBody", node.content), ("TableFoot", node.foot)] {
                state.group(name, children: rows.count)
                state.nested(rows.count) { rows.forEach(state.dump) }
            }
        }
    }

    mutating func visit(_ node: DirectiveBlock) {
        state.line(
            "DirectiveBlock",
            node,
            fields: ["name=\(jsonString(node.name))"],
            children: node.content.count
        )
        state.nested(node.content.count + (node.label == nil ? 0 : 1)) {
            if let label = node.label { state.dump(label) }
            node.content.forEach(state.dump)
        }
    }

    mutating func visit(_ node: DirectiveLabel) {
        state.line("DirectiveLabel", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Text) {
        state.line("Text", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: SoftBreak) { state.line("SoftBreak", node) }

    mutating func visit(_ node: LineBreak) { state.line("LineBreak", node) }

    mutating func visit(_ node: Code) {
        state.line("Code", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: HTML) {
        state.line("HTML", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: CrossLink) {
        state.line(
            "CrossLink",
            node,
            fields: [
                "embedded=\(node.embedded)", "dest=\(destinationString(node.dest))",
                "label=\(optionalString(node.label))",
            ]
        )
    }

    mutating func visit(_ node: Comment) {
        state.line("Comment", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: Formula) {
        state.line(
            "Formula",
            node,
            fields: ["mode=\(node.mode.rawValue)", "literal=\(jsonString(node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis) {
        state.line("Emphasis", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Strong) {
        state.line("Strong", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Strikethrough) {
        state.line("Strikethrough", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }
    mutating func visit(_ node: Mark) {
        state.line("Mark", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Link) {
        state.line(
            "Link",
            node,
            fields: ["dest=\(destinationString(node.dest))", "title=\(optionalString(node.title))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Image) {
        state.line(
            "Image",
            node,
            fields: [
                "dest=\(destinationString(node.dest))", "title=\(optionalString(node.title))",
                "width=\(node.width.map(String.init) ?? "null")", "height=\(node.height.map(String.init) ?? "null")",
            ],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Directive) {
        state.line("Directive", node, fields: ["name=\(jsonString(node.name))"])
        state.nested(node.label == nil ? 0 : 1) {
            if let label = node.label { state.dump(label) }
        }
    }

    mutating func visit(_ node: Cite) {
        // The items are value lines under the cite, and `children` counts
        // them; each item's affixes are groups whose nodes nest below them.
        state.line("Cite", node, children: node.citations.count)
        state.nested(node.citations.count) {
            for citation in node.citations { dumpCitation(citation) }
        }
    }

    mutating func visit(_ node: TableRow) {
        state.line(
            "TableRow",
            node,
            children: node.cells.count
        )
        state.nested(node.cells.count) { node.cells.forEach(state.dump) }
    }

    mutating func visit(_ node: TableCell) {
        state.line(
            "TableCell",
            node,
            fields: ["rowspan=\(node.rowspan)", "colspan=\(node.colspan)"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

}

// Owned values have their own dump shape and do not participate in Markup visitation.
extension DumpVisitor {
    fileprivate func dumpMetadata(_ value: Metadata) {
        state.line("Metadata", scope: value.scope, fields: [], children: value.content.count)
        state.nested(value.content.count) {
            for content in value.content {
                switch content {
                case .comment(let text):
                    state.group("MetadataContent", children: 0, fields: ["value=comment(\(jsonString(text)))"])
                case .data(let record):
                    state.line(
                        "MetadataRecord",
                        scope: record.scope,
                        fields: ["name=\(jsonString(record.name))", "value=\(metadataValue(record.value))"],
                        children: 0
                    )
                }
            }
        }
    }

    fileprivate func dumpFootnote(_ value: Footnote) {
        state.line(
            "Footnote",
            scope: value.scope,
            fields: ["id=\(jsonString(value.id))"],
            children: value.content.count
        )
        state.nested(value.content.count) { value.content.forEach(state.dump) }
    }

    fileprivate func dumpSpecimen(_ value: Specimen) {
        state.line(
            "Specimen",
            scope: value.scope,
            fields: ["id=\(optionalString(value.id))", "start=\(value.start.map(String.init) ?? "null")"],
            children: value.content.count
        )
        state.nested(value.content.count) { value.content.forEach(state.dump) }
    }

    fileprivate func dumpCitation(_ value: Citation) {
        state.line(
            "Citation",
            scope: value.scope,
            fields: ["referent=\(referentString(value.referent))"],
            children: 0
        )
        state.nested(2) {
            state.group("CitationPrefix", children: value.prefix.count)
            state.nested(value.prefix.count) { value.prefix.forEach(state.dump) }
            state.group("CitationSuffix", children: value.suffix.count)
            state.nested(value.suffix.count) { value.suffix.forEach(state.dump) }
        }
    }
}
