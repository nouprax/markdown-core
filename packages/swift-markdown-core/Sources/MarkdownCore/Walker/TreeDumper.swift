/// Produces the canonical diagnostic tree for immutable Markdown markup.
public enum TreeDumper {
    private struct Frame {
        var remainingChildren: Int
    }

    /// Returns the canonical diagnostic dump for the whole document.
    public static func dump(_ document: Document) -> String {
        dump(document, of: document)
    }

    /// Returns the canonical diagnostic dump for the subtree rooted at
    /// `node`. Scopes print with the subtree as origin: the root's start
    /// line becomes line 1. Position-free markers (0:0..0:0) print
    /// unchanged.
    public static func dump(_ document: Document, of node: some Markup) -> String {
        let origin = document.scope(of: node).start.line
        let offset = origin > 0 ? origin - 1 : 0
        var visitor = DumpVisitor()
        var frames: [Frame] = []
        var lines: [String] = []

        Walker().walk(document, from: node) { event, current, resolved in
            switch event {
            case .entering:
                let record = current.accept(&visitor)
                let line =
                    "\(record.kind) \(text(for: resolved, rebasedBy: offset))"
                    + "\(record.fieldText) children=\(record.children)"
                if frames.isEmpty {
                    lines.append(line)
                } else {
                    let parent = frames.count - 1
                    let prefix = frames.dropLast().map {
                        $0.remainingChildren > 0 ? "│   " : "    "
                    }.joined()
                    let connector = frames[parent].remainingChildren == 1 ? "└── " : "├── "
                    lines.append(prefix + connector + line)
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

    private static func text(for scope: Scope, rebasedBy offset: Int32) -> String {
        let start = scope.start.line > 0 ? scope.start.line - offset : scope.start.line
        let end = scope.end.line > 0 ? scope.end.line - offset : scope.end.line
        return "scope=\(start):\(scope.start.column)..\(end):\(scope.end.column)"
    }
}

extension Document {
    /// Returns the canonical diagnostic dump for this document.
    public func dump() -> String { TreeDumper.dump(self) }

    /// Returns the canonical diagnostic dump for the subtree rooted at
    /// `node`, with the subtree as scope origin.
    public func dump(of node: some Markup) -> String { TreeDumper.dump(self, of: node) }
}

private struct DumpRecord {
    let kind: String
    let fieldText: String
    let children: Int
}

private struct DumpVisitor: MarkupVisitor {
    mutating func visit(_ node: Document) -> DumpRecord {
        record("Document", children: node.children.count)
    }

    mutating func visit(_ node: BlockQuote) -> DumpRecord {
        record("BlockQuote", children: node.children.count)
    }

    mutating func visit(_ node: Paragraph) -> DumpRecord {
        record("Paragraph", children: node.children.count)
    }

    mutating func visit(_ node: Heading) -> DumpRecord {
        record("Heading", fields: ["level=\(node.level)"], children: node.children.count)
    }

    mutating func visit(_ node: ThematicBreak) -> DumpRecord {
        record("ThematicBreak")
    }

    mutating func visit(_ node: MarkdownCore.List) -> DumpRecord {
        record(
            "List",
            fields: [
                "flavor=\(node.flavor.rawValue)",
                "start=\(node.start.map(String.init) ?? "null")",
                "tight=\(boolean(node.isTight))",
            ],
            children: node.children.count
        )
    }

    mutating func visit(_ node: ListItem) -> DumpRecord {
        record(
            "ListItem",
            fields: ["checked=\(node.isChecked.map(boolean) ?? "null")"],
            children: node.children.count
        )
    }

    mutating func visit(_ node: CodeBlock) -> DumpRecord {
        record(
            "CodeBlock",
            fields: [
                "mode=standalone",
                "info=\(optionalString(node.info))",
                "language=\(optionalString(node.language))",
                "literal=\(jsonString(node.literal))",
                "fenced=\(boolean(node.isFenced))",
                "closed=\(boolean(node.isClosed))",
            ]
        )
    }

    mutating func visit(_ node: HTMLBlock) -> DumpRecord {
        record("HTMLBlock", fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock) -> DumpRecord {
        record(
            "FormulaBlock",
            fields: ["mode=\(node.mode.rawValue)", "literal=\(jsonString(node.literal))"]
        )
    }

    mutating func visit(_ node: Table) -> DumpRecord {
        let alignments = node.alignments.map(\.rawValue).joined(separator: ",")
        return record(
            "Table",
            fields: ["alignments=[\(alignments)]"],
            children: 1 + node.rows.count
        )
    }

    mutating func visit(_ node: DirectiveBlock) -> DumpRecord {
        record(
            "DirectiveBlock",
            fields: directiveFields(node.mode, node.name, node.attributes, node.labelCount),
            children: node.children.count
        )
    }

    mutating func visit(_ node: FootnoteDefinition) -> DumpRecord {
        record(
            "FootnoteDefinition",
            fields: ["id=\(jsonString(node.label))"],
            children: node.children.count
        )
    }

    mutating func visit(_ node: Text) -> DumpRecord {
        record("Text", fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: SoftBreak) -> DumpRecord { record("SoftBreak") }

    mutating func visit(_ node: LineBreak) -> DumpRecord { record("LineBreak") }

    mutating func visit(_ node: Code) -> DumpRecord {
        record("Code", fields: ["mode=embedded", "literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: HTML) -> DumpRecord {
        record("HTML", fields: ["literal=\(jsonString(node.literal))"])
    }

    mutating func visit(_ node: Formula) -> DumpRecord {
        record(
            "Formula",
            fields: ["mode=\(node.mode.rawValue)", "literal=\(jsonString(node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis) -> DumpRecord {
        record("Emphasis", children: node.children.count)
    }

    mutating func visit(_ node: Strong) -> DumpRecord {
        record("Strong", children: node.children.count)
    }

    mutating func visit(_ node: Strikethrough) -> DumpRecord {
        record("Strikethrough", children: node.children.count)
    }

    mutating func visit(_ node: Link) -> DumpRecord {
        record(
            "Link",
            fields: [
                "destination=\(optionalString(node.destination))",
                "title=\(optionalString(node.title))",
            ],
            children: node.children.count
        )
    }

    mutating func visit(_ node: Image) -> DumpRecord {
        record(
            "Image",
            fields: ["source=\(optionalString(node.source))", "title=\(optionalString(node.title))"],
            children: node.children.count
        )
    }

    mutating func visit(_ node: Directive) -> DumpRecord {
        record(
            "Directive",
            fields: directiveFields(node.mode, node.name, node.attributes, node.labelCount),
            children: node.children.count
        )
    }

    mutating func visit(_ node: FootnoteReference) -> DumpRecord {
        record("FootnoteReference", fields: ["id=\(jsonString(node.label))"])
    }

    mutating func visit(_ node: TableRow) -> DumpRecord {
        record(
            "TableRow",
            fields: ["isHeader=\(boolean(node.isHeader))"],
            children: node.cells.count
        )
    }

    mutating func visit(_ node: TableCell) -> DumpRecord {
        record("TableCell", children: node.content.count)
    }

    private func record(
        _ kind: String,
        fields: [String] = [],
        children: Int = 0
    ) -> DumpRecord {
        DumpRecord(
            kind: kind,
            fieldText: fields.isEmpty ? "" : " " + fields.joined(separator: " "),
            children: children
        )
    }

    private func directiveFields(
        _ mode: PlacementMode,
        _ name: String,
        _ attributes: String?,
        _ labelCount: Int?
    ) -> [String] {
        [
            "mode=\(mode.rawValue)",
            "name=\(jsonString(name))",
            "attributes=\(optionalString(attributes))",
            "label=\(labelCount.map(String.init) ?? "null")",
        ]
    }
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
