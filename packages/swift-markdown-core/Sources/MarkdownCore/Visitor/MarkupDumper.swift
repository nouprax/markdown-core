// Keep the complete canonical dump together so its value-formatting helpers stay file-private.
// swiftlint:disable file_length

/// Produces the canonical debug tree for immutable Markdown markup.
public enum MarkupDumper {
    /// Returns the canonical debug dump for `root` and its owned markup.
    public static func dump(_ root: some Markup) -> String {
        let state = DumpState()
        var visitor = DumpVisitor(state: state)
        root.walk(with: &visitor)
        return state.result
    }
}

extension Markup {
    /// Returns the canonical debug dump for this markup subtree.
    public func dump() -> String { MarkupDumper.dump(self) }
}

private final class DumpState {
    // These frames describe output grouping only; they never retain or visit markup.
    private struct Frame {
        let groups: [(name: String?, count: Int)]
        var index = -1
        var remaining = 0
    }

    private var frames: [Frame] = []
    private var remainingNodes: [Int] = []

    // The connector segments of every open nesting level as UTF-8, and where
    // the segments above each depth end: a line copies its lead-in once and
    // extends the segments by the one its own connector decides, instead of
    // deriving every level again per line.
    private var prefix: [UInt8] = []
    private var prefixEnds: [Int] = [0]
    private var output: [UInt8] = []

    var result: String {
        // Every byte came from a Swift string, so the output is UTF-8 by construction.
        guard let text = String(validating: output, as: UTF8.self) else { preconditionFailure("dump is UTF-8") }
        return text
    }

    func start() {
        guard !frames.isEmpty else { return }
        advance()
        precondition(frames[frames.count - 1].remaining > 0)
        frames[frames.count - 1].remaining -= 1
    }

    func end() {
        advance()
        precondition(frames.removeLast().remaining == 0)
        precondition(remainingNodes.removeLast() == 0)
    }

    private func advance() {
        let depth = frames.count - 1
        while frames[depth].remaining == 0 && frames[depth].index < frames[depth].groups.count {
            let previous = frames[depth].index
            if previous >= 0 && frames[depth].groups[previous].name != nil {
                precondition(remainingNodes.removeLast() == 0)
            }
            frames[depth].index += 1
            guard frames[depth].index < frames[depth].groups.count else { return }
            let group = frames[depth].groups[frames[depth].index]
            if let name = group.name {
                emit("\(name) children=\(group.count)")
                remainingNodes.append(group.count)
            }
            frames[depth].remaining = group.count
        }
    }

    func line(
        _ kind: String,
        _ node: any Markup,
        fields: [String] = [],
        children: Int = 0,
        groups: [(name: String?, count: Int)]? = nil
    ) {
        line(
            kind,
            scope: node.scope,
            fields: ["anchor=\(dump(optional: node.anchor))", "attributes=\(dump(attributes: node.attributes))"]
                + fields,
            children: children
        )
        let groups = groups ?? [(nil, children)]
        frames.append(Frame(groups: groups))
        remainingNodes.append(groups.reduce(0) { $0 + ($1.name == nil ? $1.count : 1) })
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

    private func emit(_ text: String) {
        let depth = remainingNodes.count
        guard depth > 0 else {
            output.append(contentsOf: text.utf8)
            output.append(0x0a)
            return
        }
        let parent = depth - 1
        let remaining = remainingNodes[parent] - 1
        remainingNodes[parent] = remaining
        let above = prefixEnds[parent]
        output.append(contentsOf: prefix[..<above])
        output.append(contentsOf: (remaining == 0 ? "└── " : "├── ").utf8)
        output.append(contentsOf: text.utf8)
        output.append(0x0a)
        prefix.removeSubrange(above...)
        prefix.append(contentsOf: (remaining > 0 ? "│   " : "    ").utf8)
        if prefixEnds.count > depth {
            prefixEnds[depth] = prefix.count
        } else {
            prefixEnds.append(prefix.count)
        }
    }
}

/// Formats walker callbacks without choosing or visiting descendant nodes.
private struct DumpVisitor: MarkupVisitor {
    let state: DumpState

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Document",
            node,
            children: node.content.count,
            groups: [
                (nil, node.content.count + node.footnotes.count + node.specimens.count + (node.metadata == nil ? 0 : 1))
            ]
        )
    }

    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Callout",
            node,
            fields: [
                "variant=\(dump(optional: node.variant))", "collapsed=\(node.collapsed.map(dump(boolean:)) ?? "null")",
            ],
            children: node.content.count,
            groups: (node.title.map { [(name: "Title", count: $0.count)] } ?? []) + [(nil, node.content.count)]
        )
    }

    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Paragraph", node, children: node.content.count)
    }

    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
    }

    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("ThematicBreak", node)
    }

    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
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
    }

    mutating func visit(_ node: ListItem, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "ListItem",
            node,
            fields: ["marker=\(dump(optional: node.marker))"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
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

    mutating func visit(_ node: HTMLBlock, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("HTMLBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("FormulaBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Table, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        let columns = node.columns.map { "\($0.flow.rawValue):\($0.relative.map(dump(decimal:)) ?? "null")" }.joined(
            separator: ","
        )
        let count = node.head.count + node.content.count + node.foot.count
        state.line(
            "Table",
            node,
            fields: ["columns=[\(columns)]"],
            children: count,
            groups: (node.caption == nil ? [] : [(nil, 1)]) + [
                ("TableHead", node.head.count), ("TableBody", node.content.count), ("TableFoot", node.foot.count),
            ]
        )
    }

    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("DefinitionList", node, fields: [], children: node.definitions.count)
    }

    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Definition",
            node,
            fields: ["compact=\(node.compact)"],
            children: node.content.count,
            groups: [("DefinitionTerm", node.term.count)] + node.content.map { ("DefinitionBody", $0.count) }
        )
    }

    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "DirectiveBlock",
            node,
            fields: ["name=\(dump(optional: node.name))"],
            children: node.content.count,
            groups: [(nil, node.content.count + (node.label == nil ? 0 : 1))]
        )
    }

    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("DirectiveLabel", node, children: node.content.count)
    }
}

extension DumpVisitor {
    mutating func visit(_ node: Text, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Text", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("SoftBreak", node)
    }

    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("LineBreak", node)
    }

    mutating func visit(_ node: Code, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Code", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("HTML", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "CrossLink",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))"]
        )
    }

    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "CrossEmbedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ]
        )
    }

    mutating func visit(_ node: Comment, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Comment", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Formula",
            node,
            fields: ["mode=\(node.mode.rawValue)", "literal=\(dump(escaped: node.literal))"]
        )
    }

    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Emphasis", node, children: node.content.count)
    }

    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Strong", node, children: node.content.count)
    }

    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Strikethrough", node, children: node.content.count)
    }

    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Mark", node, children: node.content.count)
    }

    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Insertion", node, children: node.content.count)
    }

    mutating func visit(_ node: Span, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Span", node, children: node.content.count)
    }

    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Superscript", node, children: node.content.count)
    }

    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Subscript", node, children: node.content.count)
    }

    mutating func visit(_ node: Link, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Link",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "title=\(dump(optional: node.title))"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Embedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))", "title=\(dump(optional: node.title))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Directive",
            node,
            fields: ["name=\(dump(escaped: node.name))"],
            groups: [(nil, node.label == nil ? 0 : 1)]
        )
    }

    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("Cite", node, children: node.citations.count)
    }

    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line("TableCaption", node, children: node.content.count)
    }

    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "TableRow",
            node,
            children: node.cells.count
        )
    }

    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "TableCell",
            node,
            fields: ["rowspan=\(node.rowspan)", "colspan=\(node.colspan)"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
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

    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Footnote",
            node,
            fields: ["id=\(dump(escaped: node.id))"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Specimen",
            node,
            fields: ["id=\(dump(optional: node.id))", "start=\(node.start.map(String.init) ?? "null")"],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) {
        guard phase == .enter else {
            state.end()
            return
        }
        state.start()
        state.line(
            "Citation",
            node,
            fields: ["referent=\(dump(referent: node.referent))"],
            children: 0,
            groups: [("CitationPrefix", node.prefix.count), ("CitationSuffix", node.suffix.count)]
        )
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
private let hexDigits: [Character] = Array("0123456789abcdef")

private func dump(escaped value: String) -> String {
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
            result += "\\u00\(hexDigits[Int(scalar.value >> 4)])\(hexDigits[Int(scalar.value & 0xf)])"
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
