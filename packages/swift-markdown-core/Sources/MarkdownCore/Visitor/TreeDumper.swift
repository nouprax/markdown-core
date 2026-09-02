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
        let fieldText = fields.isEmpty ? "" : " " + fields.joined(separator: " ")
        let text = "\(kind) \(scope(node.scope))\(fieldText) children=\(children)"
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
        state.line("Document", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: BlockQuote) {
        state.line("BlockQuote", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.dump) }
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
            fields: ["checked=\(node.checked.map(boolean) ?? "null")"],
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

    mutating func visit(_ node: FootnoteDefinition) {
        state.line(
            "FootnoteDefinition",
            node,
            fields: association(node.label, node.identifier),
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: ReferenceDefinition) {
        state.line(
            "ReferenceDefinition",
            node,
            fields: association(node.label, node.identifier) + [
                "destination=\(jsonString(node.destination))",
                "title=\(optionalString(node.title))",
            ]
        )
    }

    mutating func visit(_ node: LinkReference) {
        state.line(
            "LinkReference",
            node,
            fields: association(node.label, node.identifier) + ["form=\(formName(node.form))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: ImageReference) {
        state.line(
            "ImageReference",
            node,
            fields: association(node.label, node.identifier) + ["form=\(formName(node.form))"],
            children: node.content.count
        )
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
            fields: [
                "destination=\(jsonString(node.destination))",
                "title=\(optionalString(node.title))",
            ],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.dump) }
    }

    mutating func visit(_ node: Image) {
        state.line(
            "Image",
            node,
            fields: ["source=\(jsonString(node.source))", "title=\(optionalString(node.title))"],
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

    mutating func visit(_ node: FootnoteReference) {
        state.line("FootnoteReference", node, fields: association(node.label, node.identifier))
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

    private func association(_ label: String, _ identifier: String) -> [String] {
        ["label=\(jsonString(label))", "identifier=\(jsonString(identifier))"]
    }

    private func formName(_ form: ReferenceForm) -> String {
        switch form {
        case .full: "full"
        case .collapsed: "collapsed"
        case .shortcut: "shortcut"
        }
    }

    private func directiveFields(_ name: String, _ attributes: [DirectiveAttribute]?) -> [String] {
        guard let attributes else {
            return ["name=\(jsonString(name))", "attributes=null"]
        }
        let pairs = attributes.map { "\($0.name)=\(jsonString($0.value))" }.joined(separator: " ")
        return ["name=\(jsonString(name))", "attributes=[\(pairs)]"]
    }
}

private func scope(_ value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

private func boolean(_ value: Bool) -> String { value ? "true" : "false" }

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
