import MarkdownCoreC

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
    struct Fields: Sendable {
        let scope: Scope
        let anchor: String?
        let attributes: Attributes
        let content: [Int]
        let metadata: Metadata?
        let footnotes: [Int]
        let specimens: [Int]
    }

    @Stored var fields: Fields

    /// The whole document's boundaries. See ``Scope``.
    public var scope: Scope { fields.scope }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.attributes }
    /// The document's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { $fields.collection(fields.content) }
    /// Parsed Properties, absent until their syntax is implemented.
    public var metadata: Metadata? { fields.metadata }
    /// The footnotes the document owns, ordered by scope start; never part of
    /// `content`.
    public var footnotes: MarkupCollection<Footnote> { $fields.collection(fields.footnotes) }
    /// The specimen definitions, ordered by scope start and visited after footnotes.
    public var specimens: MarkupCollection<Specimen> { $fields.collection(fields.specimens) }
    /// Dispatches to the visitor's `Document` case.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    /// Parses `source` and returns the whole tree as values.
    ///
    /// There is one language and nothing to configure: every feature of the
    /// Markdown Core dialect is recognised on every call. The native parse is
    /// released before this returns, so the result borrows nothing and is safe
    /// to hold, copy and send across isolation boundaries.
    ///
    /// - Parameter source: the Markdown to parse. It is read as UTF-8.
    /// - Returns: the parsed document.
    /// - Throws: ``ParseError`` when there is no document to return at all.
    public static func parse(_ source: String) throws -> Document {
        var nativeError: OpaquePointer?
        let bytes = Array(source.utf8)
        let nativeDocument = bytes.withUnsafeBufferPointer { buffer in
            markdown_core_document_parse(buffer.baseAddress, buffer.count, &nativeError)
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

/// Source-order indices recorded while copying one native value.
private struct NativeRelations {
    var children: [Int] = []
    var caption: Int?
    var label: Int?
    var title: [Int]?
    var term: [Int] = []
    var bodies: [[Int]] = []
    var footnotes: [Int] = []
    var specimens: [Int] = []
    var citations: [Int] = []
}

/// Each queued handle has the facade type needed to read its owned relations.
private enum NativeValue {
    case markup(OpaquePointer)
    case footnote(OpaquePointer)
    case specimen(OpaquePointer)
    case citation(OpaquePointer)
}

/// Copies scalars and indexed relations once. No stored Swift record owns
/// another record, and no native pointer survives the copy.
private struct NativeTreeBuilder {
    private var pending: [NativeValue] = []
    private var records: [StoredMarkup] = []
    private var resources: [UnsafeRawPointer: SharedResource] = [:]

    init(root: OpaquePointer) {
        pending = [.markup(root)]
        var index = 0
        while index < pending.count {
            let record = copy(pending[index])
            records.append(record)
            index += 1
        }
    }

    private mutating func copy(_ value: NativeValue) -> StoredMarkup {
        switch value {
        case let .markup(node):
            let relations = markupRelations(node)
            return stored(from: node, relations: relations, resources: &resources)
        case let .footnote(node):
            return .footnote(
                Footnote.Fields(from: node, content: chain(markdown_core_footnote_content(node)))
            )
        case let .specimen(node):
            return .specimen(
                Specimen.Fields(from: node, content: chain(markdown_core_specimen_content(node)))
            )
        case let .citation(node):
            let prefix = chain(markdown_core_citation_prefix(node))
            let suffix = chain(markdown_core_citation_suffix(node))
            return .citation(Citation.Fields(from: node, prefix: prefix, suffix: suffix))
        }
    }

    // Enumerate each facade-owned relation alongside its native kind.
    // swiftlint:disable:next cyclomatic_complexity
    private mutating func markupRelations(_ node: OpaquePointer) -> NativeRelations {
        var relations = NativeRelations()
        relations.children = chain(markdown_core_node_get_first_child(node))
        precondition(relations.children.count == markdown_core_node_child_count(node))
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_TABLE:
            relations.caption = field(markdown_core_node_table_caption(node))
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, MARKDOWN_CORE_KIND_DIRECTIVE:
            relations.label = field(markdown_core_node_directive_label(node))
        case MARKDOWN_CORE_KIND_CALLOUT:
            if let title = markdown_core_node_callout_title(node) {
                relations.title = chain(title)
            }
        case MARKDOWN_CORE_KIND_DEFINITION:
            relations.term = chain(markdown_core_node_definition_term(node))
            var body = markdown_core_node_definition_bodies(node)
            while let current = body {
                relations.bodies.append(chain(markdown_core_definition_body_content(current)))
                body = markdown_core_definition_body_next(current)
            }
        case MARKDOWN_CORE_KIND_DOCUMENT:
            var note = markdown_core_node_document_footnotes(node)
            while let current = note {
                relations.footnotes.append(enqueue(.footnote(current)))
                note = markdown_core_footnote_next(current)
            }
            var specimen = markdown_core_node_document_specimens(node)
            while let current = specimen {
                relations.specimens.append(enqueue(.specimen(current)))
                specimen = markdown_core_specimen_next(current)
            }
        case MARKDOWN_CORE_KIND_CITE:
            var citation = markdown_core_node_cite_citations(node)
            while let current = citation {
                relations.citations.append(enqueue(.citation(current)))
                citation = markdown_core_citation_next(current)
            }
            precondition(!relations.citations.isEmpty)
        default:
            break
        }
        return relations
    }

    private mutating func enqueue(_ value: NativeValue) -> Int {
        let index = pending.count
        pending.append(value)
        return index
    }

    private mutating func field(_ node: OpaquePointer?) -> Int? {
        node.map { enqueue(.markup($0)) }
    }

    private mutating func chain(_ first: OpaquePointer?) -> [Int] {
        var indices: [Int] = []
        var node = first
        while let current = node {
            indices.append(enqueue(.markup(current)))
            node = markdown_core_node_get_next_sibling(current)
        }
        return indices
    }

    func document() -> Document {
        MarkupStore(records: records).value(at: 0, as: Document.self)
    }
}

// Keep the exhaustive native-kind switch in one place so a newly added native
// kind cannot silently bypass value-tree copying.
// swiftlint:disable:next cyclomatic_complexity function_body_length
private func stored(
    from node: OpaquePointer,
    relations: NativeRelations,
    resources: inout [UnsafeRawPointer: SharedResource]
) -> StoredMarkup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        .document(
            Document.Fields(
                from: node,
                content: relations.children,
                footnotes: relations.footnotes,
                specimens: relations.specimens
            )
        )
    case MARKDOWN_CORE_KIND_CALLOUT:
        .callout(Callout.Fields(from: node, title: relations.title, content: relations.children))
    case MARKDOWN_CORE_KIND_DEFINITION_LIST:
        .definitionList(DefinitionList.Fields(from: node, children: relations.children))
    case MARKDOWN_CORE_KIND_DEFINITION:
        .definition(Definition.Fields(from: node, term: relations.term, content: relations.bodies))
    case MARKDOWN_CORE_KIND_PARAGRAPH: .paragraph(Paragraph.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_HEADING: .heading(Heading.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK: .thematicBreak(ThematicBreak(from: node))
    case MARKDOWN_CORE_KIND_LIST: .list(List.Fields(from: node, children: relations.children))
    case MARKDOWN_CORE_KIND_LIST_ITEM: .listItem(ListItem.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_CODE_BLOCK: .codeBlock(CodeBlock(from: node))
    case MARKDOWN_CORE_KIND_HTML_BLOCK: .htmlBlock(HTMLBlock(from: node))
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: .formulaBlock(FormulaBlock(from: node))
    case MARKDOWN_CORE_KIND_TABLE:
        .table(Table.Fields(from: node, caption: relations.caption, children: relations.children))
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        .directiveBlock(DirectiveBlock.Fields(from: node, label: relations.label, content: relations.children))
    case MARKDOWN_CORE_KIND_TEXT: .text(Text(from: node))
    case MARKDOWN_CORE_KIND_SOFT_BREAK: .softBreak(SoftBreak(from: node))
    case MARKDOWN_CORE_KIND_LINE_BREAK: .lineBreak(LineBreak(from: node))
    case MARKDOWN_CORE_KIND_CODE: .code(Code(from: node))
    case MARKDOWN_CORE_KIND_HTML: .html(HTML(from: node))
    case MARKDOWN_CORE_KIND_COMMENT: .comment(Comment(from: node))
    case MARKDOWN_CORE_KIND_CROSS_LINK: .crossLink(CrossLink(from: node))
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: .crossEmbedded(CrossEmbedded(from: node))
    case MARKDOWN_CORE_KIND_FORMULA: .formula(Formula(from: node))
    case MARKDOWN_CORE_KIND_EMPHASIS: .emphasis(Emphasis.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_STRONG: .strong(Strong.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_STRIKETHROUGH:
        .strikethrough(Strikethrough.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_MARK: .mark(Mark.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_INSERTION: .insertion(Insertion.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_SPAN: .span(Span.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_SUPERSCRIPT: .superscript(Superscript.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_SUBSCRIPT: .subscript(Subscript.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_LINK:
        .link(Link.Fields(from: node, content: relations.children, resources: &resources))
    case MARKDOWN_CORE_KIND_MEDIA:
        .media(Media.Fields(from: node, content: relations.children, resources: &resources))
    case MARKDOWN_CORE_KIND_DIRECTIVE: .directive(Directive.Fields(from: node, label: relations.label))
    case MARKDOWN_CORE_KIND_CITE: .cite(Cite.Fields(from: node, citations: relations.citations))
    case MARKDOWN_CORE_KIND_TABLE_CAPTION:
        .tableCaption(TableCaption.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_TABLE_ROW: .tableRow(TableRow.Fields(from: node, children: relations.children))
    case MARKDOWN_CORE_KIND_TABLE_CELL: .tableCell(TableCell.Fields(from: node, content: relations.children))
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL:
        .directiveLabel(DirectiveLabel.Fields(from: node, content: relations.children))
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}

extension Document.Fields {
    init(from node: OpaquePointer, content: [Int], footnotes: [Int], specimens: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            metadata: markdown_core_node_document_metadata(node).map { Metadata(from: $0) },
            footnotes: footnotes,
            specimens: specimens
        )
    }
}
