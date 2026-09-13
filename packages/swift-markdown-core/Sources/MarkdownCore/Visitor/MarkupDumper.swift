// Keep the complete canonical dump together so its value-formatting helpers stay file-private.
// swiftlint:disable file_length

/// Produces the canonical debug tree for immutable Markdown markup.
public enum MarkupDumper {
    /// Returns the canonical debug dump for `root` and its owned markup.
    public static func dump(_ root: some Markup) -> String {
        let state = DumpState()
        state.visit(root)
        return state.result
    }
}

extension Markup {
    /// Returns the canonical debug dump for this markup subtree.
    public func dump() -> String { MarkupDumper.dump(self) }
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

    /// Writes the common source extent and fields of a node line.
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

    mutating func visit(_ node: Document, phase: MarkupWalkPhase) {
        // The footnotes are node lines after the content, each nesting its
        // own content; `children` counts the content alone.
        state.line("Document", node, children: node.content.count)
        state.nested(node.content.count + node.footnotes.count + node.specimens.count + (node.metadata == nil ? 0 : 1))
        {
            if let metadata = node.metadata { state.visit(metadata) }
            node.content.forEach(state.visit)
            for footnote in node.footnotes { state.visit(footnote) }
            for specimen in node.specimens { state.visit(specimen) }
        }
    }

    mutating func visit(_ node: Callout, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: Paragraph, phase: MarkupWalkPhase) {
        state.line("Paragraph", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Heading, phase: MarkupWalkPhase) {
        state.line("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: ThematicBreak, phase: MarkupWalkPhase) { state.line("ThematicBreak", node) }

    mutating func visit(_ node: MarkdownCore.List, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: ListItem, phase: MarkupWalkPhase) {
        state.line(
            "ListItem",
            node,
            fields: ["marker=\(dump(optional: node.marker))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: CodeBlock, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: HTMLBlock, phase: MarkupWalkPhase) {
        state.line("HTMLBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock, phase: MarkupWalkPhase) {
        state.line("FormulaBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Table, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: DefinitionList, phase: MarkupWalkPhase) {
        state.line("DefinitionList", node, fields: [], children: node.definitions.count)
        state.nested(node.definitions.count) { for definition in node.definitions { state.visit(definition) } }
    }

    mutating func visit(_ node: Definition, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: DirectiveBlock, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: DirectiveLabel, phase: MarkupWalkPhase) {
        state.line("DirectiveLabel", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }
}

extension DumpVisitor {
    mutating func visit(_ node: Text, phase: MarkupWalkPhase) {
        state.line("Text", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: SoftBreak, phase: MarkupWalkPhase) { state.line("SoftBreak", node) }

    mutating func visit(_ node: LineBreak, phase: MarkupWalkPhase) { state.line("LineBreak", node) }

    mutating func visit(_ node: Code, phase: MarkupWalkPhase) {
        state.line("Code", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: HTML, phase: MarkupWalkPhase) {
        state.line("HTML", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: CrossLink, phase: MarkupWalkPhase) {
        state.line(
            "CrossLink",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))"]
        )
    }

    mutating func visit(_ node: CrossEmbedded, phase: MarkupWalkPhase) {
        state.line(
            "CrossEmbedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ]
        )
    }

    mutating func visit(_ node: Comment, phase: MarkupWalkPhase) {
        state.line("Comment", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Formula, phase: MarkupWalkPhase) {
        state.line(
            "Formula",
            node,
            fields: ["mode=\(node.mode.rawValue)", "literal=\(dump(escaped: node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis, phase: MarkupWalkPhase) {
        state.line("Emphasis", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Strong, phase: MarkupWalkPhase) {
        state.line("Strong", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Strikethrough, phase: MarkupWalkPhase) {
        state.line("Strikethrough", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Mark, phase: MarkupWalkPhase) {
        state.line("Mark", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Insertion, phase: MarkupWalkPhase) {
        state.line("Insertion", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Span, phase: MarkupWalkPhase) {
        state.line("Span", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Superscript, phase: MarkupWalkPhase) {
        state.line("Superscript", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Subscript, phase: MarkupWalkPhase) {
        state.line("Subscript", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Link, phase: MarkupWalkPhase) {
        state.line(
            "Link",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "title=\(dump(optional: node.title))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Embedded, phase: MarkupWalkPhase) {
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

    mutating func visit(_ node: Directive, phase: MarkupWalkPhase) {
        state.line("Directive", node, fields: ["name=\(dump(escaped: node.name))"])
        state.nested(node.label == nil ? 0 : 1) {
            if let label = node.label { state.visit(label) }
        }
    }

    mutating func visit(_ node: Cite, phase: MarkupWalkPhase) {
        // The items are value lines under the cite, and `children` counts
        // them; each item's affixes are groups whose nodes nest below them.
        state.line("Cite", node, children: node.citations.count)
        state.nested(node.citations.count) {
            for citation in node.citations { state.visit(citation) }
        }
    }

    mutating func visit(_ node: TableCaption, phase: MarkupWalkPhase) {
        state.line("TableCaption", node, children: node.content.count)
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: TableRow, phase: MarkupWalkPhase) {
        state.line(
            "TableRow",
            node,
            children: node.cells.count
        )
        state.nested(node.cells.count) { node.cells.forEach(state.visit) }
    }

    mutating func visit(_ node: TableCell, phase: MarkupWalkPhase) {
        state.line(
            "TableCell",
            node,
            fields: ["rowspan=\(node.rowspan)", "colspan=\(node.colspan)"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Metadata, phase: MarkupWalkPhase) {
        state.line(
            "Metadata",
            node,
            fields: [
                "name=\(node.name.map(dump(metadata:)) ?? "null")",
                "title=\(node.title.map(dump(metadata:)) ?? "null")",
                "subtitle=\(node.subtitle.map(dump(metadata:)) ?? "null")",
                "time=\(node.time.map(dump(metadata:)) ?? "null")",
                "date=\(node.date.map(dump(metadata:)) ?? "null")",
                "authors=\(node.authors.map(dump(metadata:)) ?? "null")",
                "keywords=\(node.keywords.map(dump(metadata:)) ?? "null")",
                "abstract=\(node.abstract.map(dump(metadata:)) ?? "null")",
                "state=\(node.state.map(dump(metadata:)) ?? "null")",
                "comment=\(node.comment.map(dump(metadata:)) ?? "null")",
            ],
            children: 0
        )
    }

    mutating func visit(_ node: Footnote, phase: MarkupWalkPhase) {
        state.line(
            "Footnote",
            node,
            fields: ["id=\(dump(escaped: node.id))"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Specimen, phase: MarkupWalkPhase) {
        state.line(
            "Specimen",
            node,
            fields: ["id=\(dump(optional: node.id))", "start=\(node.start.map(String.init) ?? "null")"],
            children: node.content.count
        )
        state.nested(node.content.count) { node.content.forEach(state.visit) }
    }

    mutating func visit(_ node: Citation, phase: MarkupWalkPhase) {
        state.line(
            "Citation",
            node,
            fields: ["referent=\(dump(referent: node.referent))"],
            children: 0
        )
        state.nested(2) {
            state.group("CitationPrefix", children: node.prefix.count)
            state.nested(node.prefix.count) { node.prefix.forEach(state.visit) }
            state.group("CitationSuffix", children: node.suffix.count)
            state.nested(node.suffix.count) { node.suffix.forEach(state.visit) }
        }
    }
}

// Canonical spellings for values in the debug dump.
private func dump(scope value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

private func dump(boolean value: Bool) -> String { value ? "true" : "false" }

/// A tagged value prints its branch and its named fields with no spaces.
private func dump(referent value: CitationReferent) -> String {
    switch value {
    case .bib(let key, let mode): "bib(key=\(dump(escaped: key)),mode=\(mode.rawValue))"
    case .footnote(let id): "footnote(id=\(dump(escaped: id)))"
    case .specimen(let id): "specimen(id=\(dump(escaped: id)))"
    }
}

/// A tagged value prints its branch and its named fields with no spaces.
private func dump(destination value: Destination) -> String {
    switch value {
    case .url(let url): "url(\(dump(escaped: url)))"
    case .cross(let path, let anchor): "cross(path=\(dump(escaped: path)),anchor=\(dump(optional: anchor)))"
    }
}

private func dump(delimiter value: OrderedListDelimiter?) -> String {
    switch value {
    case .period: "period"
    case .parenthesis(let closed): "parenthesis(closed=\(dump(boolean: closed)))"
    case .default: "default"
    case nil: "null"
    }
}

private func dump(variant value: OrderedListVariant?) -> String {
    switch value {
    case .decimal: "decimal"
    case .alpha(let lowercased): "alpha(lowercased=\(dump(boolean: lowercased)))"
    case .roman(let lowercased): "roman(lowercased=\(dump(boolean: lowercased)))"
    case .default: "default"
    case nil: "null"
    }
}

private func dump(optional value: String?) -> String {
    value.map(dump(escaped:)) ?? "null"
}

/// Quotes a string and escapes its contents using JSON string-literal rules.
private func dump(escaped value: String) -> String {
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

/// Normalize the runtime's shortest round-trip digits to the dump's decimal
/// notation in [1e-6, 1e21), scientific notation outside that interval.
private func dump(decimal value: Double) -> String {
    let parts = String(value).lowercased().split(separator: "e")
    let mantissa = parts[0].split(separator: ".")
    var digits = String(mantissa.joined())
    let exponent = parts.count == 2 ? Int(parts[1]) : 0
    guard let exponent else { preconditionFailure("invalid runtime double exponent") }
    var point = mantissa[0].count + exponent
    while digits.first == "0" {
        digits.removeFirst()
        point -= 1
    }
    while digits.last == "0" { digits.removeLast() }
    if point <= -6 || point > 21 {
        let tail = digits.dropFirst()
        return String(digits.prefix(1)) + (tail.isEmpty ? "" : "." + tail) + "e" + (point > 0 ? "+" : "")
            + String(point - 1)
    }
    if point <= 0 { return "0." + String(repeating: "0", count: -point) + digits }
    if point >= digits.count { return digits + String(repeating: "0", count: point - digits.count) }
    let index = digits.index(digits.startIndex, offsetBy: point)
    return String(digits[..<index]) + "." + digits[index...]
}

private func dump(attributes value: Attributes) -> String {
    "{"
        + (value.classes.map { "." + dump(attributeClass: $0) }
        + value.records.map { "\($0.name)=\(dump(escaped: $0.value))" })
        .joined(separator: " ") + "}"
}

private func dump(metadata value: MetadataValue) -> String {
    switch value {
    case .scalar(let scalar):
        let text: String
        switch scalar {
        case .null: text = "null"
        case .bool(let value): text = "bool(\(dump(boolean: value)))"
        case .number(let value): text = "number(\(dump(escaped: value)))"
        case .text(let value): text = "text(\(dump(escaped: value)))"
        }
        return "scalar(\(text))"
    case .list(let items):
        return "list(["
            + items.map { item in
                switch item {
                case .number(let value): return "number(\(dump(escaped: value)))"
                case .text(let value): return "text(\(dump(escaped: value)))"
                }
            }.joined(separator: ",") + "])"
    }
}

private func dump(attributeClass value: String) -> String {
    let plain =
        !value.isEmpty
        && value.utf8.allSatisfy {
            $0 > 32 && $0 < 127 && ![34, 92, 123, 125, 91, 93, 40, 41, 61].contains($0)
        }
    return plain ? value : dump(escaped: value)
}

private func dump(dimensions value: Dimensions?) -> String {
    guard let value else { return "null" }
    return "(width=\(value.width),height=\(value.height.map(String.init) ?? "null"))"
}
