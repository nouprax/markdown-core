import Foundation
import MarkdownCoreC

/// The feature switches for one parse or session, fixed for its lifetime.
/// Every option defaults to `true`.
public struct ParseOptions: Sendable, Hashable {
    /// Replaces straight quotes, dashes, and ellipses with typographic forms.
    public let smartPunctuation: Bool
    /// Parses footnote definitions and references.
    public let footnotes: Bool
    /// Removes HTML comments instead of passing them through.
    public let stripHTMLComments: Bool
    /// Parses pipe tables.
    public let tables: Bool
    /// Parses `~~strikethrough~~` spans.
    public let strikethrough: Bool
    /// Recognizes bare URLs and email addresses as links.
    public let autolinks: Bool
    /// Parses `[ ]`/`[x]` task-list item markers.
    public let taskLists: Bool
    /// Parses formula spans and blocks.
    public let formulas: Bool
    /// Recognizes `$…$` and `$$…$$` formula delimiters.
    public let dollarFormulaDelimiters: Bool
    /// Recognizes `\(…\)` and `\[…\]` formula delimiters.
    public let latexFormulaDelimiters: Bool
    /// Parses inline and container directives.
    public let directives: Bool

    /// Creates a fixed option set; every switch defaults to `true`.
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

/// The category of a native parse or session failure.
public enum ParseErrorCode: Int32, Sendable {
    case invalidArgument = 1
    case allocationFailed = 2
    case `internal` = 3
}

/// A native parse or session failure, carrying the engine's message and,
/// when the input position is known, the failing scope.
public struct ParseError: Error, Sendable, CustomStringConvertible {
    /// The failure category.
    public let code: ParseErrorCode
    /// The engine's actionable description of the failure.
    public let message: String
    /// The failing input extent, when the engine could attribute one.
    public let scope: Scope?

    /// The engine's message, so string interpolation prints it directly.
    public var description: String { message }
}

extension ParseError: LocalizedError {
    /// The native parser's actionable message — the same text as
    /// `description` — so Foundation error presentation (alerts, logs,
    /// `NSError` bridging) never degrades to a bare domain and code.
    public var errorDescription: String? { message }
}

/// An immutable snapshot of a parsed Markdown document.
///
/// A `Document` is itself the root `Markup` node. Snapshots produced by a
/// `MarkupSession` structurally share every unchanged node with the previous
/// snapshot; a one-shot `Document.parse` is a self-contained value.
///
/// Absolute source positions are not stored on nodes: resolve them with
/// `scope(of:)`, receive them from `MarkupWalker` events, or print them with
/// `dump()`. A session snapshot resolves scopes against its session the
/// first time any of these is used and is self-contained from then on; see
/// `scope(of:)` for the exact rules.
public struct Document: Markup {
    /// The node's session-scoped identity; see `MarkupID`.
    public let id: MarkupID
    /// The commit revision at which this document's content last changed.
    public let revision: UInt64
    /// The document's top-level blocks in source order.
    public let children: [any Markup]
    var resolver: ScopeResolver

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    /// Parses `source` in one shot and returns a self-contained immutable
    /// snapshot; throws `ParseError` when the engine rejects the input.
    /// Semantically identical to committing the same text through a
    /// `MarkupSession`.
    public static func parse(_ source: String, options: ParseOptions = .init()) throws -> Document {
        // A one-shot parse is literally a single-commit session. Scopes
        // materialize eagerly because the session dies with this call and
        // the snapshot must leave it self-contained.
        let session = try MarkupSession(options: options)
        try session.append(source)
        let document = try session.commit().document
        document.resolver.materialize()
        return document
    }
}

extension ParseOptions {
    var native: markdown_core_parse_options {
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

    func id(of node: OpaquePointer) -> (id: MarkupID, revision: UInt64) {
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
        case MARKDOWN_CORE_KIND_DOCUMENT: Document(from: node, builder: self)
        case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node, builder: self)
        case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, builder: self)
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LIST: List(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, builder: self)
        case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE: Table(from: node, builder: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK: DirectiveBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TEXT: Text(from: node, builder: self)
        case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_CODE: Code(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HTML: HTML(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node, builder: self)
        case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, builder: self)
        case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, builder: self)
        case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LINK: Link(from: node, builder: self)
        case MARKDOWN_CORE_KIND_IMAGE: Image(from: node, builder: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, builder: self)
        default: preconditionFailure("native parser returned an unknown node kind")
        }
    }
}

extension Document {
    init(from node: OpaquePointer, builder: MarkupBuilder) {
        let (id, revision) = builder.id(of: node)
        self.init(
            id: id,
            revision: revision,
            children: builder.children(node),
            resolver: ScopeResolver.unresolvable
        )
    }
}
