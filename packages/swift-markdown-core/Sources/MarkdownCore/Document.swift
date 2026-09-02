import MarkdownCoreC

/// Which constructs a parse recognises.
///
/// Every switch is ATTACHMENT and nothing finer: an extension is on or it is
/// not, and there is no second knob that changes what an attached extension
/// means. 1.0.3 had `dollarFormulaDelimiters` and `latexFormulaDelimiters`
/// beside ``formulas``; they are gone, because an option that changes a
/// grammar rather than enabling one is a second parser hiding in the first.
///
/// Every default is `true`.
public struct ParseOptions: Sendable, Hashable {
    /// Turn straight quotes, `--` and `...` into their typographic forms.
    public let smartPunctuation: Bool
    /// Recognise `[^label]` calls and `[^label]:` definitions.
    public let footnotes: Bool
    /// Drop HTML comments from the tree instead of keeping them as ``HTML``.
    public let stripHTMLComments: Bool
    /// Recognise GFM tables.
    public let tables: Bool
    /// Recognise `~~struck~~`.
    public let strikethrough: Bool
    /// Recognise bare URLs and `www.` prefixes as links.
    public let autolinks: Bool
    /// Recognise `- [ ]` and `- [x]` list items, which gives
    /// ``ListItem/checked`` a value other than `nil`.
    public let taskLists: Bool
    /// Recognise formulas — five inline forms and four block forms.
    public let formulas: Bool
    /// Recognise directives — `:name`, `::name` and `:::name` fences.
    public let directives: Bool

    /// Creates an option set. Every parameter defaults to `true`, so
    /// `ParseOptions()` recognises everything this parser knows.
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

/// Why a parse produced no document.
///
/// These are failures of the parse operation itself, not syntax observations.
public enum ParseErrorCode: Int32, Sendable {
    /// The call itself was wrong — a null source, or a length that does not
    /// describe it.
    case invalidArgument = 1
    /// An allocation failed. The parse is abandoned rather than returning a
    /// document with something missing from it.
    case allocationFailed = 2
    /// The parser reached a state it does not otherwise account for.
    case `internal` = 3
}

/// A parse failure, and nothing else.
///
/// It carries no scope: an input the parser could not turn into a document has
/// no document extent to point at.
public struct ParseError: Error, Sendable {
    /// Which failure it was.
    public let code: ParseErrorCode
    /// A fixed English sentence naming the failure. It is for a log, not for
    /// an end user, and it is not localised.
    public let message: String
}

/// The immutable semantic root returned by a parse.
public struct Document: Markup {
    /// The whole document's boundaries. See ``Scope``.
    public let scope: Scope
    /// The document's blocks. Block content, not inline.
    public let content: [any Markup]
    /// Dispatches to the visitor's `Document` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    /// Parses `source` and returns the whole tree as values.
    ///
    /// The native parse is released before this returns, so the result borrows
    /// nothing and is safe to hold, copy and send across isolation boundaries.
    ///
    /// - Parameters:
    ///   - source: the Markdown to parse. It is read as UTF-8.
    ///   - options: which constructs to recognise. Everything, by default.
    /// - Returns: the parsed document.
    /// - Throws: ``ParseError`` when there is no document to return at all.
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

        guard let root = markdown_core_document_root(nativeDocument),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree")
        }
        return NativeTreeBuilder(root: root).document()
    }
}

private struct NativeNodeRecord {
    let node: OpaquePointer
    var children: [Int] = []
    var label: Int?
}

/// Copies the C tree without making Swift's call stack proportional to input
/// depth. A directive label is recorded as its own node-valued field, never as
/// an entry in the directive's content relation.
private struct NativeTreeBuilder {
    private var records: [NativeNodeRecord]

    init(root: OpaquePointer) {
        records = [NativeNodeRecord(node: root)]
        var recordIndex = 0
        while recordIndex < records.count {
            let node = records[recordIndex].node
            let expectedChildren = markdown_core_node_child_count(node)
            records[recordIndex].children.reserveCapacity(expectedChildren)

            var child = markdown_core_node_get_first_child(node)
            while let current = child {
                records[recordIndex].children.append(records.count)
                records.append(NativeNodeRecord(node: current))
                child = markdown_core_node_get_next_sibling(current)
            }
            precondition(
                records[recordIndex].children.count == expectedChildren,
                "native child count does not match its sibling chain"
            )

            switch markdown_core_node_get_kind(node) {
            case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, MARKDOWN_CORE_KIND_DIRECTIVE:
                if let label = markdown_core_node_directive_label(node) {
                    records[recordIndex].label = records.count
                    records.append(NativeNodeRecord(node: label))
                }
            default:
                break
            }
            recordIndex += 1
        }
    }

    func document() -> Document {
        var values: [(any Markup)?] = Array(repeating: nil, count: records.count)
        for index in records.indices.reversed() {
            let record = records[index]
            let children = record.children.map { childIndex -> any Markup in
                guard let child = values[childIndex] else {
                    preconditionFailure("native child was not materialized before its parent")
                }
                return child
            }
            let label: DirectiveLabel?
            if let labelIndex = record.label {
                guard let builtLabel = values[labelIndex] as? DirectiveLabel else {
                    preconditionFailure("native directive label has the wrong kind")
                }
                label = builtLabel
            } else {
                label = nil
            }
            values[index] = markup(from: record.node, children: children, label: label)
        }
        guard let document = values[0] as? Document else {
            preconditionFailure("native tree root is not a document")
        }
        return document
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity
func markup(
    from node: OpaquePointer,
    children: [any Markup],
    label: DirectiveLabel?
) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        Document(scope: Document.scope(from: node), content: children)
    case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node, content: children)
    case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, content: children)
    case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, content: children)
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node)
    case MARKDOWN_CORE_KIND_LIST: List(from: node, children: children)
    case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, content: children)
    case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node)
    case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node)
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node)
    case MARKDOWN_CORE_KIND_TABLE: Table(from: node, children: children)
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        DirectiveBlock(from: node, label: label, content: children)
    case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node, content: children)
    case MARKDOWN_CORE_KIND_TEXT: Text(from: node)
    case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node)
    case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node)
    case MARKDOWN_CORE_KIND_CODE: Code(from: node)
    case MARKDOWN_CORE_KIND_HTML: HTML(from: node)
    case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node)
    case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, content: children)
    case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, content: children)
    case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, content: children)
    case MARKDOWN_CORE_KIND_LINK: Link(from: node, content: children)
    case MARKDOWN_CORE_KIND_IMAGE: Image(from: node, content: children)
    case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, label: label)
    case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node)
    case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, children: children)
    case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, content: children)
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        DirectiveLabel(scope: DirectiveLabel.scope(from: node), content: children)
    case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: ReferenceDefinition(from: node)
    case MARKDOWN_CORE_KIND_LINK_REFERENCE: LinkReference(from: node, content: children)
    case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: ImageReference(from: node, content: children)
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}
