import MarkdownCoreC

public struct ParseOptions: Sendable, Hashable {
    public let smartPunctuation: Bool
    public let footnotes: Bool
    public let stripHTMLComments: Bool
    public let tables: Bool
    public let strikethrough: Bool
    public let autolinks: Bool
    public let taskLists: Bool
    public let formulas: Bool
    public let directives: Bool

    public init(
        smartPunctuation: Bool = true,
        footnotes: Bool = true,
        stripHTMLComments: Bool = true,
        tables: Bool = true,
        strikethrough: Bool = true,
        autolinks: Bool = true,
        taskLists: Bool = true,
        formulas: Bool = true,
        directives: Bool = true
    ) {
        self.smartPunctuation = smartPunctuation
        self.footnotes = footnotes
        self.stripHTMLComments = stripHTMLComments
        self.tables = tables
        self.strikethrough = strikethrough
        self.autolinks = autolinks
        self.taskLists = taskLists
        self.formulas = formulas
        self.directives = directives
    }
}

public enum ParseErrorCode: Int32, Sendable {
    case invalidArgument = 1
    case allocationFailed = 2
    case `internal` = 3
}

public struct ParseError: Error, Sendable, CustomStringConvertible {
    public let code: ParseErrorCode
    public let message: String
    public let scope: Scope?

    public var description: String { message }
}

/// A parse, under two total views.
///
/// The document IS the semantic view -- the tree with policy applied, which may
/// omit bytes: a fence, a bullet and a reference definition's punctuation are in
/// no literal anywhere. ``concrete`` omits nothing. Every byte of the source is
/// in exactly one region of the concrete view and every region has exactly one
/// owner in this tree, so the pair is complete.
///
/// In C the two are siblings, because a `markdown_core_document` is a handle and
/// the root is a node it lends out. Here they are not: the handle is gone by the
/// time `parse` returns, the tree is a value, and the concrete view hangs off
/// the root it names into.
public struct Document: Markup {
    public let scope: Scope
    public let content: [any Markup]
    public let concrete: Concrete

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    public static func parse(_ source: String, options: ParseOptions = .init()) throws -> Document {
        var nativeOptions = markdown_core_parse_options(
            smart_punctuation: options.smartPunctuation,
            footnotes: options.footnotes,
            strip_html_comments: options.stripHTMLComments,
            tables: options.tables,
            strikethrough: options.strikethrough,
            autolinks: options.autolinks,
            task_lists: options.taskLists,
            formulas: options.formulas,
            directives: options.directives
        )
        var nativeError: OpaquePointer?
        let bytes = Array(source.utf8)
        let nativeDocument = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_document_parse(buffer.baseAddress, buffer.count, &nativeOptions, &nativeError)
        }
        guard let nativeDocument else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }

        guard let root = markdown_core_document_semantic(nativeDocument),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT,
            let concrete = Concrete(from: nativeDocument)
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree", scope: nil)
        }
        return Document(from: root, concrete: concrete)
    }
}

extension Document {
    /// The node a region's ``Region/owner`` path names, or `nil` when the path
    /// names no node in this tree.
    ///
    /// The path counts children the way the C tree holds them, and the value
    /// tree splits some of those runs into named fields -- a directive's label
    /// and its content, a table's header and its rows -- so descending it is
    /// not `content[i]` at every step. This is the descent.
    public func owner(of region: Region) -> (any Markup)? {
        var node: any Markup = self
        for step in region.owner {
            var visitor = ChildrenVisitor()
            let children = node.accept(&visitor)
            guard step >= 0, Int(step) < children.count else { return nil }
            node = children[Int(step)]
        }
        return node
    }
}

extension Document {
    init(from node: OpaquePointer, concrete: Concrete) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node), concrete: concrete)
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity
func markup(from node: OpaquePointer) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        // A document is only ever the ROOT, and the root is built by `parse`,
        // which is the only place the concrete view exists to build it with.
        preconditionFailure("a document node cannot be a child")
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node)
    case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node)
    case MARKDOWN_CORE_KIND_HEADING: Heading(from: node)
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node)
    case MARKDOWN_CORE_KIND_LIST: List(from: node)
    case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node)
    case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node)
    case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node)
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node)
    case MARKDOWN_CORE_KIND_TABLE: Table(from: node)
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK: DirectiveBlock(from: node)
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node)
    case MARKDOWN_CORE_KIND_TEXT: Text(from: node)
    case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node)
    case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node)
    case MARKDOWN_CORE_KIND_CODE: Code(from: node)
    case MARKDOWN_CORE_KIND_HTML: HTML(from: node)
    case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node)
    case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node)
    case MARKDOWN_CORE_KIND_STRONG: Strong(from: node)
    case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node)
    case MARKDOWN_CORE_KIND_LINK: Link(from: node)
    case MARKDOWN_CORE_KIND_IMAGE: Image(from: node)
    case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node)
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node)
    case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node)
    case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node)
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL: DirectiveLabel(from: node)
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: ReferenceDefinition(from: node)
    case MARKDOWN_CORE_KIND_LINK_REFERENCE: LinkReference(from: node)
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: ImageReference(from: node)
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}
