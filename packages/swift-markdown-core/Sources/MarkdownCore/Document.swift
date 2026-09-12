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
    /// The whole document's boundaries. See ``Scope``.
    public let scope: Scope
    /// The explicit anchor, absent when none was attached.
    public let anchor: String?
    /// Ordered classes and records, including duplicates.
    public let attributes: Attributes
    /// The document's blocks. Block content, not inline.
    public let content: [any Markup]
    /// Parsed Properties, absent until their syntax is implemented.
    public let metadata: Metadata?
    /// The footnotes the document owns, ordered by scope start; never part of
    /// `content`.
    public let footnotes: [Footnote]
    /// The specimen definitions, ordered by scope start and visited after footnotes.
    public let specimens: [Specimen]
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

private struct NativeNodeRecord {
    let node: OpaquePointer
    var children: [Int] = []
    var caption: Int?
    var label: Int?
    var title: [Int]?
    var term: [Int] = []
    var bodies: [[Int]] = []
    /// The document's footnotes, each with the records of its content.
    var footnotes: [NativeFootnoteRecord] = []
    var specimens: [NativeSpecimenRecord] = []
    /// The cite's items, each with the records of its prefix and suffix.
    var citations: [NativeCitationRecord] = []
}

private struct NativeFootnoteRecord {
    let footnote: OpaquePointer
    let content: [Int]
}

private struct NativeSpecimenRecord {
    let specimen: OpaquePointer
    let content: [Int]
}

private struct NativeCitationRecord {
    let citation: OpaquePointer
    let prefix: [Int]
    let suffix: [Int]
}

/// The relations a node owns, materialized before the node itself: its
/// children, its node-valued fields, and the scoped values it owns.
struct NativeRelations {
    let children: [any Markup]
    let caption: TableCaption?
    let label: DirectiveLabel?
    let title: [any Markup]?
    let term: [any Markup]
    let bodies: [[any Markup]]
    let footnotes: [Footnote]
    let specimens: [Specimen]
    let citations: [Citation]
}

/// Copies the C tree without making Swift's call stack proportional to input
/// depth. A directive label is recorded as its own node-valued field, never as
/// an entry in the directive's content relation, and the nodes a scoped value
/// owns -- a footnote's content, a citation's affixes -- are recorded like
/// children under the value's owner.
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
            recordOwnedRelations(of: node, at: recordIndex)
            recordIndex += 1
        }
    }

    /// Records every node of a sibling chain a value owns and answers their indices.
    private mutating func recordChain(_ first: OpaquePointer?) -> [Int] {
        var indices: [Int] = []
        var node = first
        while let current = node {
            indices.append(records.count)
            records.append(NativeNodeRecord(node: current))
            node = markdown_core_node_get_next_sibling(current)
        }
        return indices
    }

    private mutating func recordNode(_ node: OpaquePointer?) -> Int? {
        guard let node else { return nil }
        let index = records.count
        records.append(NativeNodeRecord(node: node))
        return index
    }

    /// The relations a kind owns beside its children: a directive's label, a
    /// callout's title, the document's footnotes, and a cite's items.
    private mutating func recordOwnedRelations(of node: OpaquePointer, at recordIndex: Int) {
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_TABLE:
            let caption = recordNode(markdown_core_node_table_caption(node))
            records[recordIndex].caption = caption
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, MARKDOWN_CORE_KIND_DIRECTIVE:
            let label = recordNode(markdown_core_node_directive_label(node))
            records[recordIndex].label = label
        case MARKDOWN_CORE_KIND_CALLOUT:
            // The title is a sibling chain the callout owns beside its
            // content; its nodes are recorded like children, and the
            // record remembers which are the title's.
            if let titleNode = markdown_core_node_callout_title(node) {
                let title = recordChain(titleNode)
                records[recordIndex].title = title
            }
        case MARKDOWN_CORE_KIND_DEFINITION:
            records[recordIndex].term = recordChain(markdown_core_node_definition_term(node))
            var body = markdown_core_node_definition_bodies(node)
            while let current = body {
                let content = recordChain(markdown_core_definition_body_content(current))
                records[recordIndex].bodies.append(content)
                body = markdown_core_definition_body_next(current)
            }
        case MARKDOWN_CORE_KIND_DOCUMENT:
            recordFootnotes(of: node, at: recordIndex)
            recordSpecimens(of: node, at: recordIndex)
        case MARKDOWN_CORE_KIND_CITE:
            recordCitations(of: node, at: recordIndex)
        default:
            break
        }
    }

    /// The footnotes are values the document owns beside its content (M4);
    /// each one's content is recorded like children.
    private mutating func recordFootnotes(of node: OpaquePointer, at recordIndex: Int) {
        var footnote = markdown_core_node_document_footnotes(node)
        while let current = footnote {
            let content = recordChain(markdown_core_footnote_content(current))
            records[recordIndex].footnotes.append(NativeFootnoteRecord(footnote: current, content: content))
            footnote = markdown_core_footnote_next(current)
        }
    }

    private mutating func recordSpecimens(of node: OpaquePointer, at recordIndex: Int) {
        var specimen = markdown_core_node_document_specimens(node)
        while let current = specimen {
            let content = recordChain(markdown_core_specimen_content(current))
            records[recordIndex].specimens.append(NativeSpecimenRecord(specimen: current, content: content))
            specimen = markdown_core_specimen_next(current)
        }
    }

    /// The items are values the cite owns (M4); each one's prefix and suffix
    /// are recorded like children.
    private mutating func recordCitations(of node: OpaquePointer, at recordIndex: Int) {
        var citation = markdown_core_node_cite_citations(node)
        while let current = citation {
            let prefix = recordChain(markdown_core_citation_prefix(current))
            let suffix = recordChain(markdown_core_citation_suffix(current))
            records[recordIndex].citations.append(
                NativeCitationRecord(citation: current, prefix: prefix, suffix: suffix)
            )
            citation = markdown_core_citation_next(current)
        }
        precondition(!records[recordIndex].citations.isEmpty, "native cite holds no citation")
    }

    private func field<Value: Markup>(_ index: Int?, in values: [(any Markup)?], as type: Value.Type) -> Value? {
        index.map {
            guard let value = values[$0] as? Value else {
                preconditionFailure("native owned field has the wrong kind")
            }
            return value
        }
    }

    func document() -> Document {
        var values: [(any Markup)?] = Array(repeating: nil, count: records.count)
        // Every occurrence of one reference definition shares one resource in
        // the C tree; this materializes each distinct one once.
        var resources: [UnsafeRawPointer: SharedResource] = [:]
        func nodes(_ indices: [Int], _ what: String) -> [any Markup] {
            indices.map { nodeIndex -> any Markup in
                guard let node = values[nodeIndex] else {
                    preconditionFailure("native \(what) was not materialized before its owner")
                }
                return node
            }
        }
        for index in records.indices.reversed() {
            let record = records[index]
            let relations = NativeRelations(
                children: nodes(record.children, "child"),
                caption: field(record.caption, in: values, as: TableCaption.self),
                label: field(record.label, in: values, as: DirectiveLabel.self),
                title: record.title.map { nodes($0, "callout title") },
                term: nodes(record.term, "definition term"),
                bodies: record.bodies.map { nodes($0, "definition body") },
                footnotes: record.footnotes.map { footnote in
                    Footnote(from: footnote.footnote, content: nodes(footnote.content, "footnote content"))
                },
                specimens: record.specimens.map { specimen in
                    Specimen(from: specimen.specimen, content: nodes(specimen.content, "specimen content"))
                },
                citations: record.citations.map { citation in
                    Citation(
                        from: citation.citation,
                        prefix: nodes(citation.prefix, "citation prefix"),
                        suffix: nodes(citation.suffix, "citation suffix")
                    )
                }
            )
            values[index] = markup(from: record.node, relations: relations, resources: &resources)
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
    relations: NativeRelations,
    resources: inout [UnsafeRawPointer: SharedResource]
) -> any Markup {
    switch markdown_core_node_get_kind(node) {
    case MARKDOWN_CORE_KIND_DOCUMENT:
        Document(
            from: node,
            content: relations.children,
            footnotes: relations.footnotes,
            specimens: relations.specimens
        )
    case MARKDOWN_CORE_KIND_CALLOUT: Callout(from: node, title: relations.title, content: relations.children)
    case MARKDOWN_CORE_KIND_DEFINITION_LIST: DefinitionList(from: node, children: relations.children)
    case MARKDOWN_CORE_KIND_DEFINITION: Definition(from: node, term: relations.term, content: relations.bodies)
    case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node)
    case MARKDOWN_CORE_KIND_LIST: List(from: node, children: relations.children)
    case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node)
    case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node)
    case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node)
    case MARKDOWN_CORE_KIND_TABLE: Table(from: node, caption: relations.caption, children: relations.children)
    case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
        DirectiveBlock(from: node, label: relations.label, content: relations.children)
    case MARKDOWN_CORE_KIND_TEXT: Text(from: node)
    case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node)
    case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node)
    case MARKDOWN_CORE_KIND_CODE: Code(from: node)
    case MARKDOWN_CORE_KIND_HTML: HTML(from: node)
    case MARKDOWN_CORE_KIND_COMMENT: Comment(from: node)
    case MARKDOWN_CORE_KIND_CROSS_LINK: CrossLink(from: node)
    case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: CrossEmbedded(from: node)
    case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node)
    case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_MARK: Mark(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_INSERTION: Insertion(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_SPAN: Span(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_SUPERSCRIPT: Superscript(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_SUBSCRIPT: Subscript(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_LINK: Link(from: node, content: relations.children, resources: &resources)
    case MARKDOWN_CORE_KIND_MEDIA: Media(from: node, content: relations.children, resources: &resources)
    case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, label: relations.label)
    case MARKDOWN_CORE_KIND_CITE: Cite(from: node, citations: relations.citations)
    case MARKDOWN_CORE_KIND_TABLE_CAPTION: TableCaption(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, children: relations.children)
    case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, content: relations.children)
    case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL: DirectiveLabel(from: node, content: relations.children)
    default: preconditionFailure("native parser returned an unknown node kind")
    }
}

extension Document {
    init(from node: OpaquePointer, content: [any Markup], footnotes: [Footnote], specimens: [Specimen]) {
        self.init(
            scope: Self.scope(from: node),
            anchor: markdown_core_node_anchor(node).string,
            attributes: Attributes(from: node),
            content: content,
            metadata: markdown_core_node_document_metadata(node).map { Metadata(from: $0) },
            footnotes: footnotes,
            specimens: specimens
        )
    }
}
