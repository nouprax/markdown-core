import MarkdownCoreC

/// The root of the semantic tree — the view with policy applied, which may
/// omit bytes: a fence, a bullet and a reference definition's punctuation are
/// in no literal anywhere. It is an ordinary ``Markup`` node: nothing but its
/// ``content`` and its ``scope``, like every node under it. What it does NOT
/// carry is the text its scopes are counted against — a root detached from its
/// ``Concrete`` is not self-interpreting, which is why the two travel together
/// as a ``Read`` and never alone.
public struct Semantic: Markup {
    /// The root's identity: its own mint with ordinal 0, like every block's.
    public let id: Identity
    /// The whole tree's boundaries. See ``Scope``.
    public let scope: Scope
    /// The tree's blocks. Block content, not inline.
    public let content: [any Markup]

    /// Dispatches to the visitor's `Semantic` case.
    public func accept<V: Visitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension Semantic {
    init(from node: OpaquePointer) {
        // The root is a block, so its own mint opens the outermost namespace
        // and the owner handed in resolves nothing.
        let id = Self.identity(from: node, owner: 0)
        self.init(id: id, scope: Self.scope(from: node), content: Self.children(from: node, owner: id.block))
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity
func markup(from node: OpaquePointer, owner: UInt32) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        // The root kind is only ever the ROOT, and the root is built by the
        // copy-in that also carries the concrete view beside it.
        preconditionFailure("a document node cannot be a child")
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_LIST: List(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_TABLE: Table(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK: DirectiveBlock(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_TEXT: Text(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_CODE: Code(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_HTML: HTML(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_LINK: Link(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_IMAGE: Image(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL: DirectiveLabel(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: ReferenceDefinition(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_LINK_REFERENCE: LinkReference(from: node, owner: owner)
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: ImageReference(from: node, owner: owner)
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}
