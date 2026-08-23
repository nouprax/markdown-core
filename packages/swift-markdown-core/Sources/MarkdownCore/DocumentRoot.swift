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

public struct DocumentRoot: Markup {
    public let scope: Scope
    public let content: [any Markup]

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }
}

extension DocumentRoot {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), content: Self.children(from: node))
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity
func markup(from node: OpaquePointer) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT: DocumentRoot(from: node)
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
