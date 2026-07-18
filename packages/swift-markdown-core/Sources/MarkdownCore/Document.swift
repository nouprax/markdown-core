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

/// An immutable snapshot of a parsed Markdown document.
///
/// A `Document` is itself the root `Markup` node. Snapshots produced by a
/// `MarkupSession` structurally share every unchanged node with the previous
/// snapshot; a one-shot `Document.parse` is a self-contained value.
///
/// Absolute source positions are not stored on nodes: resolve them with
/// `scope(of:)`, receive them from `Walker` events, or print them with
/// `dump()`. A session snapshot resolves scopes against its session the
/// first time any of these is used and is self-contained from then on; see
/// `scope(of:)` for the exact rules.
public struct Document: Markup {
    public let id: MarkupID
    public let revision: UInt64
    public let children: [any Markup]
    var resolver: ScopeResolver

    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    public static func parse(_ source: String, options: ParseOptions = .init()) throws -> Document {
        // A one-shot parse is literally a single-commit session. Scopes
        // materialize eagerly because the session dies with this call and
        // the snapshot must leave it self-contained.
        let session = try MarkupSession(options: options)
        try session.append(source)
        let document = try session.commitBulk()
        document.resolver.materialize()
        return document
    }
}

extension ParseOptions {
    var nativeValue: markdown_core_parse_options {
        markdown_core_parse_options(
            smart_punctuation: smartPunctuation,
            footnotes: footnotes,
            strip_html_comments: stripHTMLComments,
            tables: tables,
            strikethrough: strikethrough,
            autolinks: autolinks,
            task_lists: taskLists,
            formulas: formulas,
            dollar_formula_delimiters: dollarFormulaDelimiters,
            latex_formula_delimiters: latexFormulaDelimiters,
            directives: directives
        )
    }
}

/// Builds platform values from native nodes for the session mirror. The
/// `children` strategy is the only degree of freedom: a first commit's bulk
/// build assembles child arrays in sibling frames, while an incremental
/// commit rebuilds parents from already-built mirror values.
struct MarkupBuilder {
    let lineage: UInt64
    let children: (OpaquePointer) -> [any Markup]

    func identity(of node: OpaquePointer) -> (id: MarkupID, revision: UInt64) {
        (
            MarkupID(lineage: lineage, rawValue: markdown_core_node_get_id(node)),
            markdown_core_node_get_revision(node)
        )
    }

    // Keep the exhaustive native-kind switch in one place so a newly added
    // native kind cannot silently bypass value-tree copying.
    // swiftlint:disable:next cyclomatic_complexity
    func markup(from node: OpaquePointer) -> any Markup {
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_DOCUMENT: Document(from: node, in: self)
        case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node, in: self)
        case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, in: self)
        case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, in: self)
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node, in: self)
        case MARKDOWN_CORE_KIND_LIST: List(from: node, in: self)
        case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, in: self)
        case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node, in: self)
        case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node, in: self)
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node, in: self)
        case MARKDOWN_CORE_KIND_TABLE: Table(from: node, in: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK: DirectiveBlock(from: node, in: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node, in: self)
        case MARKDOWN_CORE_KIND_TEXT: Text(from: node, in: self)
        case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node, in: self)
        case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node, in: self)
        case MARKDOWN_CORE_KIND_CODE: Code(from: node, in: self)
        case MARKDOWN_CORE_KIND_HTML: HTML(from: node, in: self)
        case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node, in: self)
        case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, in: self)
        case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, in: self)
        case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, in: self)
        case MARKDOWN_CORE_KIND_LINK: Link(from: node, in: self)
        case MARKDOWN_CORE_KIND_IMAGE: Image(from: node, in: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, in: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node, in: self)
        case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, in: self)
        case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, in: self)
        default: preconditionFailure("native parser returned an unknown node kind")
        }
    }
}

extension Document {
    init(from node: OpaquePointer, in builder: MarkupBuilder) {
        let (id, revision) = builder.identity(of: node)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            resolver: ScopeResolver.unresolvable
        )
    }
}
