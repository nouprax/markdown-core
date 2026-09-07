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
        line(kind, scope: node.scope, fields: fields, children: children)
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
    func group(_ kind: String, children: Int) {
        emit("\(kind) children=\(children)")
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
        state.nested(node.content.count + node.footnotes.count) {
            node.content.forEach(state.dump)
            for footnote in node.footnotes { dumpFootnote(footnote) }
        }
    }

    private func dumpFootnote(_ value: Footnote) {
        state.line(
            "Footnote",
            scope: value.scope,
            fields: ["id=\(jsonString(value.id))"],
            children: value.content.count
        )
        state.nested(value.content.count) { value.content.forEach(state.dump) }
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
                "variant=\(node.variant?.rawValue ?? "null")",
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
            fields: ["marker=\(optionalString(node.marker))", "exampleLabel=\(optionalString(node.exampleLabel))"],
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
        let alignments = node.alignments.map(\.rawValue).joined(separator: ",")
        let count = 1 + node.rows.count
        state.line("Table", node, fields: ["alignments=[\(alignments)]"], children: count)
        state.nested(count) {
            state.dump(node.header)
            node.rows.forEach(state.dump)
        }
    }

    mutating func visit(_ node: DirectiveBlock) {
        state.line(
            "DirectiveBlock",
            node,
            fields: directiveFields(node.name, node.attributes),
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
            fields: ["dest=\(destinationString(node.dest))", "title=\(optionalString(node.title))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Directive) {
        state.line("Directive", node, fields: directiveFields(node.name, node.attributes))
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

    private func dumpCitation(_ value: Citation) {
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

    mutating func visit(_ node: TableRow) {
        state.line(
            "TableRow",
            node,
            fields: ["isHeader=\(boolean(node.isHeader))"],
            children: node.cells.count
        )
        state.nested(node.cells.count) { node.cells.forEach(state.dump) }
    }

    mutating func visit(_ node: TableCell) {
        state.line("TableCell", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    private func directiveFields(_ name: String, _ attributes: [DirectiveAttribute]?) -> [String] {
        guard let attributes else {
            return ["name=\(jsonString(name))", "attributes=null"]
        }
        let pairs = attributes.map { "\($0.name)=\(jsonString($0.value))" }.joined(separator: " ")
        return ["name=\(jsonString(name))", "attributes=[\(pairs)]"]
    }
}

private func scopeString(_ value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

private func boolean(_ value: Bool) -> String { value ? "true" : "false" }

/// A tagged value prints its branch and its named fields with no spaces.
private func referentString(_ value: CitationReferent) -> String {
    switch value {
    case .bib(let key, let mode): "bib(key=\(jsonString(key)),mode=\(mode.rawValue))"
    case .footnote(let id): "footnote(id=\(jsonString(id)))"
    }
}

/// A tagged value prints its branch and its named fields with no spaces.
private func destinationString(_ value: Destination) -> String {
    switch value {
    case .url(let url): "url(\(jsonString(url)))"
    case .cross(let path, let anchor): "cross(path=\(jsonString(path)),anchor=\(optionalString(anchor)))"
    }
}

private func orderedListDelimiter(_ value: OrderedListDelimiter?) -> String {
    switch value {
    case .period: "period"
    case .parenthesis(let closed): "parenthesis(closed=\(boolean(closed)))"
    case .default: "default"
    case nil: "null"
    }
}

private func optionalString(_ value: String?) -> String {
    value.map(jsonString) ?? "null"
}

private func jsonString(_ value: String) -> String {
    let hex = Array("0123456789abcdef")
    var result = "\""
    for scalar in value.unicodeScalars {
        switch scalar.value {
        case 0x22: result += "\\\""
        case 0x5c: result += "\\\\"
        case 0x08: result += "\\b"
        case 0x0c: result += "\\f"
        case 0x0a: result += "\\n"
        case 0x0d: result += "\\r"
        case 0x09: result += "\\t"
        case 0..<0x20:
            result += "\\u00\(hex[Int(scalar.value >> 4)])\(hex[Int(scalar.value & 0xf)])"
        default: result.unicodeScalars.append(scalar)
        }
    }
    return result + "\""
}
