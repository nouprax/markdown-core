// Canonical spellings for values; tree topology stays in TreeDumper.
func scopeString(_ value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

func boolean(_ value: Bool) -> String { value ? "true" : "false" }

/// A tagged value prints its branch and its named fields with no spaces.
func referentString(_ value: CitationReferent) -> String {
    switch value {
    case .bib(let key, let mode): "bib(key=\(jsonString(key)),mode=\(mode.rawValue))"
    case .footnote(let id): "footnote(id=\(jsonString(id)))"
    case .specimen(let id): "specimen(id=\(jsonString(id)))"
    }
}

/// A tagged value prints its branch and its named fields with no spaces.
func destinationString(_ value: Destination) -> String {
    switch value {
    case .url(let url): "url(\(jsonString(url)))"
    case .cross(let path, let anchor): "cross(path=\(jsonString(path)),anchor=\(optionalString(anchor)))"
    }
}

func orderedListDelimiter(_ value: OrderedListDelimiter?) -> String {
    switch value {
    case .period: "period"
    case .parenthesis(let closed): "parenthesis(closed=\(boolean(closed)))"
    case .default: "default"
    case nil: "null"
    }
}

func orderedListVariant(_ value: OrderedListVariant?) -> String {
    switch value {
    case .decimal: "decimal"
    case .alpha(let lowercased): "alpha(lowercased=\(boolean(lowercased)))"
    case .roman(let lowercased): "roman(lowercased=\(boolean(lowercased)))"
    case .default: "default"
    case nil: "null"
    }
}

func optionalString(_ value: String?) -> String {
    value.map(jsonString) ?? "null"
}

func jsonString(_ value: String) -> String {
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
func decimal(_ value: Double) -> String {
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

func attributesString(_ value: Attributes) -> String {
    "{"
        + (value.classes.map { "." + attributeClass($0) } + value.records.map { "\($0.name)=\(jsonString($0.value))" })
        .joined(separator: " ") + "}"
}
func metadataValue(_ value: MetadataValue) -> String {
    switch value {
    case .scalar(let scalar):
        let text: String
        switch scalar {
        case .null: text = "null"
        case .bool(let value): text = "bool(\(boolean(value)))"
        case .number(let value): text = "number(\(jsonString(value)))"
        case .text(let value): text = "text(\(jsonString(value)))"
        }
        return "scalar(\(text))"
    case .list(let items):
        return "list(["
            + items.map { item in
                switch item {
                case .number(let value): return "number(\(jsonString(value)))"
                case .text(let value): return "text(\(jsonString(value)))"
                }
            }.joined(separator: ",") + "])"
    }
}

func attributeClass(_ value: String) -> String {
    let plain =
        !value.isEmpty
        && value.utf8.allSatisfy {
            $0 > 32 && $0 < 127 && ![34, 92, 123, 125, 91, 93, 40, 41, 61].contains($0)
        }
    return plain ? value : jsonString(value)
}

func dimensionsString(_ value: Dimensions?) -> String {
    guard let value else { return "null" }
    return "(width=\(value.width),height=\(value.height.map(String.init) ?? "null"))"
}
