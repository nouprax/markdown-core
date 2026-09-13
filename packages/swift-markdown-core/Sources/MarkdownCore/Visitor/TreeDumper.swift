/// Produces the canonical debug tree for immutable Markdown markup.
public enum TreeDumper {
    /// Returns the canonical debug dump for `root` and its owned markup.
    public static func dump(_ root: some Markup) -> String {
        let state = DumpState()
        state.visit(root)
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

    func visit(_ node: any Markup) {
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
            fields: ["anchor=\(dump(optional: node.anchor))", "attributes=\(dump(attributes: node.attributes))"]
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
        emit("\(kind) \(dump(scope: scope))\(fieldText) children=\(children)")
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
            if let metadata = node.metadata { dump(metadata: metadata, to: state) }
            node.content.forEach(state.visit)
            for footnote in node.footnotes { dump(footnote: footnote, to: state) }
            for specimen in node.specimens { dump(specimen: specimen, to: state) }
        }
    }

    mutating func visit(_ node: Callout) {
        state.line(
            "Callout",
            node,
            fields: [
                "variant=\(dump(optional: node.variant))", "collapsed=\(node.collapsed.map(dump(boolean:)) ?? "null")",
            ],
            children: node.content.count
        )
        // A non-null title is a `Title` group before the content; a
        // null one prints nothing. Neither is counted by `children`.
        state.nested(node.content.count + (node.title == nil ? 0 : 1)) {
            if let title = node.title {
                state.group("Title", children: title.count)
                state.nested(title.count) { title.forEach(state.visit) }
            }
            node.content.forEach(state.visit)
        }
    }

    mutating func visit(_ node: Paragraph) {
        state.line("Paragraph", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Heading) {
        state.line("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: ThematicBreak) { state.line("ThematicBreak", node) }

    mutating func visit(_ node: MarkdownCore.List) {
        state.line(
            "List",
            node,
            fields: [
                "flavor=\(node.flavor.rawValue)",
                "start=\(node.start.map(String.init) ?? "null")",
                "variant=\(dump(variant: node.variant))",
                "delimiter=\(dump(delimiter: node.delimiter))",
                "tight=\(dump(boolean: node.tight))",
            ],
            children: node.items.count
        )
        state.nested(node.items.count) { node.items.forEach(state.visit) }
    }

    mutating func visit(_ node: ListItem) {
        state.line(
            "ListItem",
            node,
            fields: ["marker=\(dump(optional: node.marker))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: CodeBlock) {
        state.line(
            "CodeBlock",
            node,
            fields: [
                "info=\(dump(optional: node.info))",
                "language=\(dump(optional: node.language))",
                "literal=\(dump(escaped: node.literal))",
                "fenced=\(dump(boolean: node.fenced))",
                "closed=\(dump(boolean: node.closed))",
            ]
        )
    }

    mutating func visit(_ node: HTMLBlock) {
        state.line("HTMLBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock) {
        state.line("FormulaBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Table) {
        let columns = node.columns.map { "\($0.flow.rawValue):\($0.relative.map(dump(decimal:)) ?? "null")" }.joined(
            separator: ","
        )
        let count = node.head.count + node.content.count + node.foot.count
        state.line("Table", node, fields: ["columns=[\(columns)]"], children: count)
        state.nested(3 + (node.caption == nil ? 0 : 1)) {
            if let caption = node.caption { state.visit(caption) }
            for (name, rows) in [("TableHead", node.head), ("TableBody", node.content), ("TableFoot", node.foot)] {
                state.group(name, children: rows.count)
                state.nested(rows.count) { rows.forEach(state.visit) }
            }
        }
    }

    mutating func visit(_ node: DefinitionList) {
        state.line("DefinitionList", node, fields: [], children: node.definitions.count)
        state.nested(node.definitions.count) { for definition in node.definitions { state.visit(definition) } }
    }

    mutating func visit(_ node: Definition) {
        state.line("Definition", node, fields: ["compact=\(node.compact)"], children: node.content.count)
        state.nested(node.content.count + 1) {
            state.group("DefinitionTerm", children: node.term.count)
            state.nested(node.term.count) { node.term.forEach(state.visit) }
            for body in node.content {
                state.group("DefinitionBody", children: body.count)
                state.nested(body.count) { body.forEach(state.visit) }
            }
        }
    }

    mutating func visit(_ node: DirectiveBlock) {
        state.line(
            "DirectiveBlock",
            node,
            fields: ["name=\(dump(optional: node.name))"],
            children: node.content.count
        )
        state.nested(node.content.count + (node.label == nil ? 0 : 1)) {
            if let label = node.label { state.visit(label) }
            node.content.forEach(state.visit)
        }
    }

    mutating func visit(_ node: DirectiveLabel) {
        state.line("DirectiveLabel", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Text) {
        state.line("Text", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: SoftBreak) { state.line("SoftBreak", node) }

    mutating func visit(_ node: LineBreak) { state.line("LineBreak", node) }

    mutating func visit(_ node: Code) {
        state.line("Code", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: HTML) {
        state.line("HTML", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: CrossLink) {
        state.line(
            "CrossLink",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))"]
        )
    }

    mutating func visit(_ node: CrossEmbedded) {
        state.line(
            "CrossEmbedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ]
        )
    }

    mutating func visit(_ node: Comment) {
        state.line("Comment", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Formula) {
        state.line(
            "Formula",
            node,
            fields: ["mode=\(node.mode.rawValue)", "literal=\(dump(escaped: node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis) {
        state.line("Emphasis", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Strong) {
        state.line("Strong", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Strikethrough) {
        state.line("Strikethrough", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
    mutating func visit(_ node: Mark) {
        state.line("Mark", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
    mutating func visit(_ node: Insertion) {
        state.line("Insertion", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
    mutating func visit(_ node: Span) {
        state.line("Span", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
    mutating func visit(_ node: Superscript) {
        state.line("Superscript", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
    mutating func visit(_ node: Subscript) {
        state.line("Subscript", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Link) {
        state.line(
            "Link",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "title=\(dump(optional: node.title))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Embedded) {
        state.line(
            "Embedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))", "title=\(dump(optional: node.title))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Directive) {
        state.line("Directive", node, fields: ["name=\(dump(escaped: node.name))"])
        state.nested(node.label == nil ? 0 : 1) {
            if let label = node.label { state.visit(label) }
        }
    }

    mutating func visit(_ node: Cite) {
        // The items are value lines under the cite, and `children` counts
        // them; each item's affixes are groups whose nodes nest below them.
        state.line("Cite", node, children: node.citations.count)
        state.nested(node.citations.count) {
            for citation in node.citations { dump(citation: citation, to: state) }
        }
    }

    mutating func visit(_ node: TableCaption) {
        state.line("TableCaption", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: TableRow) {
        state.line(
            "TableRow",
            node,
            children: node.cells.count
        )
        state.nested(node.cells.count) { node.cells.forEach(state.visit) }
    }

    mutating func visit(_ node: TableCell) {
        state.line(
            "TableCell",
            node,
            fields: ["rowspan=\(node.rowspan)", "colspan=\(node.colspan)"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

}

// Owned values have their own dump shape and do not participate in Markup visitation.
private func dump(metadata value: Metadata, to state: DumpState) {
    state.line(
        "Metadata",
        scope: value.scope,
        fields: [
            "name=\(value.name.map(dump(metadata:)) ?? "null")",
            "title=\(value.title.map(dump(metadata:)) ?? "null")",
            "subtitle=\(value.subtitle.map(dump(metadata:)) ?? "null")",
            "time=\(value.time.map(dump(metadata:)) ?? "null")",
            "date=\(value.date.map(dump(metadata:)) ?? "null")",
            "authors=\(value.authors.map(dump(metadata:)) ?? "null")",
            "keywords=\(value.keywords.map(dump(metadata:)) ?? "null")",
            "abstract=\(value.abstract.map(dump(metadata:)) ?? "null")",
            "state=\(value.state.map(dump(metadata:)) ?? "null")",
            "comment=\(value.comment.map(dump(metadata:)) ?? "null")",
        ],
        children: 0
    )
}

private func dump(footnote value: Footnote, to state: DumpState) {
    state.line(
        "Footnote",
        scope: value.scope,
        fields: ["id=\(dump(escaped: value.id))"],
        children: value.content.count
    )
    state.nested(value.content.count) { value.content.forEach(state.visit) }
}

private func dump(specimen value: Specimen, to state: DumpState) {
    state.line(
        "Specimen",
        scope: value.scope,
        fields: ["id=\(dump(optional: value.id))", "start=\(value.start.map(String.init) ?? "null")"],
        children: value.content.count
    )
    state.nested(value.content.count) { value.content.forEach(state.visit) }
}

private func dump(citation value: Citation, to state: DumpState) {
    state.line(
        "Citation",
        scope: value.scope,
        fields: ["referent=\(dump(referent: value.referent))"],
        children: 0
    )
    state.nested(2) {
        state.group("CitationPrefix", children: value.prefix.count)
        state.nested(value.prefix.count) { value.prefix.forEach(state.visit) }
        state.group("CitationSuffix", children: value.suffix.count)
        state.nested(value.suffix.count) { value.suffix.forEach(state.visit) }
    }
}
