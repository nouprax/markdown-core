import MarkdownCoreC

/// Copies the native tree into records once, with no recursion: every native
/// node is queued in breadth-first order, so each node's children follow it,
/// and records are built from the last queued node back to the root. No native
/// pointer survives the copy.
struct DocumentBuilder {
    /// Queue positions of one native node's owned relations.
    private struct Relations {
        var children: [Int] = []
        var metadata: Int?
        var caption: Int?
        var label: Int?
        var title: [Int] = []
        var term: [Int] = []
        var bodies: [[Int]] = []
        var citations: [Int] = []
        var note: Int?
        var prefix: [Int] = []
        var suffix: [Int] = []
    }

    private let document: OpaquePointer
    private let unit: TextUnit
    private var queue: [OpaquePointer] = []
    private var relations: [Relations] = []
    private var records: [MarkupRecord?] = []
    private var definitions: [OpaquePointer: MarkupRecord] = [:]
    private var resources: [Int: SharedResource] = [:]

    init(document: OpaquePointer, root: OpaquePointer, unit: TextUnit) {
        self.document = document
        self.unit = unit
        queue = [root]
        var index = 0
        while index < queue.count {
            let owned = scan(queue[index])
            relations.append(owned)
            index += 1
        }
    }

    mutating func build() -> DocumentRecord {
        records = Array(repeating: nil, count: queue.count)
        for index in queue.indices.reversed() {
            let built = record(from: queue[index], relations: relations[index])
            let kind = markdown_core_node_get_kind(queue[index])
            if kind == MARKDOWN_CORE_KIND_FOOTNOTE || kind == MARKDOWN_CORE_KIND_SPECIMEN {
                definitions[queue[index]] = built
            }
            records[index] = built
        }
        return unsafeDowncast(take(0), to: DocumentRecord.self)
    }

    // Enumerate each facade-owned relation alongside its native kind.
    private mutating func scan(_ node: OpaquePointer) -> Relations {
        var relations = Relations()
        relations.children = enqueue(chain: markdown_core_node_get_first_child(node))
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_TABLE:
            relations.caption = enqueue(field: answer { markdown_core_node_table_caption(node, $0) })
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK, MARKDOWN_CORE_KIND_DIRECTIVE:
            relations.label = enqueue(field: answer { markdown_core_node_directive_label(node, $0) })
        case MARKDOWN_CORE_KIND_CALLOUT:
            relations.title = enqueue(chain: answer { markdown_core_node_callout_title(node, $0) })
        case MARKDOWN_CORE_KIND_DEFINITION:
            relations.term = enqueue(chain: answer { markdown_core_node_definition_term(node, $0) })
            var body = answer { markdown_core_node_definition_bodies(node, $0) }
            while let current = body {
                relations.bodies.append(enqueue(chain: markdown_core_definition_body_content(current)))
                body = markdown_core_definition_body_next(current)
            }
        case MARKDOWN_CORE_KIND_DOCUMENT:
            relations.metadata = enqueue(field: answer { markdown_core_node_document_metadata(node, $0) })
        case MARKDOWN_CORE_KIND_CITE:
            relations.citations = enqueue(chain: answer { markdown_core_node_cite_citations(node, $0) })
        case MARKDOWN_CORE_KIND_CITATION:
            let referent = answer(markdown_core_referent()) { markdown_core_citation_referent(node, $0) }
            relations.note = enqueue(field: referent.note)
            relations.prefix = enqueue(chain: answer { markdown_core_citation_prefix(node, $0) })
            relations.suffix = enqueue(chain: answer { markdown_core_citation_suffix(node, $0) })
        default:
            break
        }
        return relations
    }

    private mutating func enqueue(_ value: OpaquePointer) -> Int {
        let index = queue.count
        queue.append(value)
        return index
    }

    private mutating func enqueue(field node: OpaquePointer?) -> Int? {
        node.map { enqueue($0) }
    }

    private mutating func enqueue(chain first: OpaquePointer?) -> [Int] {
        var indices: [Int] = []
        var node = first
        while let current = node {
            indices.append(enqueue(current))
            node = markdown_core_node_get_next_sibling(current)
        }
        return indices
    }

    /// Moves a built record out of the queue, so its parent alone owns it.
    private mutating func take(_ index: Int) -> MarkupRecord {
        let record = records[index]
        records[index] = nil
        // swift-format-ignore: NeverForceUnwrap
        return record!
    }

    private mutating func take(_ indices: [Int]) -> [MarkupRecord] {
        indices.map { take($0) }
    }

    private mutating func take<Node: MarkupRecord>(_ index: Int?, as _: Node.Type) -> Node? {
        index.map { unsafeDowncast(take($0), to: Node.self) }
    }

    /// The definitions the document's table names, in its order.
    private func table<Node: MarkupRecord>(
        count: Int,
        at entry: (Int) -> OpaquePointer?,
        as _: Node.Type
    ) -> [Node] {
        // Every index is below the count, so each entry is a definition built
        // from the tree.
        // swift-format-ignore: NeverForceUnwrap
        (0..<count).map { unsafeDowncast(definitions[entry($0)!]!, to: Node.self) }
    }
}

extension DocumentBuilder {
    // Keep the exhaustive native-kind switch in one place so a newly added native
    // kind cannot silently bypass value-tree copying.
    // swiftlint:disable:next cyclomatic_complexity function_body_length
    private mutating func record(from node: OpaquePointer, relations: Relations) -> MarkupRecord {
        let children = take(relations.children)
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_DOCUMENT:
            let document = document
            return DocumentRecord(
                InheritedFields(from: node),
                unit: unit,
                metadata: take(relations.metadata, as: MetadataRecord.self),
                content: children,
                footnotes: table(
                    count: markdown_core_document_footnote_count(document),
                    at: { index in answer { markdown_core_document_footnote_at(document, index, $0) } },
                    as: FootnoteRecord.self
                ),
                specimens: table(
                    count: markdown_core_document_specimen_count(document),
                    at: { index in answer { markdown_core_document_specimen_at(document, index, $0) } },
                    as: SpecimenRecord.self
                )
            )
        case MARKDOWN_CORE_KIND_CITATION:
            return CitationRecord(
                from: node,
                note: take(relations.note, as: FootnoteRecord.self),
                prefix: take(relations.prefix),
                suffix: take(relations.suffix)
            )
        case MARKDOWN_CORE_KIND_FOOTNOTE: return FootnoteRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SPECIMEN: return SpecimenRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_METADATA: return MetadataRecord(from: node)
        case MARKDOWN_CORE_KIND_CALLOUT:
            return CalloutRecord(from: node, title: take(relations.title), content: children)
        case MARKDOWN_CORE_KIND_DEFINITION_LIST: return DefinitionListRecord(from: node, definitions: children)
        case MARKDOWN_CORE_KIND_DEFINITION:
            let term = take(relations.term)
            let bodies = relations.bodies.map { body in body.map { take($0) } }
            return DefinitionRecord(from: node, term: term, bodies: bodies)
        case MARKDOWN_CORE_KIND_PARAGRAPH: return ParagraphRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_HEADING: return HeadingRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: return ThematicBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_LIST: return ListRecord(from: node, items: children)
        case MARKDOWN_CORE_KIND_LIST_ITEM: return ListItemRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_CODE_BLOCK: return CodeBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_HTML_BLOCK: return HTMLBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: return FormulaBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_TABLE:
            let caption = take(relations.caption, as: TableCaptionRecord.self)
            return TableRecord(from: node, caption: caption, rows: children)
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
            return DirectiveBlockRecord(
                from: node,
                label: take(relations.label, as: DirectiveLabelRecord.self),
                content: children
            )
        case MARKDOWN_CORE_KIND_TEXT: return TextRecord(from: node)
        case MARKDOWN_CORE_KIND_SOFT_BREAK: return SoftBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_LINE_BREAK: return LineBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_CODE: return CodeRecord(from: node)
        case MARKDOWN_CORE_KIND_HTML: return HTMLRecord(from: node)
        case MARKDOWN_CORE_KIND_COMMENT: return CommentRecord(from: node)
        case MARKDOWN_CORE_KIND_CROSS_LINK: return CrossLinkRecord(from: node)
        case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: return CrossEmbeddedRecord(from: node)
        case MARKDOWN_CORE_KIND_FORMULA: return FormulaRecord(from: node)
        case MARKDOWN_CORE_KIND_EMPHASIS: return EmphasisRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_STRONG: return StrongRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_STRIKETHROUGH: return StrikethroughRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_MARK: return MarkRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_INSERTION: return InsertionRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SPAN: return SpanRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SUPERSCRIPT: return SuperscriptRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SUBSCRIPT: return SubscriptRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_LINK: return LinkRecord(from: node, content: children, resources: &resources)
        case MARKDOWN_CORE_KIND_EMBEDDED: return EmbeddedRecord(from: node, content: children, resources: &resources)
        case MARKDOWN_CORE_KIND_DIRECTIVE:
            return DirectiveRecord(from: node, label: take(relations.label, as: DirectiveLabelRecord.self))
        case MARKDOWN_CORE_KIND_CITE: return CiteRecord(from: node, citations: take(relations.citations))
        case MARKDOWN_CORE_KIND_TABLE_CAPTION: return TableCaptionRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_TABLE_ROW: return TableRowRecord(from: node, cells: children)
        case MARKDOWN_CORE_KIND_TABLE_CELL: return TableCellRecord(from: node, content: children)
        // A C enum switch is never exhaustive in Swift; the one kind
        // left is MARKDOWN_CORE_KIND_DIRECTIVE_LABEL.
        default: return DirectiveLabelRecord(from: node, content: children)
        }
    }
}
