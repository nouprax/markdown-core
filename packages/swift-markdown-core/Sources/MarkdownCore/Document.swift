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
        let content: MarkupReferences<any Markup>
        let metadata: MarkupReference<Metadata>?
        let footnotes: MarkupReferences<Footnote>
        let specimens: MarkupReferences<Specimen>
    }

    let fields: Stored<Fields>

    /// The whole document's boundaries. See ``Scope``.
    public var scope: Scope { fields.read { $0.scope } }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { fields.read { $0.anchor } }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { fields.read { $0.attributes } }
    /// The document's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> { fields.children { $0.content } }
    /// The parsed Properties node, absent when no Properties block was authored.
    public var metadata: Metadata? { fields.optionalElement { $0.metadata } }
    /// The footnotes the document owns, ordered by scope start; never part of
    /// `content`.
    public var footnotes: MarkupCollection<Footnote> { fields.elements { $0.footnotes } }
    /// The specimen definitions, ordered by scope start and visited after footnotes.
    public var specimens: MarkupCollection<Specimen> { fields.elements { $0.specimens } }

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
        var error: OpaquePointer?
        // A native String already holds contiguous UTF-8: the parser reads it
        // in place, so the source crosses the boundary without a copy of its
        // own. A bridged or non-contiguous string is made contiguous once.
        var bytes = source
        let document = bytes.withUTF8 { buffer in
            markdown_core_document_parse(buffer.baseAddress, buffer.count, &error)
        }
        guard let document else {
            defer { markdown_core_error_free(error) }
            throw ParseError(from: error)
        }
        defer { markdown_core_document_free(document) }

        guard let root = markdown_core_document_root(document),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree")
        }
        return DocumentBuilder(root: root).document()
    }
}

/// Copies scalars and indexed relations once. No stored Swift record owns
/// another record, and no native pointer survives the copy.
private struct DocumentBuilder {
    /// Source-order indices recorded while copying one native value.
    private struct Relations {
        var metadata: Int?
        var children: [Int] = []
        var caption: Int?
        var label: Int?
        var title: [Int]?
        var term: [Int] = []
        var bodies: [[Int]] = []
        var footnotes: [Int] = []
        var specimens: [Int] = []
        var citations: [Int] = []
        var prefix: [Int] = []
        var suffix: [Int] = []
    }

    private var pending: [OpaquePointer] = []
    private var stored: [StoredMarkup] = []
    private var resources: [UnsafeRawPointer: SharedResource] = [:]

    init(root: OpaquePointer) {
        pending = [root]
        var index = 0
        while index < pending.count {
            let record = copy(pending[index])
            stored.append(record)
            index += 1
        }
    }

    private mutating func copy(_ node: OpaquePointer) -> StoredMarkup {
        // The kind is read once per node and handed on: the relations it has
        // and the value it becomes are both decided by it.
        let kind = markdown_core_node_get_kind(node)
        let relations = record(relations: node, kind: kind)
        return Self.stored(from: node, kind: kind, relations: relations, resources: &resources)
    }

    // Enumerate each facade-owned relation alongside its native kind. The
    // child chain is walked once, here; nothing counts it again.
    // swiftlint:disable:next cyclomatic_complexity
    private mutating func record(relations node: OpaquePointer, kind: markdown_core_node_kind) -> Relations {
        var relations = Relations()
        relations.children = record(chain: markdown_core_node_get_first_child(node))
        switch kind {
        case MARKDOWN_CORE_KIND_TABLE:
            relations.caption = record(field: markdown_core_node_table_caption(node))
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, MARKDOWN_CORE_KIND_DIRECTIVE:
            relations.label = record(field: markdown_core_node_directive_label(node))
        case MARKDOWN_CORE_KIND_CALLOUT:
            if let title = markdown_core_node_callout_title(node) {
                relations.title = record(chain: title)
            }
        case MARKDOWN_CORE_KIND_DEFINITION:
            relations.term = record(chain: markdown_core_node_definition_term(node))
            var body = markdown_core_node_definition_bodies(node)
            while let current = body {
                relations.bodies.append(record(chain: markdown_core_definition_body_content(current)))
                body = markdown_core_definition_body_next(current)
            }
        case MARKDOWN_CORE_KIND_DOCUMENT:
            relations.metadata = record(field: markdown_core_node_document_metadata(node))
            var note = markdown_core_node_document_footnotes(node)
            while let current = note {
                relations.footnotes.append(enqueue(current))
                note = markdown_core_node_get_next_sibling(current)
            }
            var specimen = markdown_core_node_document_specimens(node)
            while let current = specimen {
                relations.specimens.append(enqueue(current))
                specimen = markdown_core_node_get_next_sibling(current)
            }
        case MARKDOWN_CORE_KIND_CITE:
            var citation = markdown_core_node_cite_citations(node)
            while let current = citation {
                relations.citations.append(enqueue(current))
                citation = markdown_core_node_get_next_sibling(current)
            }
            precondition(!relations.citations.isEmpty)
        case MARKDOWN_CORE_KIND_CITATION:
            relations.prefix = record(chain: markdown_core_citation_prefix(node))
            relations.suffix = record(chain: markdown_core_citation_suffix(node))
        default:
            break
        }
        return relations
    }

    private mutating func enqueue(_ value: OpaquePointer) -> Int {
        let index = pending.count
        pending.append(value)
        return index
    }

    private mutating func record(field node: OpaquePointer?) -> Int? {
        node.map { enqueue($0) }
    }

    private mutating func record(chain first: OpaquePointer?) -> [Int] {
        var indices: [Int] = []
        var node = first
        while let current = node {
            indices.append(enqueue(current))
            node = markdown_core_node_get_next_sibling(current)
        }
        return indices
    }

    func document() -> Document {
        Document.stored(at: 0, in: MarkupStore(records: stored))
    }
}

extension DocumentBuilder {
    // Keep the exhaustive native-kind switch in one place so a newly added native
    // kind cannot silently bypass value-tree copying.
    // swiftlint:disable:next cyclomatic_complexity function_body_length
    private static func stored(
        from node: OpaquePointer,
        kind: markdown_core_node_kind,
        relations: Relations,
        resources: inout [UnsafeRawPointer: SharedResource]
    ) -> StoredMarkup {
        switch kind {
        case MARKDOWN_CORE_KIND_DOCUMENT:
            .document(
                Document.Fields(
                    from: node,
                    content: relations.children,
                    metadata: relations.metadata,
                    footnotes: relations.footnotes,
                    specimens: relations.specimens
                )
            )
        case MARKDOWN_CORE_KIND_CITATION:
            .citation(Citation.Fields(from: node, prefix: relations.prefix, suffix: relations.suffix))
        case MARKDOWN_CORE_KIND_FOOTNOTE:
            .footnote(Footnote.Fields(from: node, content: relations.children))
        case MARKDOWN_CORE_KIND_SPECIMEN:
            .specimen(Specimen.Fields(from: node, content: relations.children))
        case MARKDOWN_CORE_KIND_METADATA:
            .metadata(Metadata(from: node))
        case MARKDOWN_CORE_KIND_CALLOUT:
            .callout(Callout.Fields(from: node, title: relations.title, content: relations.children))
        case MARKDOWN_CORE_KIND_DEFINITION_LIST:
            .definitionList(DefinitionList.Fields(from: node, children: relations.children))
        case MARKDOWN_CORE_KIND_DEFINITION:
            .definition(Definition.Fields(from: node, term: relations.term, content: relations.bodies))
        case MARKDOWN_CORE_KIND_PARAGRAPH: .paragraph(Paragraph.Fields(from: node, content: relations.children))
        case MARKDOWN_CORE_KIND_HEADING: .heading(Heading.Fields(from: node, content: relations.children))
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: .thematicBreak(ThematicBreak.Fields(from: node))
        case MARKDOWN_CORE_KIND_LIST: .list(List.Fields(from: node, children: relations.children))
        case MARKDOWN_CORE_KIND_LIST_ITEM: .listItem(ListItem.Fields(from: node, content: relations.children))
        case MARKDOWN_CORE_KIND_CODE_BLOCK: .codeBlock(CodeBlock.Fields(from: node))
        case MARKDOWN_CORE_KIND_HTML_BLOCK: .htmlBlock(HTMLBlock.Fields(from: node))
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: .formulaBlock(FormulaBlock.Fields(from: node))
        case MARKDOWN_CORE_KIND_TABLE:
            .table(Table.Fields(from: node, caption: relations.caption, children: relations.children))
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
            .directiveBlock(DirectiveBlock.Fields(from: node, label: relations.label, content: relations.children))
        case MARKDOWN_CORE_KIND_TEXT: .text(Text.Fields(from: node))
        case MARKDOWN_CORE_KIND_SOFT_BREAK: .softBreak(SoftBreak.Fields(from: node))
        case MARKDOWN_CORE_KIND_LINE_BREAK: .lineBreak(LineBreak.Fields(from: node))
        case MARKDOWN_CORE_KIND_CODE: .code(Code.Fields(from: node))
        case MARKDOWN_CORE_KIND_HTML: .html(HTML.Fields(from: node))
        case MARKDOWN_CORE_KIND_COMMENT: .comment(Comment.Fields(from: node))
        case MARKDOWN_CORE_KIND_CROSS_LINK: .crossLink(CrossLink.Fields(from: node))
        case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: .crossEmbedded(CrossEmbedded.Fields(from: node))
        case MARKDOWN_CORE_KIND_FORMULA: .formula(Formula.Fields(from: node))
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
        case MARKDOWN_CORE_KIND_EMBEDDED:
            .embedded(Embedded.Fields(from: node, content: relations.children, resources: &resources))
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
}

extension Document.Fields {
    init(from node: OpaquePointer, content: [Int], metadata: Int?, footnotes: [Int], specimens: [Int]) {
        self.init(
            scope: Scope(from: markdown_core_node_scope(node)),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: .init(indices: content),
            metadata: metadata.map { .init(index: $0) },
            footnotes: .init(indices: footnotes),
            specimens: .init(indices: specimens)
        )
    }
}
