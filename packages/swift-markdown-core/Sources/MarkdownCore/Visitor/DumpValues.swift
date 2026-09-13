// Canonical spellings for values; tree topology stays in TreeDumper.
func dump(scope value: Scope) -> String {
    "scope=\(value.start.line):\(value.start.column)..\(value.end.line):\(value.end.column)"
}

func dump(boolean value: Bool) -> String { value ? "true" : "false" }

/// A tagged value prints its branch and its named fields with no spaces.
func dump(referent value: CitationReferent) -> String {
    switch value {
    case .bib(let key, let mode): "bib(key=\(dump(escaped: key)),mode=\(mode.rawValue))"
    case .footnote(let id): "footnote(id=\(dump(escaped: id)))"
    case .specimen(let id): "specimen(id=\(dump(escaped: id)))"
    }
}

/// A tagged value prints its branch and its named fields with no spaces.
func dump(destination value: Destination) -> String {
    switch value {
    case .url(let url): "url(\(dump(escaped: url)))"
    case .cross(let path, let anchor): "cross(path=\(dump(escaped: path)),anchor=\(dump(optional: anchor)))"
    }
}

func dump(delimiter value: OrderedListDelimiter?) -> String {
    switch value {
    case .period: "period"
    case .parenthesis(let closed): "parenthesis(closed=\(dump(boolean: closed)))"
    case .default: "default"
    case nil: "null"
    }
}

func dump(variant value: OrderedListVariant?) -> String {
    switch value {
    case .decimal: "decimal"
    case .alpha(let lowercased): "alpha(lowercased=\(dump(boolean: lowercased)))"
    case .roman(let lowercased): "roman(lowercased=\(dump(boolean: lowercased)))"
    case .default: "default"
    case nil: "null"
    }
}

func dump(optional value: String?) -> String {
    value.map(dump(escaped:)) ?? "null"
}

/// Quotes a string and escapes its contents using JSON string-literal rules.
func dump(escaped value: String) -> String {
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
func dump(decimal value: Double) -> String {
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

func dump(attributes value: Attributes) -> String {
    "{"
        + (value.classes.map { "." + dump(attributeClass: $0) }
        + value.records.map { "\($0.name)=\(dump(escaped: $0.value))" })
        .joined(separator: " ") + "}"
}
func dump(metadata value: MetadataValue) -> String {
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

func dump(attributeClass value: String) -> String {
    let plain =
        !value.isEmpty
        && value.utf8.allSatisfy {
            $0 > 32 && $0 < 127 && ![34, 92, 123, 125, 91, 93, 40, 41, 61].contains($0)
        }
    return plain ? value : dump(escaped: value)
}

func dump(dimensions value: Dimensions?) -> String {
    guard let value else { return "null" }
    return "(width=\(value.width),height=\(value.height.map(String.init) ?? "null"))"
}
