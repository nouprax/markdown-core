/// Produces the canonical diagnostic tree for immutable Markdown markup.
public enum TreeDumper {
    private struct Frame {
        var remainingChildren: Int
    }

    /// Returns the canonical diagnostic dump for `root` and its descendants.
    public static func dump(_ root: some Markup) -> String {
        var visitor = DumpVisitor()
        var frames: [Frame] = []
        var lines: [String] = []

        Walker().walk(root) { event, node in
            switch event {
            case .entering:
                let record = node.accept(&visitor)
                if frames.isEmpty {
                    lines.append(record.line)
                } else {
                    let parent = frames.count - 1
                    let prefix = frames.dropLast().map {
                        $0.remainingChildren > 0 ? "│   " : "    "
                    }.joined()
                    let connector = frames[parent].remainingChildren == 1 ? "└── " : "├── "
                    lines.append(prefix + connector + record.line)
                    frames[parent].remainingChildren -= 1
                }
                frames.append(Frame(remainingChildren: record.children))
            case .exiting:
                precondition(frames.last?.remainingChildren == 0)
                frames.removeLast()
            }
        }
        return lines.joined(separator: "\n") + "\n"
    }
}

extension Markup {
    /// Returns the canonical diagnostic dump for this markup subtree.
    public func dump() -> String { TreeDumper.dump(self) }
}

private struct DumpRecord {
    let line: String
    let children: Int
}

private struct DumpVisitor: MarkupVisitor {
    mutating func visit(_ node: DocumentRoot) -> DumpRecord {
        record("Document", node, children: node.content.count)
    }

    mutating func visit(_ node: BlockQuote) -> DumpRecord {
        record("BlockQuote", node, children: node.content.count)
    }

    mutating func visit(_ node: Paragraph) -> DumpRecord {
        record("Paragraph", node, children: node.content.count)
    }

    mutating func visit(_ node: Heading) -> DumpRecord {
        record("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
    }

    mutating func visit(_ node: ThematicBreak) -> DumpRecord {
        record("ThematicBreak", node)
    }

    mutating func visit(_ node: MarkdownCore.List) -> DumpRecord {
        record(
            "List",
            node,
            fields: [
                "flavor=\(node.flavor.rawValue)",
                "start=\(node.start.map(String.init) ?? "null")",
                "tight=\(boolean(node.tight))",
            ],
            children: node.items.count
        )
    }

    mutating func visit(_ node: ListItem) -> DumpRecord {
        record(
            "ListItem",
            node,
            fields: ["checked=\(node.checked.map(boolean) ?? "null")"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: CodeBlock) -> DumpRecord {
        record(
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

    mutating func visit(_ node: HTMLBlock) -> DumpRecord {
        record("HTMLBlock", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock) -> DumpRecord {
        record("FormulaBlock", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: Table) -> DumpRecord {
        let alignments = node.alignments.map(\.rawValue).joined(separator: ",")
        return record(
            "Table",
            node,
            fields: ["alignments=[\(alignments)]"],
            children: 1 + node.rows.count
        )
    }

    mutating func visit(_ node: DirectiveBlock) -> DumpRecord {
        record(
            "DirectiveBlock",
            node,
            fields: directiveFields(node.name, node.attributes),
            children: (node.label == nil ? 0 : 1) + node.content.count
        )
    }

    mutating func visit(_ node: DirectiveLabel) -> DumpRecord {
        record("DirectiveLabel", node, children: node.content.count)
    }

    mutating func visit(_ node: FootnoteDefinition) -> DumpRecord {
        record(
            "FootnoteDefinition",
            node,
            fields: association(node.label, node.identifier),
            children: node.content.count
        )
    }

    mutating func visit(_ node: ReferenceDefinition) -> DumpRecord {
        record(
            "ReferenceDefinition",
            node,
            fields: association(node.label, node.identifier) + [
                "destination=\(jsonString(node.destination))",
                "title=\(optionalString(node.title))",
            ]
        )
    }

    mutating func visit(_ node: LinkReference) -> DumpRecord {
        record(
            "LinkReference",
            node,
            fields: association(node.label, node.identifier) + ["form=\(formName(node.form))"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: ImageReference) -> DumpRecord {
        record(
            "ImageReference",
            node,
            fields: association(node.label, node.identifier) + ["form=\(formName(node.form))"],
            children: node.content.count
        )
    }

    /// The two fields five kinds carry identically, in contract order.
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

    mutating func visit(_ node: Text) -> DumpRecord {
        record("Text", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: SoftBreak) -> DumpRecord { record("SoftBreak", node) }

    mutating func visit(_ node: LineBreak) -> DumpRecord { record("LineBreak", node) }

    mutating func visit(_ node: Code) -> DumpRecord {
        record("Code", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: HTML) -> DumpRecord {
        record("HTML", node, fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: Formula) -> DumpRecord {
        record(
            "Formula",
            node,
            fields: ["mode=\(node.mode.rawValue)", "literal=\(jsonString(node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis) -> DumpRecord {
        record("Emphasis", node, children: node.content.count)
    }

    mutating func visit(_ node: Strong) -> DumpRecord {
        record("Strong", node, children: node.content.count)
    }

    mutating func visit(_ node: Strikethrough) -> DumpRecord {
        record("Strikethrough", node, children: node.content.count)
    }

    mutating func visit(_ node: Link) -> DumpRecord {
        record(
            "Link",
            node,
            fields: [
                "destination=\(optionalString(node.destination))",
                "title=\(optionalString(node.title))",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Image) -> DumpRecord {
        record(
            "Image",
            node,
            fields: ["source=\(optionalString(node.source))", "title=\(optionalString(node.title))"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Directive) -> DumpRecord {
        record(
            "Directive",
            node,
            fields: directiveFields(node.name, node.attributes),
            children: node.label == nil ? 0 : 1
        )
    }

    mutating func visit(_ node: FootnoteReference) -> DumpRecord {
        record("FootnoteReference", node, fields: association(node.label, node.identifier))
    }

    mutating func visit(_ node: TableRow) -> DumpRecord {
        record(
            "TableRow",
            node,
            fields: ["isHeader=\(boolean(node.isHeader))"],
            children: node.cells.count
        )
    }

    mutating func visit(_ node: TableCell) -> DumpRecord {
        record("TableCell", node, children: node.content.count)
    }

    private func record(
        _ kind: String,
        _ node: any Markup,
        fields: [String] = [],
        children: Int = 0
    ) -> DumpRecord {
        let fieldText = fields.isEmpty ? "" : " " + fields.joined(separator: " ")
        return DumpRecord(
            line: "\(kind) \(scope(node.scope))\(fieldText) children=\(children)",
            children: children
        )
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
