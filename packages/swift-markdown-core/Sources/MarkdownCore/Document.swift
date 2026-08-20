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
    public let dollarFormulaDelimiters: Bool
    public let latexFormulaDelimiters: Bool
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
        dollarFormulaDelimiters: Bool = true,
        latexFormulaDelimiters: Bool = true,
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
        self.dollarFormulaDelimiters = dollarFormulaDelimiters
        self.latexFormulaDelimiters = latexFormulaDelimiters
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

public struct Document: Markup {
    public let scope: Scope
    public let children: [any Markup]

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
            dollar_formula_delimiters: options.dollarFormulaDelimiters,
            latex_formula_delimiters: options.latexFormulaDelimiters,
            directives: options.directives
        )
        var nativeError: OpaquePointer?
        let bytes = Array(source.utf8)
        let nativeDocument = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_document_parse(
                buffer.baseAddress,
                buffer.count,
                &nativeOptions,
                &nativeError
            )
        }
        guard let nativeDocument else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
        defer { markdown_core_document_free(nativeDocument) }

        guard let root = markdown_core_document_root(nativeDocument),
            let document = markup(from: root) as? Document
        else {
            throw ParseError(
                code: .internal,
                message: "parser returned an invalid document tree",
                scope: nil
            )
        }
        return document
    }
}

extension Document {
    init(from node: OpaquePointer) {
        self.init(scope: Self.scope(from: node), children: Self.children(from: node))
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity
func markup(from node: OpaquePointer) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT: Document(from: node)
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
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}
