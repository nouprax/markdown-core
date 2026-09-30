/// Draws the canonical debug tree over the canonical walk, one line per node
/// or named relation, as the C dump does. The walk's frames are the tree's
/// depth; the call stack stays constant.
enum MarkupDumper {
    /// The canonical dump of `root`, whose extent is relative to the absolute
    /// offset `anchor`, with scopes in UTF-8 columns of `bytes`.
    static func render(
        _ root: MarkupRecord,
        anchor: Int,
        bytes: UnsafeBufferPointer<UInt8>,
        lines: SourceLines
    ) -> String {
        var output = ""
        // `more[n]` says whether the latest line at level `n + 1` has a later
        // sibling; `segments[n]` is the prefix segment lines below it draw.
        var more: [Bool] = []
        var segments: [String] = []
        var walk = CanonicalWalk(root: root, anchor: anchor)
        while let item = walk.next() {
            if item.level > 0 {
                let depth = item.level - 1
                if more.count <= depth { more.append(contentsOf: repeatElement(false, count: depth + 1 - more.count)) }
                more[depth] = item.hasNext
                if depth > 0 {
                    // The lines nested below the item's parent lead with the
                    // segments above it plus the one its own connector decides.
                    segments.removeLast(segments.count - (depth - 1))
                    segments.append(more[depth - 1] ? "│   " : "    ")
                }
                output += segments[..<depth].joined()
                output += item.hasNext ? "├── " : "└── "
            }
            if let record = item.record {
                let scope = lines.scope(from: item.start, to: item.end, in: bytes, unit: .utf8)
                var visitor = LineVisitor(place: dump(scope: scope))
                dispatch(record.markup, to: &visitor, phase: .enter)
                output += visitor.text
            } else if let name = item.name {
                output += "\(name) children=\(item.count)"
            }
            output += "\n"
        }
        return output
    }
}

/// Formats one node's line: its kind, its place, its fields and its count of
/// structural children. It never visits descendants; the walk draws them.
private struct LineVisitor: MarkupVisitor {
    let place: String
    var text = ""

    private mutating func line(_ kind: String, _ node: some Markup, fields: [String] = [], children: Int = 0) {
        let common = [
            kind, place, "anchor=\(dump(optional: node.anchor))", "attributes=\(dump(attributes: node.attributes))",
        ]
        text = (common + fields + ["children=\(children)"]).joined(separator: " ")
    }

    mutating func visit(_ node: Document, phase: MarkupVisitPhase) {
        line("Document", node, children: node.content.count)
    }

    mutating func visit(_ node: Callout, phase: MarkupVisitPhase) {
        line(
            "Callout",
            node,
            fields: [
                "variant=\(dump(optional: node.variant))",
                "collapsed=\(node.collapsed.map(dump(boolean:)) ?? "null")",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Paragraph, phase: MarkupVisitPhase) {
        line("Paragraph", node, children: node.content.count)
    }

    mutating func visit(_ node: Heading, phase: MarkupVisitPhase) {
        line("Heading", node, fields: ["level=\(node.level)"], children: node.content.count)
    }

    mutating func visit(_ node: ThematicBreak, phase: MarkupVisitPhase) { line("ThematicBreak", node) }

    mutating func visit(_ node: MarkdownCore.List, phase: MarkupVisitPhase) {
        line(
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
        line("ListItem", node, fields: ["marker=\(dump(optional: node.marker))"], children: node.content.count)
    }

    mutating func visit(_ node: CodeBlock, phase: MarkupVisitPhase) {
        line(
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
        line("HTMLBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: FormulaBlock, phase: MarkupVisitPhase) {
        line("FormulaBlock", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Table, phase: MarkupVisitPhase) {
        line(
            "Table",
            node,
            fields: ["columns=[\(dump(columns: node.columns))]"],
            children: node.head.count + node.content.count + node.foot.count
        )
    }

    mutating func visit(_ node: DirectiveBlock, phase: MarkupVisitPhase) {
        line("DirectiveBlock", node, fields: ["name=\(dump(optional: node.name))"], children: node.content.count)
    }

    mutating func visit(_ node: DirectiveLabel, phase: MarkupVisitPhase) {
        line("DirectiveLabel", node, children: node.content.count)
    }

    mutating func visit(_ node: Text, phase: MarkupVisitPhase) {
        line("Text", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: SoftBreak, phase: MarkupVisitPhase) { line("SoftBreak", node) }

    mutating func visit(_ node: LineBreak, phase: MarkupVisitPhase) { line("LineBreak", node) }

    mutating func visit(_ node: Code, phase: MarkupVisitPhase) {
        line("Code", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: HTML, phase: MarkupVisitPhase) {
        line("HTML", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Comment, phase: MarkupVisitPhase) {
        line("Comment", node, fields: ["literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: CrossLink, phase: MarkupVisitPhase) {
        line(
            "CrossLink",
            node,
            fields: ["dest=\(dump(destination: node.dest))", "label=\(dump(optional: node.label))"]
        )
    }

    mutating func visit(_ node: CrossEmbedded, phase: MarkupVisitPhase) {
        line(
            "CrossEmbedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))",
                "label=\(dump(optional: node.label))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ]
        )
    }

    mutating func visit(_ node: Formula, phase: MarkupVisitPhase) {
        line("Formula", node, fields: ["mode=\(node.mode.rawValue)", "literal=\(dump(escaped: node.literal))"])
    }

    mutating func visit(_ node: Emphasis, phase: MarkupVisitPhase) {
        line("Emphasis", node, children: node.content.count)
    }

    mutating func visit(_ node: Strong, phase: MarkupVisitPhase) { line("Strong", node, children: node.content.count) }

    mutating func visit(_ node: Strikethrough, phase: MarkupVisitPhase) {
        line("Strikethrough", node, children: node.content.count)
    }

    mutating func visit(_ node: Mark, phase: MarkupVisitPhase) { line("Mark", node, children: node.content.count) }

    mutating func visit(_ node: Insertion, phase: MarkupVisitPhase) {
        line("Insertion", node, children: node.content.count)
    }

    mutating func visit(_ node: Span, phase: MarkupVisitPhase) { line("Span", node, children: node.content.count) }

    mutating func visit(_ node: Superscript, phase: MarkupVisitPhase) {
        line("Superscript", node, children: node.content.count)
    }

    mutating func visit(_ node: Subscript, phase: MarkupVisitPhase) {
        line("Subscript", node, children: node.content.count)
    }

    mutating func visit(_ node: DefinitionList, phase: MarkupVisitPhase) {
        line("DefinitionList", node, children: node.definitions.count)
    }

    mutating func visit(_ node: Definition, phase: MarkupVisitPhase) {
        line("Definition", node, fields: ["compact=\(dump(boolean: node.compact))"], children: node.content.count)
    }

    mutating func visit(_ node: Link, phase: MarkupVisitPhase) {
        line(
            "Link",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))",
                "title=\(dump(optional: node.title))",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Embedded, phase: MarkupVisitPhase) {
        line(
            "Embedded",
            node,
            fields: [
                "dest=\(dump(destination: node.dest))",
                "title=\(dump(optional: node.title))",
                "dimensions=\(dump(dimensions: node.dimensions))",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Directive, phase: MarkupVisitPhase) {
        line("Directive", node, fields: ["name=\(dump(escaped: node.name))"])
    }

    mutating func visit(_ node: Cite, phase: MarkupVisitPhase) { line("Cite", node, children: node.citations.count) }

    mutating func visit(_ node: TableCaption, phase: MarkupVisitPhase) {
        line("TableCaption", node, children: node.content.count)
    }

    mutating func visit(_ node: TableRow, phase: MarkupVisitPhase) {
        line("TableRow", node, children: node.cells.count)
    }

    mutating func visit(_ node: TableCell, phase: MarkupVisitPhase) {
        line(
            "TableCell",
            node,
            fields: [
                "rowspan=\(node.rowspan)",
                "colspan=\(node.colspan)",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Citation, phase: MarkupVisitPhase) {
        line("Citation", node, fields: ["referent=\(dump(referent: node.referent))"])
    }

    mutating func visit(_ node: Footnote, phase: MarkupVisitPhase) {
        line("Footnote", node, fields: ["label=\(dump(optional: node.label))"], children: node.content.count)
    }

    mutating func visit(_ node: Specimen, phase: MarkupVisitPhase) {
        line(
            "Specimen",
            node,
            fields: [
                "label=\(dump(optional: node.label))",
                "start=\(node.start.map(String.init) ?? "null")",
            ],
            children: node.content.count
        )
    }

    mutating func visit(_ node: Metadata, phase: MarkupVisitPhase) {
        line(
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
            ]
        )
    }
}

private func dump(columns value: [TableColumn]) -> String {
    value.map { "\($0.flow.rawValue):\($0.relative.map(dump(decimal:)) ?? "null")" }.joined(separator: ",")
}

// Canonical spellings for values in the debug dump.
private func dump(scope value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

private func dump(boolean value: Bool) -> String { value ? "true" : "false" }

/// A tagged value prints its branch and its named fields with no spaces.
/// An inline note is drawn under its Citation, so its branch prints no field.
private func dump(referent value: CitationReferent) -> String {
    switch value {
    case .bib(let key, let mode): "bib(key=\(dump(escaped: key)),mode=\(mode.rawValue))"
    case .footnote(.label(let label)): "footnote(label=\(dump(escaped: label)))"
    case .footnote(.note): "footnote(note)"
    case .specimen(let label): "specimen(label=\(dump(escaped: label)))"
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
