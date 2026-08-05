import MarkdownCoreC

/// A block of raw HTML, passed through unparsed.
public struct HTMLBlock: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this node's content last changed.
    public let revision: UInt64
    /// The raw HTML text.
    public let literal: String

    /// True when the literal is one complete comment, so consumers without
    /// an HTML parser can skip comment material by this bit alone.
    public var comment: Bool { htmlLiteralIsComment(literal) }

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension HTMLBlock {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        var literal = markdown_core_string_view()
        markdown_core_node_literal(node, &literal)
        self.init(id: id, revision: revision, literal: literal.requiredString)
    }
}

/// The canonical one-complete-comment rule shared by `HTMLBlock` and `HTML`:
/// after surrounding whitespace the literal opens with `<!--` and its first
/// `-->` is the terminal bytes. Comment-prefixed HTML with a same-line tail
/// is not a comment. Derived purely from the literal, matching
/// `markdown_core_node_html_comment`.
func htmlLiteralIsComment(_ literal: String) -> Bool {
    var bytes = ArraySlice(Array(literal.utf8))
    while let last = bytes.last, last == 0x0A || last == 0x20 || last == 0x09 { bytes = bytes.dropLast() }
    while let first = bytes.first, first == 0x20 || first == 0x09 { bytes = bytes.dropFirst() }
    let opener: [UInt8] = [0x3C, 0x21, 0x2D, 0x2D]
    let closer: [UInt8] = [0x2D, 0x2D, 0x3E]
    guard bytes.count >= 4, bytes.prefix(4).elementsEqual(opener) else { return false }
    var index = bytes.startIndex + 1
    while index + 3 <= bytes.endIndex {
        if bytes[index..<(index + 3)].elementsEqual(closer) {
            return index + 3 == bytes.endIndex
        }
        index += 1
    }
    return false
}
