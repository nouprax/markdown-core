import MarkdownCoreC

/// One read of the text, under two total views.
///
/// ``semantic`` is the tree with policy applied, which may omit bytes;
/// ``concrete`` omits nothing. Every byte of the source is in exactly one
/// region of the concrete view and every region has exactly one owner in the
/// tree, so the pair is complete — and it is CLOSED: every scope in
/// ``semantic`` is counted against ``concrete``, and nothing outside this
/// value is needed to resolve one.
///
/// A read is an immutable value the caller owns outright. It retains nothing
/// native, so it is `Sendable` and stays readable after every later feed and
/// after the ``Document`` that produced it is gone. A mid-stream read is the
/// projection of the parse as it stands; the read ``Document/seal()`` returns
/// is the whole text's.
///
/// In C the two views are siblings on one `markdown_core_document` handle
/// (`markdown_core_document_semantic`, `markdown_core_document_source`); this
/// value is that handle's shape, copied out.
public struct Read: Sendable {
    /// The semantic tree.
    public let semantic: Semantic
    /// The text every scope in ``semantic`` is counted against.
    public let concrete: Concrete

    /// The canonical diagnostic dump of ``semantic``.
    public func dump() -> String { semantic.dump() }
}

extension Read {
    /// Copies a native document out as values — the one conversion behind
    /// every read a ``Document`` returns. The handle stays with the caller,
    /// who frees it as soon as this returns.
    init(copiedFrom nativeDocument: OpaquePointer) throws {
        guard let root = markdown_core_document_semantic(nativeDocument),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT,
            let concrete = Concrete(from: nativeDocument)
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree")
        }
        self.init(semantic: Semantic(from: root), concrete: concrete)
    }
}
