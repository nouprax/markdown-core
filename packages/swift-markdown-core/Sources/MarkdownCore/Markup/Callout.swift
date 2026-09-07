import MarkdownCoreC

/// How a callout's fold marker was written.
public enum CalloutFold: String, Sendable {
    /// No `+` or `-` after the type.
    case none
    /// `+`: the callout opens expanded.
    case expanded
    /// `-`: the callout opens collapsed.
    case collapsed
}

/// A callout — every `>` container.
///
/// A plain quoted block is a callout without metadata: `variant` is `nil`,
/// `fold` is ``CalloutFold/none`` and `title` is `nil`. The callouts module's
/// metadata rule, which fills them in, lands with `O8`.
public struct Callout: Markup {
    /// Where it is. See ``Scope`` — boundaries, not a byte range.
    public let scope: Scope
    /// The authored type as written, or `nil` when the container has no
    /// metadata line.
    public let variant: String?
    /// The fold marker.
    public let fold: CalloutFold
    /// The title's inline content, or `nil` when no title was authored. The
    /// callout owns it as a field; it is never part of `content`.
    public let title: [any Markup]?
    /// The quoted blocks. Block content, not inline.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Callout` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Callout {
    init(from node: OpaquePointer, title: [any Markup]?, content: [any Markup]) {
        var variant = markdown_core_optional_string()
        var fold = MARKDOWN_CORE_CALLOUT_FOLD_NONE
        markdown_core_node_callout_properties(node, &variant, &fold)
        self.init(
            scope: Self.scope(from: node),
            variant: variant.string,
            fold: CalloutFold(from: fold),
            title: title,
            content: content
        )
    }
}

extension CalloutFold {
    init(from fold: markdown_core_callout_fold) {
        switch fold {
        case MARKDOWN_CORE_CALLOUT_FOLD_EXPANDED: self = .expanded
        case MARKDOWN_CORE_CALLOUT_FOLD_COLLAPSED: self = .collapsed
        default: self = .none
        }
    }
}
