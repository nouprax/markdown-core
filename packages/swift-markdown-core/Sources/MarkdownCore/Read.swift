import MarkdownCoreC

/// One read of the text.
///
/// ``semantic`` is the tree with policy applied. Every scope in it is counted
/// against the NORMALIZED source — UTF-8 as fed, every NUL replaced by
/// U+FFFD, every line ending a single `\n` and every line having one — which
/// the library does not hand back: a caller whose input can differ from it
/// applies the same normalization to its own copy before resolving a scope.
///
/// A read is an immutable value the caller owns outright. It retains nothing
/// native, so it is `Sendable` and stays readable after every later feed and
/// after the ``Document`` that produced it is gone. A mid-stream read is the
/// projection of the parse as it stands; the read ``Document/seal()`` returns
/// is the whole text's.
public struct Read: Sendable {
    /// The semantic tree.
    public let semantic: Semantic

    /// The canonical diagnostic dump of ``semantic``.
    public func dump() -> String { semantic.dump() }
}

extension Read {
    /// Copies a native document out as values — the one conversion behind
    /// every read a ``Document`` returns. The handle stays with the caller,
    /// who frees it as soon as this returns.
    init(copiedFrom nativeDocument: OpaquePointer) throws {
        guard let root = markdown_core_document_semantic(nativeDocument),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree")
        }
        self.init(semantic: Semantic(from: root))
    }
}
